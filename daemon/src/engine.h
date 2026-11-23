/* The device logic, free of I/O: validate samples, aggregate readings, run alarm state
 * machines, detect offline sensors, and hand finished messages to an output callback. */
#ifndef ENVMOND_ENGINE_H
#define ENVMOND_ENGINE_H

#include <stddef.h>

#include "alarm.h"
#include "config.h"
#include "sample.h"

#define REJECT_REASONS 9
enum reject_reason {
    REJ_PARSE_FORMAT, REJ_PARSE_ID, REJ_PARSE_NUMBER, REJ_TOO_LONG, REJ_UNKNOWN_SENSOR, REJ_OUT_OF_RANGE,
    REJ_OUT_OF_ORDER, REJ_GLITCH, REJ_OTHER
};

/* Returns 0 if the message was stored for sending, -1 if it had to be dropped. */
typedef int (*emit_fn)(void *ctx, const char *msg, size_t len);
typedef unsigned long long (*seq_fn)(void *ctx);

struct engine {
    struct config cfg;
    struct sensor_state st[MAX_SENSORS];
    long long now;                 /* newest sample time seen */
    long long last_status;
    unsigned long long accepted, rejected[REJECT_REASONS], dropped, alarms;
    emit_fn emit;
    seq_fn next_seq;
    void *ctx;
    long long (*backlog)(void *ctx);
};

void engine_init(struct engine *e, const struct config *cfg, emit_fn emit, seq_fn next_seq, void *ctx);

/* Swap in a new configuration, keeping state for sensors that still exist. */
void engine_reload(struct engine *e, const struct config *cfg);

/* Process one input line. Returns 0 if accepted, otherwise the rejection reason + 1. */
int engine_feed(struct engine *e, const char *line, size_t len);

/* Emit the readings of all open aggregation windows (at shutdown). */
void engine_flush(struct engine *e);

unsigned long long engine_rejected_total(const struct engine *e);
const char *reject_name(enum reject_reason r);

#endif
