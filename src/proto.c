/* proto.c - 封包編解碼、CRC32 與帶期限的 socket I/O */
#include "netproto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>

static int g_crc_check = 1;

void proto_set_crc_check(int enabled)
{
    g_crc_check = enabled;
}

uint32_t proto_crc32(uint32_t seed, const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = ~seed;

    while (len--) {
        crc ^= *p++;
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* deadline_ms 為 0 表示不限時；被 signal 打斷時用剩餘時間重新等待 */
static int wait_readable(int fd, int64_t deadline_ms)
{
    if (fd >= FD_SETSIZE)
        return PROTO_ERR_IO;

    for (;;) {
        fd_set rfds;
        struct timeval tv, *tvp = NULL;

        if (deadline_ms > 0) {
            int64_t left = deadline_ms - now_ms();

            if (left <= 0)
                return PROTO_ERR_TIMEOUT;
            tv.tv_sec = left / 1000;
            tv.tv_usec = (left % 1000) * 1000;
            tvp = &tv;
        }

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        int n = select(fd + 1, &rfds, NULL, NULL, tvp);
        if (n > 0)
            return PROTO_OK;
        if (n == 0)
            return PROTO_ERR_TIMEOUT;
        if (errno != EINTR)
            return PROTO_ERR_IO;
    }
}

static int recv_full(int fd, void *buf, size_t len, int64_t deadline_ms)
{
    uint8_t *p = buf;
    size_t got = 0;

    while (got < len) {
        int rc = wait_readable(fd, deadline_ms);
        if (rc != PROTO_OK)
            return rc;

        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n == 0)
            return got == 0 ? PROTO_ERR_CLOSED : PROTO_ERR_TRUNCATED;
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return PROTO_ERR_IO;
        }
        got += (size_t)n;
    }
    return PROTO_OK;
}

static int send_frame(int fd, uint8_t opcode, const void *payload,
                      uint32_t length, int corrupt)
{
    uint8_t frame[PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD];
    uint16_t magic = htons(PROTO_MAGIC);
    uint32_t net_len = htonl(length);
    uint32_t crc, net_crc;

    if (length > PROTO_MAX_PAYLOAD)
        return PROTO_ERR_LENGTH;

    /* 逐欄位寫入，不依賴 struct 的記憶體配置 */
    memcpy(frame, &magic, 2);
    frame[2] = PROTO_VERSION;
    frame[3] = opcode;
    memcpy(frame + 4, &net_len, 4);
    if (length > 0)
        memcpy(frame + PROTO_HEADER_SIZE, payload, length);

    crc = proto_crc32(0, frame, 8);
    crc = proto_crc32(crc, frame + PROTO_HEADER_SIZE, length);
    net_crc = htonl(crc);
    memcpy(frame + 8, &net_crc, 4);

    if (corrupt && length > 0)
        frame[PROTO_HEADER_SIZE] ^= 0x20;

    LOG_DEBUG("tx op=0x%02x len=%u crc=%08x", opcode, length, crc);

    size_t total = PROTO_HEADER_SIZE + (size_t)length;
    size_t sent = 0;

    while (sent < total) {
        /* MSG_NOSIGNAL：對方已關閉時回傳錯誤而不是收到 SIGPIPE */
        ssize_t n = send(fd, frame + sent, total - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return PROTO_ERR_IO;
        }
        sent += (size_t)n;
    }
    return PROTO_OK;
}

int proto_send(int fd, uint8_t opcode, const void *payload, uint32_t length)
{
    return send_frame(fd, opcode, payload, length, 0);
}

int proto_send_corrupted(int fd, uint8_t opcode, const void *payload,
                         uint32_t length)
{
    return send_frame(fd, opcode, payload, length, 1);
}

int proto_recv(int fd, proto_packet_t *pkt, int timeout_sec)
{
    uint8_t hdr[PROTO_HEADER_SIZE];
    uint16_t magic;
    uint32_t net_len, net_crc, want, crc;
    int64_t deadline = timeout_sec > 0 ? now_ms() + timeout_sec * 1000LL : 0;
    int rc;

    rc = recv_full(fd, hdr, sizeof(hdr), deadline);
    if (rc != PROTO_OK)
        return rc;

    memcpy(&magic, hdr, 2);
    memcpy(&net_len, hdr + 4, 4);
    memcpy(&net_crc, hdr + 8, 4);
    pkt->opcode = hdr[3];
    pkt->length = ntohl(net_len);
    want = ntohl(net_crc);

    /* 先驗證 header，避免依不可信的 Length 繼續收資料 */
    if (ntohs(magic) != PROTO_MAGIC)
        return PROTO_ERR_MAGIC;
    if (hdr[2] != PROTO_VERSION)
        return PROTO_ERR_VERSION;
    if (pkt->length > PROTO_MAX_PAYLOAD)
        return PROTO_ERR_LENGTH;

    if (pkt->length > 0) {
        rc = recv_full(fd, pkt->payload, pkt->length, deadline);
        if (rc == PROTO_ERR_CLOSED)
            return PROTO_ERR_TRUNCATED;
        if (rc != PROTO_OK)
            return rc;
    }
    pkt->payload[pkt->length] = '\0';

    crc = proto_crc32(0, hdr, 8);
    crc = proto_crc32(crc, pkt->payload, pkt->length);

    LOG_DEBUG("rx op=0x%02x len=%u crc=%08x", pkt->opcode, pkt->length, want);

    if (g_crc_check && crc != want)
        return PROTO_ERR_CRC;
    return PROTO_OK;
}

const char *proto_strerror(int status)
{
    switch (status) {
    case PROTO_OK:            return "ok";
    case PROTO_ERR_CLOSED:    return "connection closed";
    case PROTO_ERR_IO:        return "socket I/O error";
    case PROTO_ERR_TIMEOUT:   return "idle timeout";
    case PROTO_ERR_MAGIC:     return "bad magic";
    case PROTO_ERR_VERSION:   return "unsupported version";
    case PROTO_ERR_LENGTH:    return "bad length";
    case PROTO_ERR_CRC:       return "CRC mismatch";
    case PROTO_ERR_TRUNCATED: return "truncated packet";
    default:                  return "unknown error";
    }
}

int parse_int(const char *text, int min, int max, int *out)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < min || value > max)
        return -1;
    *out = (int)value;
    return 0;
}
