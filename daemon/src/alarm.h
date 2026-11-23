/* Per-sensor alarm state machine and reading aggregation.
 *
 * Severity rises only after the condition has held for `delay` seconds of sample time, and
 * falls only after the value has been back inside the limit by `hysteresis` for `delay`
 * seconds. Both rules exist to stop alarm flapping when a value hovers near a threshold.
 */
#ifndef ENVMOND_ALARM_H
#define ENVMOND_ALARM_H

#include "config.h"

enum severity { SEV_NORMAL = 0, SEV_WARNING = 1, SEV_CRITICAL = 2, SEV_OFFLINE = 3 };

struct sensor_state {
    enum severity severity;
    enum severity pending;       /* level the value currently points to */
    long long pending_since;     /* sample time the pending level was first seen */
    long long last_ts;           /* last accepted sample */
    double last_value;
    int glitches;                /* consecutive samples rejected as jumps */
    /* aggregation window */
    long long window_start;
    double sum, min, max;
    long n;
};

struct alarm_event {
    enum severity from, to;
    long long ts;
    double value;
};

const char *severity_name(enum severity s);

void sensor_state_init(struct sensor_state *st);

/* Feed one valid sample. Returns 1 and fills *ev when the alarm state changes. */
int alarm_update(const struct sensor_config *cfg, struct sensor_state *st, long long ts, double value,
                 struct alarm_event *ev);

/* Raise or clear the offline state. `now` is the newest sample time seen from any sensor. */
int alarm_check_stale(const struct sensor_state *st, long long now, long stale_after);

/* The level a value points to, given the current severity (hysteresis depends on it). */
enum severity alarm_target(const struct sensor_config *cfg, enum severity current, double value);

#endif
