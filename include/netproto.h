/*
 * netproto.h - public interface of libnetproto.so: packet protocol and logging.
 *
 * Packet format (all multi-byte fields in network byte order):
 *
 *   offset  size  field
 *        0     2  Magic   (0x4E53, "NS")
 *        2     1  Version (1)
 *        3     1  OpCode
 *        4     4  Length  (payload length, header excluded)
 *        8     4  CRC32   (over the first 8 header bytes + payload)
 *       12     N  Payload
 */
#ifndef NETPROTO_H
#define NETPROTO_H

#include <stddef.h>
#include <stdint.h>

#define PROTO_MAGIC        0x4E53
#define PROTO_VERSION      1
#define PROTO_HEADER_SIZE  12
#define PROTO_MAX_PAYLOAD  8192

/* Reply OpCode = request OpCode | PROTO_OP_RESPONSE */
#define PROTO_OP_RESPONSE  0x80

enum proto_opcode {
    OP_PING     = 0x01, /* payload is echoed back */
    OP_HOSTNAME = 0x02,
    OP_UPTIME   = 0x03,
    OP_LOADAVG  = 0x04,
    OP_MEMINFO  = 0x05,
    OP_KERNEL   = 0x06,
    OP_TIME     = 0x07,
    OP_ALL      = 0x08,
    OP_BYE      = 0x0F,
    OP_BUSY     = 0xFE, /* connection limit reached; the server then closes */
    OP_ERROR    = 0xFF  /* payload is the error message */
};

enum proto_status {
    PROTO_OK            =  0,
    PROTO_ERR_CLOSED    = -1, /* peer closed cleanly at a packet boundary */
    PROTO_ERR_IO        = -2,
    PROTO_ERR_TIMEOUT   = -3,
    PROTO_ERR_MAGIC     = -4,
    PROTO_ERR_VERSION   = -5,
    PROTO_ERR_LENGTH    = -6,
    PROTO_ERR_CRC       = -7,
    PROTO_ERR_TRUNCATED = -8  /* peer closed in the middle of a packet */
};

typedef struct {
    uint8_t  opcode;
    uint32_t length;
    /* One extra byte so a '\0' can be appended and the payload used as a string */
    uint8_t  payload[PROTO_MAX_PAYLOAD + 1];
} proto_packet_t;

/* CRC-32 (IEEE 802.3); pass seed 0, or a previous result to continue a running CRC */
uint32_t proto_crc32(uint32_t seed, const void *data, size_t len);

/* Build header + payload and send them together; returns enum proto_status */
int proto_send(int fd, uint8_t opcode, const void *payload, uint32_t length);

/* Testing only: flip the first payload byte after computing the CRC, simulating corruption */
int proto_send_corrupted(int fd, uint8_t opcode, const void *payload,
                         uint32_t length);

/* Comparison experiments only: when 0, proto_recv skips the CRC check */
void proto_set_crc_check(int enabled);

/*
 * Wait with select() and receive one complete packet.
 * timeout_sec is the deadline for the whole packet; 0 means no limit.
 */
int proto_recv(int fd, proto_packet_t *pkt, int timeout_sec);

const char *proto_strerror(int status);

/* Parse a decimal integer and range-check it; returns 0 on success, -1 on failure */
int parse_int(const char *text, int min, int max, int *out);

/* Logging: level 1, the DEBUG macro, decides whether debug code is compiled in; level 2, -d, decides whether it prints at run time */
void log_set_debug(int enabled);
int  log_debug_enabled(void);
void log_msg(const char *level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define LOG_INFO(fmt, ...)  log_msg("INFO",  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  log_msg("WARN",  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_msg("ERROR", fmt, ##__VA_ARGS__)

#ifdef DEBUG
#define LOG_DEBUG(fmt, ...)                                            \
    do {                                                               \
        if (log_debug_enabled())                                       \
            log_msg("DEBUG", "%s:%d: " fmt, __FILE__, __LINE__,        \
                    ##__VA_ARGS__);                                    \
    } while (0)
#else
/* Keep if (0) so the compiler still checks format and arguments; optimization leaves no code or strings */
#define LOG_DEBUG(fmt, ...)                                            \
    do {                                                               \
        if (0)                                                         \
            log_msg("DEBUG", fmt, ##__VA_ARGS__);                      \
    } while (0)
#endif

#endif /* NETPROTO_H */
