/*
 * netproto.h - libnetproto.so 的公開介面：封包協定與 log。
 *
 * 封包格式（多位元組欄位皆為 network byte order）：
 *
 *   offset  size  field
 *        0     2  Magic   (0x4E53, "NS")
 *        2     1  Version (1)
 *        3     1  OpCode
 *        4     4  Length  (payload 長度，不含 header)
 *        8     4  CRC32   (涵蓋 header 前 8 bytes + payload)
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

/* 回應的 OpCode = 請求的 OpCode | PROTO_OP_RESPONSE */
#define PROTO_OP_RESPONSE  0x80

enum proto_opcode {
    OP_PING     = 0x01, /* payload 原樣回傳 */
    OP_HOSTNAME = 0x02,
    OP_UPTIME   = 0x03,
    OP_LOADAVG  = 0x04,
    OP_MEMINFO  = 0x05,
    OP_KERNEL   = 0x06,
    OP_TIME     = 0x07,
    OP_ALL      = 0x08,
    OP_BYE      = 0x0F,
    OP_BUSY     = 0xFE, /* 已達連線上限，server 隨後關閉連線 */
    OP_ERROR    = 0xFF  /* payload 為錯誤訊息 */
};

enum proto_status {
    PROTO_OK            =  0,
    PROTO_ERR_CLOSED    = -1, /* 對方在封包邊界正常關閉 */
    PROTO_ERR_IO        = -2,
    PROTO_ERR_TIMEOUT   = -3,
    PROTO_ERR_MAGIC     = -4,
    PROTO_ERR_VERSION   = -5,
    PROTO_ERR_LENGTH    = -6,
    PROTO_ERR_CRC       = -7,
    PROTO_ERR_TRUNCATED = -8  /* 對方在封包中途關閉 */
};

typedef struct {
    uint8_t  opcode;
    uint32_t length;
    /* 多留 1 byte，收到後補 '\0' 方便當字串用 */
    uint8_t  payload[PROTO_MAX_PAYLOAD + 1];
} proto_packet_t;

/* CRC-32 (IEEE 802.3)，seed 傳 0；可把前一次結果當 seed 接續計算 */
uint32_t proto_crc32(uint32_t seed, const void *data, size_t len);

/* 將 header 與 payload 組成一筆送出，回傳 enum proto_status */
int proto_send(int fd, uint8_t opcode, const void *payload, uint32_t length);

/* 測試用：算完 CRC 後才竄改 payload 第一個 byte，模擬傳輸錯誤 */
int proto_send_corrupted(int fd, uint8_t opcode, const void *payload,
                         uint32_t length);

/* 對照實驗用：傳 0 時 proto_recv 不驗證 CRC */
void proto_set_crc_check(int enabled);

/*
 * 用 select() 等待並收一個完整封包。
 * timeout_sec 是整個封包的期限，傳 0 表示不限時。
 */
int proto_recv(int fd, proto_packet_t *pkt, int timeout_sec);

const char *proto_strerror(int status);

/* 解析十進位整數並檢查範圍，成功回傳 0，失敗回傳 -1 */
int parse_int(const char *text, int min, int max, int *out);

/* log：第一層 DEBUG 巨集決定是否編入，第二層 -d 決定執行時是否輸出 */
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
/* 保留 if (0) 讓編譯器仍檢查格式與參數，最佳化後不留下任何程式碼或字串 */
#define LOG_DEBUG(fmt, ...)                                            \
    do {                                                               \
        if (0)                                                         \
            log_msg("DEBUG", fmt, ##__VA_ARGS__);                      \
    } while (0)
#endif

#endif /* NETPROTO_H */
