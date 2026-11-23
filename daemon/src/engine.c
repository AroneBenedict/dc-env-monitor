#include "engine.h"

#include <math.h>
#include <string.h>

#include "message.h"

const char *reject_name(enum reject_reason r)
{
    static const char *names[REJECT_REASONS] = {"bad_format", "bad_id", "bad_number", "too_long", "unknown_sensor",
                                                "out_of_range", "out_of_order", "glitch", "other"};
    return (unsigned)r < REJECT_REASONS ? names[r] : "other";
}

void engine_init(struct engine *e, const struct config *cfg, emit_fn emit, seq_fn next_seq, void *ctx)
{
    memset(e, 0, sizeof *e);
    e->cfg = *cfg;
    for (int i = 0; i < MAX_SENSORS; i++)
        sensor_state_init(&e->st[i]);
    e->emit = emit;
    e->next_seq = next_seq;
    e->ctx = ctx;
}

void engine_reload(struct engine *e, const struct config *cfg)
{
    struct sensor_state old[MAX_SENSORS];
    memcpy(old, e->st, sizeof old);
    struct config prev = e->cfg;
    for (int i = 0; i < cfg->nsensors; i++) {
        int j = config_find(&prev, cfg->sensors[i].id);
        if (j >= 0)
            e->st[i] = old[j];
        else
            sensor_state_init(&e->st[i]);
    }
    e->cfg = *cfg;
}

static void send_msg(struct engine *e, const char *buf, int len)
{
    if (len < 0 || e->emit(e->ctx, buf, (size_t)len) < 0)
        e->dropped++;
}

static void emit_window(struct engine *e, int i)
{
    struct sensor_state *s = &e->st[i];
    const struct sensor_config *c = &e->cfg.sensors[i];
    if (s->n == 0)
        return;
    char buf[MAX_MSG];
    int len = msg_reading(buf, sizeof buf, e->next_seq(e->ctx), e->cfg.device_id, c->id, kind_name(c->kind),
                          s->window_start, s->n, s->min, s->sum / (double)s->n, s->max);
    send_msg(e, buf, len);
    s->n = 0;
    s->sum = 0;
    s->min = INFINITY;
    s->max = -INFINITY;
}

static void emit_alarm(struct engine *e, int i, const struct alarm_event *ev)
{
    const struct sensor_config *c = &e->cfg.sensors[i];
    char buf[MAX_MSG];
    int len = msg_alarm(buf, sizeof buf, e->next_seq(e->ctx), e->cfg.device_id, c->id, kind_name(c->kind), ev);
    send_msg(e, buf, len);
    e->alarms++;
}

static int reject(struct engine *e, enum reject_reason r)
{
    e->rejected[r]++;
    return (int)r + 1;
}

int engine_feed(struct engine *e, const char *line, size_t len)
{
    struct sample s;
    switch (sample_parse(line, len, &s)) {
    case PARSE_OK: break;
    case PARSE_EMPTY: return 0;
    case PARSE_BAD_ID: return reject(e, REJ_PARSE_ID);
    case PARSE_BAD_NUMBER: return reject(e, REJ_PARSE_NUMBER);
    case PARSE_TOO_LONG: return reject(e, REJ_TOO_LONG);
    default: return reject(e, REJ_PARSE_FORMAT);
    }
    int i = config_find(&e->cfg, s.sensor);
    if (i < 0)
        return reject(e, REJ_UNKNOWN_SENSOR);
    const struct sensor_config *c = &e->cfg.sensors[i];
    struct sensor_state *st = &e->st[i];
    if (s.value < c->min_valid || s.value > c->max_valid)
        return reject(e, REJ_OUT_OF_RANGE);
    if (s.ts < st->last_ts)
        return reject(e, REJ_OUT_OF_ORDER);
    /* A single jump is a glitch; if the new level persists for 3 samples it is real. */
    if (c->max_step > 0 && !isnan(st->last_value) && fabs(s.value - st->last_value) > c->max_step &&
        st->glitches < 2) {
        st->glitches++;
        return reject(e, REJ_GLITCH);
    }
    st->glitches = 0;
    st->last_value = s.value;
    e->accepted++;
    if (s.ts > e->now)
        e->now = s.ts;

    /* Aggregate into fixed windows aligned to report_interval. */
    long long interval = e->cfg.report_interval;
    long long window = s.ts - s.ts % interval;
    if (st->n > 0 && window != st->window_start)
        emit_window(e, i);
    if (st->n == 0)
        st->window_start = window;
    st->n++;
    st->sum += s.value;
    if (s.value < st->min) st->min = s.value;
    if (s.value > st->max) st->max = s.value;

    struct alarm_event ev;
    if (alarm_update(c, st, s.ts, s.value, &ev))
        emit_alarm(e, i, &ev);

    /* Any sensor silent for too long goes offline (measured against the newest sample time). */
    for (int j = 0; j < e->cfg.nsensors; j++) {
        if (alarm_check_stale(&e->st[j], e->now, e->cfg.stale_after)) {
            struct alarm_event off = {e->st[j].severity, SEV_OFFLINE, e->now, NAN};
            e->st[j].severity = SEV_OFFLINE;
            e->st[j].pending = SEV_OFFLINE;
            emit_window(e, j);
            emit_alarm(e, j, &off);
        }
    }

    if (e->now - e->last_status >= 3600) {
        char buf[MAX_MSG];
        int n = msg_status(buf, sizeof buf, e->next_seq(e->ctx), e->cfg.device_id, e->now, e->accepted,
                           engine_rejected_total(e), e->dropped, e->backlog ? e->backlog(e->ctx) : 0);
        send_msg(e, buf, n);
        e->last_status = e->now;
    }
    return 0;
}

void engine_flush(struct engine *e)
{
    for (int i = 0; i < e->cfg.nsensors; i++)
        emit_window(e, i);
}

unsigned long long engine_rejected_total(const struct engine *e)
{
    unsigned long long total = 0;
    for (int i = 0; i < REJECT_REASONS; i++)
        total += e->rejected[i];
    return total;
}
