#include "message.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ids are restricted to [A-Za-z0-9._-] by valid_id(), so they never need JSON escaping. */

static int finish(int n, size_t size)
{
    return (n < 0 || (size_t)n >= size) ? -1 : n;
}

int msg_reading(char *buf, size_t size, unsigned long long seq, const char *device, const char *sensor,
                const char *kind, long long ts, long n, double min, double avg, double max)
{
    return finish(snprintf(buf, size,
                           "{\"seq\":%llu,\"device\":\"%s\",\"type\":\"reading\",\"sensor\":\"%s\",\"kind\":\"%s\","
                           "\"ts\":%lld,\"n\":%ld,\"min\":%.2f,\"avg\":%.2f,\"max\":%.2f}\n",
                           seq, device, sensor, kind, ts, n, min, avg, max),
                  size);
}

int msg_alarm(char *buf, size_t size, unsigned long long seq, const char *device, const char *sensor,
              const char *kind, const struct alarm_event *ev)
{
    char value[32];
    if (isnan(ev->value))
        copy_str(value, sizeof value, "null"); /* offline: there is no current value */
    else
        snprintf(value, sizeof value, "%.2f", ev->value);
    return finish(snprintf(buf, size,
                           "{\"seq\":%llu,\"device\":\"%s\",\"type\":\"alarm\",\"sensor\":\"%s\",\"kind\":\"%s\","
                           "\"ts\":%lld,\"from\":\"%s\",\"to\":\"%s\",\"value\":%s}\n",
                           seq, device, sensor, kind, ev->ts, severity_name(ev->from), severity_name(ev->to), value),
                  size);
}

int msg_status(char *buf, size_t size, unsigned long long seq, const char *device, long long ts,
               unsigned long long accepted, unsigned long long rejected, unsigned long long dropped,
               long long spool_bytes)
{
    return finish(snprintf(buf, size,
                           "{\"seq\":%llu,\"device\":\"%s\",\"type\":\"status\",\"ts\":%lld,\"accepted\":%llu,"
                           "\"rejected\":%llu,\"dropped\":%llu,\"spool_bytes\":%lld}\n",
                           seq, device, ts, accepted, rejected, dropped, spool_bytes),
                  size);
}

unsigned long long msg_seq(const char *line)
{
    const char *p = strstr(line, "\"seq\":");
    if (!p)
        return 0;
    return strtoull(p + 6, NULL, 10);
}
