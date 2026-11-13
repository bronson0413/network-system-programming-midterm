/* log.c - 帶時間與 PID 的 log，多個子程序同時輸出時行內容不會交錯 */
#include "netproto.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int g_debug_enabled;

void log_set_debug(int enabled)
{
#ifndef DEBUG
    if (enabled)
        LOG_WARN("-d ignored: built without DEBUG (use 'make debug')");
#endif
    g_debug_enabled = enabled;
}

int log_debug_enabled(void)
{
    return g_debug_enabled;
}

void log_msg(const char *level, const char *fmt, ...)
{
    char line[1024];
    struct timeval tv;
    struct tm tm;
    va_list ap;
    size_t off;
    int n;

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);

    off = strftime(line, sizeof(line), "%H:%M:%S", &tm);
    n = snprintf(line + off, sizeof(line) - off, ".%03ld [%d] %-5s ",
                 (long)tv.tv_usec / 1000, (int)getpid(), level);
    off += (size_t)n;

    va_start(ap, fmt);
    n = vsnprintf(line + off, sizeof(line) - off - 1, fmt, ap);
    va_end(ap);

    if (n > 0)
        off += (size_t)n < sizeof(line) - off - 1 ? (size_t)n
                                                  : sizeof(line) - off - 2;
    line[off++] = '\n';

    /* 整行一次 write，不經 stdio 緩衝 */
    if (write(STDERR_FILENO, line, off) < 0)
        return;
}
