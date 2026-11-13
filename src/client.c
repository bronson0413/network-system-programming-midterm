/*
 * client.c - query system information from the server
 *
 * Exit codes: 0 success, 1 connection or protocol error, 2 bad arguments,
 *             3 server reached its connection limit, 4 server reported an error (e.g. idle timeout)
 */
#include "netproto.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define EXIT_PROTOCOL 1
#define EXIT_USAGE    2
#define EXIT_BUSY     3
#define EXIT_SERVER   4

static const struct {
    const char *name;
    uint8_t op;
} QUERIES[] = {
    { "ping",     OP_PING },
    { "hostname", OP_HOSTNAME },
    { "uptime",   OP_UPTIME },
    { "load",     OP_LOADAVG },
    { "mem",      OP_MEMINFO },
    { "kernel",   OP_KERNEL },
    { "time",     OP_TIME },
    { "all",      OP_ALL },
};

static int connect_to(const char *host, const char *port)
{
    struct addrinfo hints, *res, *ai;
    int fd = -1, rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        LOG_ERROR("getaddrinfo %s:%s: %s", host, port, gai_strerror(rc));
        return -1;
    }

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        LOG_DEBUG("connect failed: %s", strerror(errno));
        close(fd);
        fd = -1;
    }
    if (fd < 0)
        LOG_ERROR("cannot connect to %s:%s: %s", host, port, strerror(errno));

    freeaddrinfo(res);
    return fd;
}

/* Returns 0 on success, otherwise the process exit code */
static int transact(int fd, uint8_t op, const void *data, uint32_t len,
                    int corrupt, int timeout_sec, proto_packet_t *reply)
{
    int send_rc = corrupt ? proto_send_corrupted(fd, op, data, len)
                          : proto_send(fd, op, data, len);
    /* Receive even if the send failed: a BUSY/ERROR sent before the server closed may still be buffered */
    int rc = proto_recv(fd, reply, timeout_sec);

    if (rc != PROTO_OK) {
        LOG_ERROR("no valid reply: %s",
                  proto_strerror(send_rc != PROTO_OK ? send_rc : rc));
        return EXIT_PROTOCOL;
    }
    if (reply->opcode == OP_BUSY) {
        LOG_WARN("server busy: %s", (char *)reply->payload);
        return EXIT_BUSY;
    }
    if (reply->opcode == OP_ERROR) {
        LOG_WARN("server error: %s", (char *)reply->payload);
        return EXIT_SERVER;
    }
    if (reply->opcode != (uint8_t)(op | PROTO_OP_RESPONSE)) {
        LOG_ERROR("unexpected reply opcode 0x%02x", reply->opcode);
        return EXIT_PROTOCOL;
    }
    return 0;
}

static int lookup_query(const char *name, uint8_t *op)
{
    for (size_t i = 0; i < sizeof(QUERIES) / sizeof(QUERIES[0]); i++) {
        if (strcmp(QUERIES[i].name, name) == 0) {
            *op = QUERIES[i].op;
            return 0;
        }
    }
    return -1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-s host] [-p port] [-q query] [-w sec] [-t sec] [-B]"
            " [-d]\n"
            "  -s host   server host (default 127.0.0.1)\n"
            "  -p port   server port (default 9000)\n"
            "  -q query  ping|hostname|uptime|load|mem|kernel|time|all"
            " (default all)\n"
            "  -w sec    keep the connection open for sec after the query\n"
            "  -t sec    reply timeout (default 10)\n"
            "  -B        send a corrupted ping to test the CRC check\n"
            "  -d        enable debug log (needs a DEBUG build)\n",
            prog);
}

int main(int argc, char **argv)
{
    static proto_packet_t reply;
    const char *host = "127.0.0.1", *port = "9000", *query = "all";
    char ping[64];
    uint32_t ping_len = 0;
    int opt, fd, rc, ignored, hold_sec = 0, timeout_sec = 10, corrupt = 0;
    uint8_t op = OP_ALL;

    while ((opt = getopt(argc, argv, "s:p:q:w:t:Bdh")) != -1) {
        switch (opt) {
        case 's':
            host = optarg;
            break;
        case 'p':
            if (parse_int(optarg, 1, 65535, &ignored) < 0)
                goto bad_usage;
            port = optarg;
            break;
        case 'q':
            query = optarg;
            break;
        case 'w':
            if (parse_int(optarg, 0, 86400, &hold_sec) < 0)
                goto bad_usage;
            break;
        case 't':
            if (parse_int(optarg, 1, 86400, &timeout_sec) < 0)
                goto bad_usage;
            break;
        case 'B':
            corrupt = 1;
            query = "ping";
            break;
        case 'd':
            log_set_debug(1);
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            goto bad_usage;
        }
    }
    if (optind != argc || lookup_query(query, &op) < 0)
        goto bad_usage;

    fd = connect_to(host, port);
    if (fd < 0)
        return EXIT_PROTOCOL;

    if (op == OP_PING)
        ping_len = (uint32_t)snprintf(ping, sizeof(ping), "hello from pid %d",
                                      (int)getpid());

    rc = transact(fd, op, ping, ping_len, corrupt, timeout_sec, &reply);
    if (rc != 0) {
        close(fd);
        return rc;
    }
    fputs((char *)reply.payload, stdout);
    if (op == OP_PING)
        putchar('\n');
    fflush(stdout);

    if (hold_sec > 0)
        sleep((unsigned)hold_sec);

    rc = transact(fd, OP_BYE, NULL, 0, 0, timeout_sec, &reply);
    /* Wait for the server to close first so TIME_WAIT always stays on the server side */
    if (rc == 0)
        (void)proto_recv(fd, &reply, timeout_sec);
    close(fd);
    return rc;

bad_usage:
    usage(argv[0]);
    return EXIT_USAGE;
}
