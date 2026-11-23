#include "log.h"

#include <stdarg.h>
#include <stdio.h>

enum log_level log_threshold = LOG_INFO;

void log_msg(enum log_level level, const char *fmt, ...)
{
    static const char *names[] = {"debug", "info", "warn", "error"};
    if (level < log_threshold)
        return;
    fprintf(stderr, "envmond[%s]: ", names[level]);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
