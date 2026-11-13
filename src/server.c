/* server.c - 每條連線 fork() 一個子程序的系統資訊 server */
#include "netproto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT      9000
#define DEFAULT_MAX_CONN  32
#define DEFAULT_IDLE_SEC  30
#define MAX_CONN_LIMIT    512
#define LISTEN_BACKLOG    MAX_CONN_LIMIT /* 小於上限時，尖峰連線會溢出並等 TCP 重送 */
#define ACCEPT_BACKOFF_USEC 100000
#define LOAD_SCALE        65536.0 /* sysinfo.loads 的定點數比例 (1 << 16) */

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_active;
static volatile sig_atomic_t g_reaped;
/* 主流程只在擋住 SIGCHLD 時修改，signal handler 內才能安全存取 */
static pid_t g_children[MAX_CONN_LIMIT];
static int g_max_conn = DEFAULT_MAX_CONN;

/* -X 關閉的機制，只供「有／無穩健機制」對照實驗使用 */
static int g_no_reap, g_no_idle, g_no_limit, g_no_reuseaddr;

static int disable_mechanism(const char *name)
{
    if (strcmp(name, "reap") == 0)
        g_no_reap = 1;
    else if (strcmp(name, "idle") == 0)
        g_no_idle = 1;
    else if (strcmp(name, "limit") == 0)
        g_no_limit = 1;
    else if (strcmp(name, "reuseaddr") == 0)
        g_no_reuseaddr = 1;
    else if (strcmp(name, "crc") == 0)
        proto_set_crc_check(0);
    else
        return -1;
    LOG_WARN("robustness mechanism disabled: %s", name);
    return 0;
}

/* 一次 SIGCHLD 可能代表多個子程序結束，必須迴圈回收到沒有為止 */
static void on_sigchld(int sig)
{
    int saved_errno = errno;
    pid_t pid;

    (void)sig;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        for (int i = 0; i < g_max_conn; i++) {
            if (g_children[i] == pid) {
                g_children[i] = 0;
                g_active--;
                g_reaped++;
                break;
            }
        }
    }
    errno = saved_errno;
}

static void on_stop(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void install_signal(int sig, void (*handler)(int), int flags)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    if (sigaction(sig, &sa, NULL) < 0) {
        LOG_ERROR("sigaction(%d): %s", sig, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

static size_t appendf(char *buf, size_t cap, size_t off, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (off + 1 >= cap)
        return off;
    va_start(ap, fmt);
    n = vsnprintf(buf + off, cap - off, fmt, ap);
    va_end(ap);
    if (n < 0)
        return off;
    return (size_t)n < cap - off ? off + (size_t)n : cap - 1;
}

static size_t add_hostname(char *buf, size_t cap, size_t off)
{
    char name[256];

    if (gethostname(name, sizeof(name)) < 0)
        snprintf(name, sizeof(name), "unknown");
    name[sizeof(name) - 1] = '\0';
    return appendf(buf, cap, off, "hostname=%s\n", name);
}

static size_t add_uptime(char *buf, size_t cap, size_t off)
{
    struct sysinfo si;

    if (sysinfo(&si) < 0)
        return appendf(buf, cap, off, "uptime_sec=unknown\n");
    return appendf(buf, cap, off, "uptime_sec=%ld\n", si.uptime);
}

static size_t add_loadavg(char *buf, size_t cap, size_t off)
{
    struct sysinfo si;

    if (sysinfo(&si) < 0)
        return appendf(buf, cap, off, "loadavg=unknown\n");
    return appendf(buf, cap, off, "loadavg=%.2f %.2f %.2f\nprocs=%u\ncpus=%ld\n",
                   si.loads[0] / LOAD_SCALE, si.loads[1] / LOAD_SCALE,
                   si.loads[2] / LOAD_SCALE, (unsigned)si.procs,
                   sysconf(_SC_NPROCESSORS_ONLN));
}

static size_t add_meminfo(char *buf, size_t cap, size_t off)
{
    struct sysinfo si;
    unsigned long long unit;

    if (sysinfo(&si) < 0)
        return appendf(buf, cap, off, "mem=unknown\n");
    unit = si.mem_unit;
    return appendf(buf, cap, off, "mem_total_kb=%llu\nmem_free_kb=%llu\n",
                   si.totalram * unit / 1024, si.freeram * unit / 1024);
}

static size_t add_kernel(char *buf, size_t cap, size_t off)
{
    struct utsname un;

    if (uname(&un) < 0)
        return appendf(buf, cap, off, "kernel=unknown\n");
    return appendf(buf, cap, off, "kernel=%s %s %s\n",
                   un.sysname, un.release, un.machine);
}

static size_t add_time(char *buf, size_t cap, size_t off)
{
    char text[64];
    struct tm tm;
    time_t now = time(NULL);

    localtime_r(&now, &tm);
    strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S%z", &tm);
    return appendf(buf, cap, off, "time=%s\n", text);
}

static size_t add_process(char *buf, size_t cap, size_t off)
{
    return appendf(buf, cap, off, "worker_pid=%d\nserver_pid=%d\n",
                   (int)getpid(), (int)getppid());
}

/* 回傳 payload 長度；不是查詢類 OpCode 時回傳 -1 */
static long build_info(uint8_t op, char *buf, size_t cap)
{
    size_t off = 0;

    switch (op) {
    case OP_HOSTNAME: off = add_hostname(buf, cap, off); break;
    case OP_UPTIME:   off = add_uptime(buf, cap, off);   break;
    case OP_LOADAVG:  off = add_loadavg(buf, cap, off);  break;
    case OP_MEMINFO:  off = add_meminfo(buf, cap, off);  break;
    case OP_KERNEL:   off = add_kernel(buf, cap, off);   break;
    case OP_TIME:     off = add_time(buf, cap, off);     break;
    case OP_ALL:
        off = add_hostname(buf, cap, off);
        off = add_kernel(buf, cap, off);
        off = add_uptime(buf, cap, off);
        off = add_loadavg(buf, cap, off);
        off = add_meminfo(buf, cap, off);
        off = add_time(buf, cap, off);
        off = add_process(buf, cap, off);
        break;
    default:
        return -1;
    }
    return (long)off;
}

static void send_error(int fd, const char *msg)
{
    (void)proto_send(fd, OP_ERROR, msg, (uint32_t)strlen(msg));
}

static void serve_client(int fd, int idle_sec)
{
    static proto_packet_t req;
    char body[PROTO_MAX_PAYLOAD];

    for (;;) {
        int rc = proto_recv(fd, &req, idle_sec);
        uint8_t reply_op = req.opcode | PROTO_OP_RESPONSE;
        long len;

        if (rc == PROTO_ERR_CLOSED) {
            LOG_INFO("client disconnected");
            return;
        }
        if (rc != PROTO_OK) {
            /* 串流已無法對齊封包邊界，回報後直接結束連線 */
            LOG_WARN("closing connection: %s", proto_strerror(rc));
            send_error(fd, proto_strerror(rc));
            return;
        }

        LOG_DEBUG("request op=0x%02x len=%u", req.opcode, req.length);

        if (req.opcode == OP_BYE) {
            (void)proto_send(fd, reply_op, NULL, 0);
            LOG_INFO("client said bye");
            return;
        }

        if (req.opcode == OP_PING) {
            rc = proto_send(fd, reply_op, req.payload, req.length);
        } else if ((len = build_info(req.opcode, body, sizeof(body))) >= 0) {
            rc = proto_send(fd, reply_op, body, (uint32_t)len);
        } else {
            LOG_WARN("unknown opcode 0x%02x", req.opcode);
            send_error(fd, "unknown opcode");
            continue;
        }

        if (rc != PROTO_OK) {
            LOG_WARN("send failed: %s", proto_strerror(rc));
            return;
        }
    }
}

static int create_listener(const char *addr, int port)
{
    struct sockaddr_in sin;
    int fd, on = 1;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, addr, &sin.sin_addr) != 1) {
        LOG_ERROR("invalid bind address: %s", addr);
        return -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("socket: %s", strerror(errno));
        return -1;
    }

    /* 必須在 bind() 前設定，server 重啟時才不會被 TIME_WAIT 擋住 */
    if (!g_no_reuseaddr &&
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        LOG_ERROR("setsockopt(SO_REUSEADDR): %s", strerror(errno));
        goto fail;
    }
    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        LOG_ERROR("bind %s:%d: %s", addr, port, strerror(errno));
        goto fail;
    }
    /* select() 說可讀後，連線仍可能已被對方中止，accept() 不可卡住 */
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        LOG_ERROR("fcntl: %s", strerror(errno));
        goto fail;
    }
    if (listen(fd, LISTEN_BACKLOG) < 0) {
        LOG_ERROR("listen: %s", strerror(errno));
        goto fail;
    }
    return fd;

fail:
    close(fd);
    return -1;
}

/* 子程序入口，不會返回 */
static void run_child(int listen_fd, int conn_fd, int idle_sec,
                      const sigset_t *orig_mask)
{
    struct timeval tv = { .tv_sec = idle_sec, .tv_usec = 0 };

    install_signal(SIGCHLD, SIG_DFL, 0);
    install_signal(SIGINT, SIG_DFL, 0);
    install_signal(SIGTERM, SIG_DFL, 0);
    sigprocmask(SIG_SETMASK, orig_mask, NULL);
    close(listen_fd);

    /* client 一直不讀時，send() 也要有期限，才不會永遠佔住名額 */
    if (setsockopt(conn_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0)
        LOG_WARN("setsockopt(SO_SNDTIMEO): %s", strerror(errno));

    serve_client(conn_fd, g_no_idle ? 0 : idle_sec);
    close(conn_fd);
    _exit(EXIT_SUCCESS);
}

static void stop_children(const sigset_t *block_mask)
{
    sigset_t wait_mask;

    sigprocmask(SIG_BLOCK, block_mask, &wait_mask);
    sigdelset(&wait_mask, SIGCHLD);

    for (int i = 0; i < g_max_conn; i++) {
        if (g_children[i] > 0)
            kill(g_children[i], SIGTERM);
    }
    if (g_no_reap)
        return;
    while (g_active > 0)
        sigsuspend(&wait_mask);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-b addr] [-p port] [-m max_conn] [-t idle_sec] [-d]"
            " [-X name]\n"
            "  -b addr      bind address (default 127.0.0.1)\n"
            "  -p port      listen port (default %d)\n"
            "  -m max_conn  concurrent connection limit, 1-%d (default %d)\n"
            "  -t idle_sec  idle timeout in seconds (default %d)\n"
            "  -d           enable debug log (needs a DEBUG build)\n"
            "  -X name      disable a mechanism for comparison, repeatable:\n"
            "               reap|idle|limit|reuseaddr|crc\n",
            prog, DEFAULT_PORT, MAX_CONN_LIMIT, DEFAULT_MAX_CONN,
            DEFAULT_IDLE_SEC);
}

int main(int argc, char **argv)
{
    const char *bind_addr = "127.0.0.1";
    int port = DEFAULT_PORT, idle_sec = DEFAULT_IDLE_SEC;
    int opt, listen_fd, peak = 0;
    unsigned long total = 0, rejected = 0;
    sigset_t block_mask, orig_mask;

    while ((opt = getopt(argc, argv, "b:p:m:t:dX:h")) != -1) {
        switch (opt) {
        case 'b':
            bind_addr = optarg;
            break;
        case 'p':
            if (parse_int(optarg, 1, 65535, &port) < 0)
                goto bad_usage;
            break;
        case 'm':
            if (parse_int(optarg, 1, MAX_CONN_LIMIT, &g_max_conn) < 0)
                goto bad_usage;
            break;
        case 't':
            if (parse_int(optarg, 1, 86400, &idle_sec) < 0)
                goto bad_usage;
            break;
        case 'd':
            log_set_debug(1);
            break;
        case 'X':
            if (disable_mechanism(optarg) < 0)
                goto bad_usage;
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            goto bad_usage;
        }
    }
    if (optind != argc)
        goto bad_usage;

    /* 關閉上限時改用追蹤表的容量，g_children 仍不會溢位 */
    if (g_no_limit)
        g_max_conn = MAX_CONN_LIMIT;

    if (!g_no_reap)
        install_signal(SIGCHLD, on_sigchld, SA_RESTART | SA_NOCLDSTOP);
    install_signal(SIGINT, on_stop, 0);
    install_signal(SIGTERM, on_stop, 0);
    install_signal(SIGPIPE, SIG_IGN, 0);

    listen_fd = create_listener(bind_addr, port);
    if (listen_fd < 0)
        return EXIT_FAILURE;

    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGCHLD);
    sigaddset(&block_mask, SIGINT);
    sigaddset(&block_mask, SIGTERM);

    LOG_INFO("listening on %s:%d max_conn=%d idle_timeout=%ds",
             bind_addr, port, g_max_conn, idle_sec);

    while (!g_stop) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        struct timeval tick = { .tv_sec = 1, .tv_usec = 0 };
        char peer_ip[INET_ADDRSTRLEN] = "?";
        fd_set rfds;
        int ready, conn_fd;
        pid_t pid;

        /* 定期醒來，確保在 select 前到達的停止訊號也會被看見 */
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        ready = select(listen_fd + 1, &rfds, NULL, NULL, &tick);
        if (ready < 0 && errno != EINTR) {
            LOG_ERROR("select: %s", strerror(errno));
            break;
        }
        if (ready <= 0)
            continue;

        conn_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (conn_fd < 0) {
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK &&
                errno != ECONNABORTED) {
                LOG_WARN("accept: %s", strerror(errno));
                /* fd 用盡時連線仍在佇列，select 會立刻返回，暫停避免空轉 */
                usleep(ACCEPT_BACKOFF_USEC);
            }
            continue;
        }
        inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));

        /* 名額檢查、fork 與登記必須不被 SIGCHLD 插入 */
        sigprocmask(SIG_BLOCK, &block_mask, &orig_mask);

        if (g_active >= g_max_conn) {
            int active = g_active;

            sigprocmask(SIG_SETMASK, &orig_mask, NULL);
            rejected++;
            LOG_WARN("rejected %s:%u: limit reached active=%d/%d",
                     peer_ip, ntohs(peer.sin_port), active, g_max_conn);
            (void)proto_send(conn_fd, OP_BUSY, "server busy", 11);
            close(conn_fd);
            continue;
        }

        pid = fork();
        if (pid == 0)
            run_child(listen_fd, conn_fd, idle_sec, &orig_mask);

        if (pid < 0) {
            sigprocmask(SIG_SETMASK, &orig_mask, NULL);
            LOG_ERROR("fork: %s", strerror(errno));
            send_error(conn_fd, "server cannot fork");
            close(conn_fd);
            continue;
        }

        for (int i = 0; i < g_max_conn; i++) {
            if (g_children[i] == 0) {
                g_children[i] = pid;
                break;
            }
        }
        g_active++;
        if (g_active > peak)
            peak = g_active;
        total++;
        LOG_INFO("accepted %s:%u worker=%d active=%d/%d", peer_ip,
                 ntohs(peer.sin_port), (int)pid, (int)g_active, g_max_conn);

        sigprocmask(SIG_SETMASK, &orig_mask, NULL);
        close(conn_fd);
    }

    close(listen_fd);
    stop_children(&block_mask);
    LOG_INFO("shutdown total=%lu rejected=%lu reaped=%d peak=%d",
             total, rejected, (int)g_reaped, peak);
    return EXIT_SUCCESS;

bad_usage:
    usage(argv[0]);
    return 2;
}
