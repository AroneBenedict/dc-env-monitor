#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *kind_name(enum sensor_kind kind)
{
    switch (kind) {
    case KIND_TEMPERATURE: return "temperature";
    case KIND_HUMIDITY: return "humidity";
    case KIND_DOOR: return "door";
    case KIND_LEAK: return "leak";
    }
    return "unknown";
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static int parse_double(const char *v, double *out)
{
    char *end;
    errno = 0;
    double d = strtod(v, &end);
    if (errno || end == v || *end || !isfinite(d))
        return -1;
    *out = d;
    return 0;
}

static int parse_long(const char *v, long lo, long hi, long *out)
{
    char *end;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (errno || end == v || *end || n < lo || n > hi)
        return -1;
    *out = n;
    return 0;
}

static void sensor_defaults(struct sensor_config *s, enum sensor_kind kind)
{
    s->kind = kind;
    s->warn_low = s->crit_low = s->warn_high = s->crit_high = NAN;
    s->hysteresis = 0;
    s->delay = 0;
    switch (kind) {
    /* 1-Wire temperature probes report 85.0 C after a brown-out reset and -127 C on a CRC
       failure; a 10 C jump within one sample is a glitch, not air temperature. */
    case KIND_TEMPERATURE: s->min_valid = -40; s->max_valid = 125; s->hysteresis = 1; s->delay = 60; s->max_step = 10;
        break;
    case KIND_HUMIDITY: s->min_valid = 0; s->max_valid = 100; s->hysteresis = 3; s->delay = 120; s->max_step = 25;
        break;
    case KIND_DOOR: s->min_valid = 0; s->max_valid = 1; s->warn_high = 0.5; s->delay = 300; break;
    case KIND_LEAK: s->min_valid = 0; s->max_valid = 1; s->crit_high = 0.5; s->delay = 0; break;
    }
}

#define FAIL(...) do { snprintf(err, errlen, "line %d: ", lineno); \
    size_t used_ = strlen(err); snprintf(err + used_, errlen - used_, __VA_ARGS__); return -1; } while (0)

int config_parse(const char *text, struct config *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof *out);
    copy_str(out->collector_host, sizeof out->collector_host, "127.0.0.1");
    out->collector_port = 7070;
    copy_str(out->spool_dir, sizeof out->spool_dir, ".");
    out->report_interval = 60;
    out->stale_after = 180;
    out->spool_max_bytes = 64L * 1024 * 1024;

    enum { NONE, DEVICE, SENSOR } section = NONE;
    struct sensor_config *cur = NULL;
    int kind_set = 0;
    int lineno = 0;
    const char *p = text;
    char line[MAX_LINE];

    while (*p) {
        lineno++;
        size_t len = strcspn(p, "\n");
        if (len >= sizeof line)
            FAIL("line too long");
        memcpy(line, p, len);
        line[len] = '\0';
        p += len + (p[len] == '\n');

        char *hash = strpbrk(line, "#;");
        if (hash)
            *hash = '\0';
        char *s = trim(line);
        if (!*s)
            continue;

        if (*s == '[') {
            if (cur && !kind_set)
                FAIL("sensor %s has no kind", cur->id);
            char *close = strchr(s, ']');
            if (!close || close[1])
                FAIL("bad section header");
            *close = '\0';
            char *name = trim(s + 1);
            if (strcmp(name, "device") == 0) {
                section = DEVICE;
                cur = NULL;
            } else if (strncmp(name, "sensor ", 7) == 0) {
                char *id = trim(name + 7);
                if (!valid_id(id))
                    FAIL("invalid sensor id");
                if (config_find(out, id) >= 0)
                    FAIL("duplicate sensor %s", id);
                if (out->nsensors >= MAX_SENSORS)
                    FAIL("too many sensors (max %d)", MAX_SENSORS);
                cur = &out->sensors[out->nsensors++];
                memset(cur, 0, sizeof *cur);
                copy_str(cur->id, sizeof cur->id, id);
                sensor_defaults(cur, KIND_TEMPERATURE);
                kind_set = 0;
                section = SENSOR;
            } else {
                FAIL("unknown section [%s]", name);
            }
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq)
            FAIL("expected key = value");
        *eq = '\0';
        char *key = trim(s), *val = trim(eq + 1);
        if (!*val)
            FAIL("empty value for %s", key);

        if (section == DEVICE) {
            long n;
            if (strcmp(key, "id") == 0) {
                if (!valid_id(val))
                    FAIL("invalid device id");
                copy_str(out->device_id, sizeof out->device_id, val);
            } else if (strcmp(key, "collector_host") == 0) {
                if (copy_str(out->collector_host, sizeof out->collector_host, val))
                    FAIL("collector_host too long");
            } else if (strcmp(key, "collector_port") == 0) {
                if (parse_long(val, 1, 65535, &n))
                    FAIL("collector_port must be 1-65535");
                out->collector_port = (int)n;
            } else if (strcmp(key, "spool_dir") == 0) {
                if (copy_str(out->spool_dir, sizeof out->spool_dir, val))
                    FAIL("spool_dir too long");
            } else if (strcmp(key, "report_interval") == 0) {
                if (parse_long(val, 1, 86400, &out->report_interval))
                    FAIL("report_interval must be 1-86400 seconds");
            } else if (strcmp(key, "stale_after") == 0) {
                if (parse_long(val, 10, 86400, &out->stale_after))
                    FAIL("stale_after must be 10-86400 seconds");
            } else if (strcmp(key, "spool_max_bytes") == 0) {
                if (parse_long(val, 4096, 1L << 34, &out->spool_max_bytes))
                    FAIL("spool_max_bytes out of range");
            } else {
                FAIL("unknown device key %s", key);
            }
        } else if (section == SENSOR) {
            if (strcmp(key, "kind") == 0) {
                enum sensor_kind k;
                if (strcmp(val, "temperature") == 0) k = KIND_TEMPERATURE;
                else if (strcmp(val, "humidity") == 0) k = KIND_HUMIDITY;
                else if (strcmp(val, "door") == 0) k = KIND_DOOR;
                else if (strcmp(val, "leak") == 0) k = KIND_LEAK;
                else FAIL("unknown kind %s", val);
                if (kind_set)
                    FAIL("kind set twice");
                char id[MAX_ID];
                copy_str(id, sizeof id, cur->id);
                sensor_defaults(cur, k);
                copy_str(cur->id, sizeof cur->id, id);
                kind_set = 1;
            } else {
                if (!kind_set)
                    FAIL("set kind before other sensor keys");
                double d;
                long n;
                if (strcmp(key, "delay") == 0) {
                    if (parse_long(val, 0, 86400, &n))
                        FAIL("delay must be 0-86400 seconds");
                    cur->delay = n;
                    continue;
                }
                if (parse_double(val, &d))
                    FAIL("%s must be a number", key);
                if (strcmp(key, "warn_low") == 0) cur->warn_low = d;
                else if (strcmp(key, "crit_low") == 0) cur->crit_low = d;
                else if (strcmp(key, "warn_high") == 0) cur->warn_high = d;
                else if (strcmp(key, "crit_high") == 0) cur->crit_high = d;
                else if (strcmp(key, "hysteresis") == 0) {
                    if (d < 0)
                        FAIL("hysteresis must be >= 0");
                    cur->hysteresis = d;
                } else if (strcmp(key, "min_valid") == 0) cur->min_valid = d;
                else if (strcmp(key, "max_valid") == 0) cur->max_valid = d;
                else if (strcmp(key, "max_step") == 0) {
                    if (d < 0)
                        FAIL("max_step must be >= 0");
                    cur->max_step = d;
                }
                else FAIL("unknown sensor key %s", key);
            }
        } else {
            FAIL("key outside a section");
        }
    }
    if (cur && !kind_set)
        FAIL("sensor %s has no kind", cur->id);
    lineno = 0;
    if (!out->device_id[0])
        FAIL("[device] id is required");
    for (int i = 0; i < out->nsensors; i++) {
        struct sensor_config *s = &out->sensors[i];
        if (!isnan(s->warn_high) && !isnan(s->crit_high) && s->crit_high < s->warn_high)
            FAIL("sensor %s: crit_high below warn_high", s->id);
        if (!isnan(s->warn_low) && !isnan(s->crit_low) && s->crit_low > s->warn_low)
            FAIL("sensor %s: crit_low above warn_low", s->id);
        if (s->min_valid >= s->max_valid)
            FAIL("sensor %s: min_valid must be below max_valid", s->id);
    }
    return 0;
}

int config_load(const char *path, struct config *out, char *err, size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    static char buf[64 * 1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    int too_big = !feof(f);
    fclose(f);
    if (too_big) {
        snprintf(err, errlen, "%s is larger than %zu bytes", path, sizeof buf - 1);
        return -1;
    }
    buf[n] = '\0';
    return config_parse(buf, out, err, errlen);
}

int config_find(const struct config *cfg, const char *id)
{
    for (int i = 0; i < cfg->nsensors; i++)
        if (strcmp(cfg->sensors[i].id, id) == 0)
            return i;
    return -1;
}
