#include "alarm.h"

#include <math.h>

const char *severity_name(enum severity s)
{
    switch (s) {
    case SEV_NORMAL: return "normal";
    case SEV_WARNING: return "warning";
    case SEV_CRITICAL: return "critical";
    case SEV_OFFLINE: return "offline";
    }
    return "unknown";
}

void sensor_state_init(struct sensor_state *st)
{
    st->severity = SEV_NORMAL;
    st->pending = SEV_NORMAL;
    st->pending_since = 0;
    st->last_ts = 0;
    st->last_value = NAN;
    st->glitches = 0;
    st->window_start = 0;
    st->sum = 0;
    st->min = INFINITY;
    st->max = -INFINITY;
    st->n = 0;
}

/* Is the value beyond a threshold? When already at or above that level, require it to come
 * back by `hyst` before it counts as inside again. */
static int beyond_high(double v, double limit, int holding, double hyst)
{
    return !isnan(limit) && (holding ? v > limit - hyst : v > limit);
}

static int beyond_low(double v, double limit, int holding, double hyst)
{
    return !isnan(limit) && (holding ? v < limit + hyst : v < limit);
}

enum severity alarm_target(const struct sensor_config *cfg, enum severity current, double value)
{
    int hold_crit = current == SEV_CRITICAL;
    int hold_warn = current >= SEV_WARNING && current != SEV_OFFLINE;
    double h = cfg->hysteresis;
    if (beyond_high(value, cfg->crit_high, hold_crit, h) || beyond_low(value, cfg->crit_low, hold_crit, h))
        return SEV_CRITICAL;
    if (beyond_high(value, cfg->warn_high, hold_warn, h) || beyond_low(value, cfg->warn_low, hold_warn, h))
        return SEV_WARNING;
    return SEV_NORMAL;
}

int alarm_update(const struct sensor_config *cfg, struct sensor_state *st, long long ts, double value,
                 struct alarm_event *ev)
{
    enum severity base = st->severity == SEV_OFFLINE ? SEV_NORMAL : st->severity;
    enum severity target = alarm_target(cfg, base, value);
    int was_offline = st->severity == SEV_OFFLINE;
    st->last_ts = ts;

    if (target != st->pending) {
        st->pending = target;
        st->pending_since = ts;
    }
    int held = ts - st->pending_since >= cfg->delay;
    if (was_offline || (target != st->severity && held)) {
        /* Coming back from offline reports the current level straight away. */
        enum severity next = target;
        ev->from = st->severity;
        ev->to = next;
        ev->ts = ts;
        ev->value = value;
        st->severity = next;
        return 1;
    }
    return 0;
}

int alarm_check_stale(const struct sensor_state *st, long long now, long stale_after)
{
    return st->last_ts > 0 && st->severity != SEV_OFFLINE && now - st->last_ts > stale_after;
}
