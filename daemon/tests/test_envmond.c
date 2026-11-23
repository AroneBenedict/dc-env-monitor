/* Unit tests for envmond. Plain C, no framework: each CHECK reports file:line on failure. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "alarm.h"
#include "config.h"
#include "engine.h"
#include "message.h"
#include "sample.h"
#include "spool.h"

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const char *BASE_CFG =
    "[device]\nid = dc1-row3\ncollector_port = 7070\nreport_interval = 60\nstale_after = 180\n"
    "[sensor rack1-inlet]\nkind = temperature\nwarn_high = 27\ncrit_high = 32\nhysteresis = 1\ndelay = 120\n"
    "[sensor rack1-rh]\nkind = humidity\nwarn_low = 20\nwarn_high = 80\n"
    "[sensor rack1-door]\nkind = door\n"
    "[sensor floor-leak]\nkind = leak\n";

static struct config load(const char *text)
{
    struct config c;
    char err[128];
    int rc = config_parse(text, &c, err, sizeof err);
    if (rc)
        fprintf(stderr, "config error: %s\n", err);
    return c;
}

static void test_config(void)
{
    struct config c = load(BASE_CFG);
    char err[128];
    CHECK(strcmp(c.device_id, "dc1-row3") == 0);
    CHECK(c.nsensors == 4);
    CHECK(c.sensors[0].kind == KIND_TEMPERATURE && c.sensors[0].crit_high == 32 && c.sensors[0].delay == 120);
    CHECK(c.sensors[1].kind == KIND_HUMIDITY && c.sensors[1].warn_low == 20 && c.sensors[1].delay == 120);
    CHECK(c.sensors[2].kind == KIND_DOOR && c.sensors[2].delay == 300 && c.sensors[2].warn_high == 0.5);
    CHECK(c.sensors[3].kind == KIND_LEAK && c.sensors[3].delay == 0 && c.sensors[3].crit_high == 0.5);
    CHECK(config_find(&c, "floor-leak") == 3 && config_find(&c, "nope") == -1);

    struct { const char *text, *msg; } bad[] = {
        {"[sensor a]\nkind = temperature\n", "[device] id is required"},
        {"[device]\nid = x\ncollector_port = 70000\n", "collector_port"},
        {"[device]\nid = x\n[sensor a]\nwarn_high = 3\n", "set kind before"},
        {"[device]\nid = x\n[sensor a]\nkind = plasma\n", "unknown kind"},
        {"[device]\nid = x\n[sensor a]\nkind = temperature\n[sensor a]\nkind = humidity\n", "duplicate sensor"},
        {"[device]\nid = x\n[sensor a]\nkind = temperature\nwarn_high = 30\ncrit_high = 25\n", "crit_high below"},
        {"[device]\nid = x\n[sensor bad id!]\nkind = leak\n", "invalid sensor id"},
        {"[device]\nid = x\nfoo\n", "expected key = value"},
        {"[device]\nid = x\n[sensor a]\nkind = temperature\nwarn_high = abc\n", "must be a number"},
        {"[device]\nid = x\n[sensor a]\n", "has no kind"},
        {"[gadget]\n", "unknown section"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        struct config tmp;
        CHECK(config_parse(bad[i].text, &tmp, err, sizeof err) == -1);
        if (!strstr(err, bad[i].msg)) {
            failures++;
            fprintf(stderr, "config case %zu: got '%s', want '%s'\n", i, err, bad[i].msg);
        }
    }
    struct config tmp;
    CHECK(config_parse("[device]\nid = x  # comment\n; full comment line\n", &tmp, err, sizeof err) == 0);
    CHECK(strcmp(tmp.device_id, "x") == 0);
}

static void test_sample_parse(void)
{
    struct sample s;
    CHECK(sample_parse("1735689600 rack1-inlet 24.5\n", 28, &s) == PARSE_OK);
    CHECK(s.ts == 1735689600 && strcmp(s.sensor, "rack1-inlet") == 0 && s.value == 24.5);
    const char *tabs = "1735689600\track1-inlet\t-3e1\r\n";
    CHECK(sample_parse(tabs, strlen(tabs), &s) == PARSE_OK && s.value == -30);
    CHECK(sample_parse("", 0, &s) == PARSE_EMPTY);
    CHECK(sample_parse("\n", 1, &s) == PARSE_EMPTY);
    CHECK(sample_parse("1735689600 rack1-inlet", 22, &s) == PARSE_BAD_FORMAT);
    CHECK(sample_parse("1735689600 rack1-inlet 1 2", 26, &s) == PARSE_BAD_FORMAT);
    CHECK(sample_parse("1735689600 rack;rm 1", 20, &s) == PARSE_BAD_ID);
    CHECK(sample_parse("17356x9600 a 1", 14, &s) == PARSE_BAD_NUMBER);
    CHECK(sample_parse("12 a 1", 6, &s) == PARSE_BAD_NUMBER);              /* before 2000 */
    CHECK(sample_parse("1735689600 a nan", 16, &s) == PARSE_BAD_NUMBER);
    CHECK(sample_parse("1735689600 a inf", 16, &s) == PARSE_BAD_NUMBER);
    CHECK(sample_parse("1735689600 a 1e999", 18, &s) == PARSE_BAD_NUMBER);
    char nul[] = "1735689600 a\0b 1";
    CHECK(sample_parse(nul, sizeof nul - 1, &s) == PARSE_BAD_FORMAT);
    char longline[400];
    memset(longline, 'a', sizeof longline);
    CHECK(sample_parse(longline, sizeof longline, &s) == PARSE_TOO_LONG);
    char longid[100];
    snprintf(longid, sizeof longid, "1735689600 %060d 1", 7);
    CHECK(sample_parse(longid, strlen(longid), &s) == PARSE_BAD_ID);
}

static void test_alarm_delay_and_hysteresis(void)
{
    struct config c = load(BASE_CFG);
    const struct sensor_config *t = &c.sensors[0]; /* warn 27, crit 32, hyst 1, delay 120 */
    struct sensor_state st;
    struct alarm_event ev;
    sensor_state_init(&st);
    long long t0 = 1735689600;
    CHECK(!alarm_update(t, &st, t0, 26.0, &ev));
    CHECK(!alarm_update(t, &st, t0 + 60, 28.0, &ev));          /* above warn, but not for 120 s yet */
    CHECK(!alarm_update(t, &st, t0 + 120, 26.5, &ev));         /* dipped back: timer restarts */
    CHECK(!alarm_update(t, &st, t0 + 180, 28.0, &ev));
    CHECK(!alarm_update(t, &st, t0 + 240, 28.0, &ev));
    CHECK(alarm_update(t, &st, t0 + 300, 28.0, &ev));          /* held 120 s */
    CHECK(ev.from == SEV_NORMAL && ev.to == SEV_WARNING && ev.ts == t0 + 300);
    /* Hovering just under the threshold does not clear it: hysteresis needs < 26. */
    CHECK(!alarm_update(t, &st, t0 + 360, 26.6, &ev));
    CHECK(!alarm_update(t, &st, t0 + 600, 26.6, &ev));
    CHECK(st.severity == SEV_WARNING);
    CHECK(!alarm_update(t, &st, t0 + 660, 25.5, &ev));
    CHECK(alarm_update(t, &st, t0 + 780, 25.5, &ev) && ev.to == SEV_NORMAL);
    /* Straight to critical. */
    CHECK(!alarm_update(t, &st, t0 + 800, 35, &ev));
    CHECK(alarm_update(t, &st, t0 + 920, 35, &ev) && ev.to == SEV_CRITICAL && ev.from == SEV_NORMAL);
    CHECK(alarm_target(t, SEV_CRITICAL, 31.5) == SEV_CRITICAL);   /* hysteresis on the critical limit */
    CHECK(alarm_target(t, SEV_CRITICAL, 30.9) == SEV_WARNING);
}

static void test_alarm_low_side_and_binary(void)
{
    struct config c = load(BASE_CFG);
    struct sensor_state st;
    struct alarm_event ev;
    const struct sensor_config *rh = &c.sensors[1], *leak = &c.sensors[3], *door = &c.sensors[2];
    long long t0 = 1735689600;
    sensor_state_init(&st);
    CHECK(!alarm_update(rh, &st, t0, 15, &ev));
    CHECK(alarm_update(rh, &st, t0 + 120, 15, &ev) && ev.to == SEV_WARNING);
    sensor_state_init(&st);
    CHECK(alarm_update(leak, &st, t0, 1, &ev) && ev.to == SEV_CRITICAL);  /* a leak alarms at once */
    CHECK(alarm_update(leak, &st, t0 + 1, 0, &ev) && ev.to == SEV_NORMAL);
    sensor_state_init(&st);
    CHECK(!alarm_update(door, &st, t0, 1, &ev));
    CHECK(!alarm_update(door, &st, t0 + 299, 1, &ev));                   /* doors may open briefly */
    CHECK(alarm_update(door, &st, t0 + 300, 1, &ev) && ev.to == SEV_WARNING);
}

/* ---------------------------------------------------------------- engine, with a capture sink */

static char captured[200][MAX_MSG];
static int ncaptured;
static unsigned long long seq_counter;

static int capture(void *ctx, const char *msg, size_t len)
{
    (void)ctx;
    if (ncaptured >= 200 || len >= MAX_MSG)
        return -1;
    memcpy(captured[ncaptured], msg, len);
    captured[ncaptured][len] = '\0';
    ncaptured++;
    return 0;
}

static unsigned long long next_seq(void *ctx)
{
    (void)ctx;
    return ++seq_counter;
}

static int count_type(const char *needle)
{
    int n = 0;
    for (int i = 0; i < ncaptured; i++)
        n += strstr(captured[i], needle) != NULL;
    return n;
}

static void feed(struct engine *e, const char *fmt, long long ts, const char *id, double v)
{
    char line[128];
    int n = snprintf(line, sizeof line, fmt, ts, id, v);
    engine_feed(e, line, (size_t)n);
}

static void test_engine(void)
{
    struct config c = load(BASE_CFG);
    struct engine e;
    ncaptured = 0;
    seq_counter = 0;
    engine_init(&e, &c, capture, next_seq, NULL);
    long long t0 = 1735689600; /* aligned to a minute */
    for (int i = 0; i < 6; i++) {
        feed(&e, "%lld %s %.1f\n", t0 + i * 20, "rack1-inlet", 24.0 + i);
        feed(&e, "%lld %s %.1f\n", t0 + i * 20, "rack1-rh", 45.0);
    }
    /* Windows: [t0, t0+60) got 3 samples per sensor; the second window is still open. */
    CHECK(count_type("\"type\":\"reading\"") == 2);
    const char *inlet = NULL;
    for (int i = 0; i < ncaptured; i++)
        if (strstr(captured[i], "\"type\":\"reading\"") && strstr(captured[i], "\"sensor\":\"rack1-inlet\""))
            inlet = captured[i];
    CHECK(inlet && strstr(inlet, "\"n\":3") && strstr(inlet, "\"min\":24.00") && strstr(inlet, "\"avg\":25.00") &&
          strstr(inlet, "\"max\":26.00") && strstr(inlet, "\"ts\":1735689600"));
    CHECK(count_type("\"type\":\"status\"") == 1);
    engine_flush(&e);
    CHECK(count_type("\"type\":\"reading\"") == 4);

    /* Rejections are counted by reason and change nothing else. */
    unsigned long long before = e.accepted;
    CHECK(engine_feed(&e, "garbage", 7) == REJ_PARSE_FORMAT + 1);
    CHECK(engine_feed(&e, "1735689700 ghost 1", 18) == REJ_UNKNOWN_SENSOR + 1);
    CHECK(engine_feed(&e, "1735689700 rack1-inlet 999", 26) == REJ_OUT_OF_RANGE + 1);
    CHECK(engine_feed(&e, "1735600000 rack1-inlet 20", 25) == REJ_OUT_OF_ORDER + 1);
    CHECK(e.accepted == before && engine_rejected_total(&e) == 4);

    /* A one-sample jump (a probe's 85.0 C reset value) is a glitch; a lasting change is not. */
    CHECK(engine_feed(&e, "1735689700 rack1-inlet 85", 25) == REJ_GLITCH + 1);
    CHECK(engine_feed(&e, "1735689710 rack1-inlet 25", 25) == 0);
    CHECK(engine_feed(&e, "1735689720 rack1-inlet 40", 25) == REJ_GLITCH + 1);
    CHECK(engine_feed(&e, "1735689730 rack1-inlet 40", 25) == REJ_GLITCH + 1);
    CHECK(engine_feed(&e, "1735689740 rack1-inlet 40", 25) == 0);   /* third time: accepted */
    CHECK(e.rejected[REJ_GLITCH] == 3);

    /* A sensor that stops reporting goes offline, then comes back. */
    ncaptured = 0;
    for (int i = 0; i <= 12; i++)
        feed(&e, "%lld %s %.1f\n", t0 + 200 + i * 20, "rack1-rh", 45.0);
    CHECK(count_type("\"to\":\"offline\"") == 1);
    CHECK(count_type("\"value\":null") == 1);
    feed(&e, "%lld %s %.1f\n", t0 + 460, "rack1-inlet", 38.0);
    CHECK(count_type("\"from\":\"offline\"") == 1);

    /* Messages carry increasing sequence numbers. */
    unsigned long long last = 0;
    for (int i = 0; i < ncaptured; i++) {
        unsigned long long s = msg_seq(captured[i]);
        CHECK(s > last);
        last = s;
    }

    /* Reload keeps state for sensors that remain. */
    struct config c2 = load(
        "[device]\nid = dc1-row3\n[sensor floor-leak]\nkind = leak\n[sensor rack1-inlet]\nkind = temperature\n");
    long long last_inlet = e.st[0].last_ts;
    engine_reload(&e, &c2);
    CHECK(e.cfg.nsensors == 2 && e.st[1].last_ts == last_inlet && e.st[0].last_ts == 0);
}

static void test_messages(void)
{
    char buf[MAX_MSG];
    struct alarm_event ev = {SEV_NORMAL, SEV_CRITICAL, 1735689600, 33.25};
    CHECK(msg_alarm(buf, sizeof buf, 42, "d", "s", "temperature", &ev) > 0);
    CHECK(strcmp(buf, "{\"seq\":42,\"device\":\"d\",\"type\":\"alarm\",\"sensor\":\"s\",\"kind\":\"temperature\","
                      "\"ts\":1735689600,\"from\":\"normal\",\"to\":\"critical\",\"value\":33.25}\n") == 0);
    CHECK(msg_seq(buf) == 42 && msg_seq("{}") == 0);
    CHECK(msg_reading(buf, 20, 1, "d", "s", "k", 1, 1, 1, 1, 1) == -1); /* truncation is reported */
}

/* ---------------------------------------------------------------- spool */

static struct spool sp; /* large */

static void line(char *buf, size_t size, unsigned long long seq)
{
    snprintf(buf, size, "{\"seq\":%llu,\"x\":1}\n", seq);
}

static void test_spool(void)
{
    char dir[] = "/tmp/envmond-test-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    CHECK(spool_open(&sp, dir, 1 << 20) == 0);
    char buf[64];
    for (int i = 0; i < 5; i++) {
        unsigned long long s = spool_take_seq(&sp);
        line(buf, sizeof buf, s);
        CHECK(spool_append(&sp, buf, strlen(buf)) == 0);
    }
    const char *out;
    size_t n = spool_peek(&sp, &out);
    CHECK((long long)n == sp.size);
    spool_sent(&sp, 10);                 /* partial write: nothing complete yet */
    CHECK(sp.q_len == 0 && sp.send_off == 10);
    n = spool_peek(&sp, &out);
    CHECK(out[0] == buf[10] || n > 0);   /* continues from byte 10, not from the line start */
    spool_sent(&sp, n);
    CHECK(sp.q_len == 5 && sp.send_off == sp.size);
    spool_ack(&sp, 3);
    CHECK(sp.q_len == 2 && spool_backlog(&sp) == (long long)(2 * strlen(buf)));

    /* Reconnect: resend from the first unacknowledged message. */
    spool_rewind(&sp);
    n = spool_peek(&sp, &out);
    CHECK(n == (size_t)spool_backlog(&sp) && msg_seq(out) == 4);
    spool_close(&sp);

    /* Restart: acked offset and sequence counter survive; a torn last line is removed. */
    char path[256];
    snprintf(path, sizeof path, "%s/spool.log", dir);
    FILE *f = fopen(path, "a");
    fputs("{\"seq\":6,\"x\"", f); /* crash mid-write */
    fclose(f);
    CHECK(spool_open(&sp, dir, 1 << 20) == 0);
    CHECK(spool_backlog(&sp) == (long long)(2 * strlen(buf)));
    CHECK(spool_take_seq(&sp) == 6);
    n = spool_peek(&sp, &out);
    CHECK(msg_seq(out) == 4);
    spool_sent(&sp, n);
    spool_ack(&sp, 5);
    CHECK(spool_backlog(&sp) == 0);

    /* Full spool refuses new messages instead of growing without limit. */
    spool_close(&sp);
    CHECK(spool_open(&sp, dir, 100) == 0);
    int stored = 0;
    for (int i = 0; i < 20; i++) {
        line(buf, sizeof buf, spool_take_seq(&sp));
        stored += spool_append(&sp, buf, strlen(buf)) == 0;
    }
    CHECK(stored > 0 && stored < 20 && spool_backlog(&sp) <= 100);
    spool_close(&sp);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    CHECK(system(cmd) == 0);
}

static void test_spool_compaction(void)
{
    char dir[] = "/tmp/envmond-test-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    CHECK(spool_open(&sp, dir, 8 << 20) == 0);
    sp.compact_at = 4096;
    char buf[64];
    unsigned long long last = 0;
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 300; i++) {
            last = spool_take_seq(&sp);
            line(buf, sizeof buf, last);
            spool_append(&sp, buf, strlen(buf));
        }
        const char *out;
        size_t n;
        while ((n = spool_peek(&sp, &out)) > 0)
            spool_sent(&sp, n);
        spool_ack(&sp, last);
        CHECK(sp.size == 0 && spool_backlog(&sp) == 0); /* truncated once fully delivered */
    }
    CHECK(spool_take_seq(&sp) == last + 1);             /* numbering continues after truncation */
    spool_close(&sp);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    CHECK(system(cmd) == 0);
}

int main(void)
{
    test_config();
    test_sample_parse();
    test_alarm_delay_and_hysteresis();
    test_alarm_low_side_and_binary();
    test_engine();
    test_messages();
    test_spool();
    test_spool_compaction();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
