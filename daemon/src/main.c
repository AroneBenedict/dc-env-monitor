/* envmond: data-centre environment monitoring daemon.
 *
 * Reads sensor samples ("<unix_time> <sensor_id> <value>") from standard input, raises and
 * clears alarms, aggregates readings, and delivers everything to a collector over TCP with
 * on-disk store-and-forward. A hardware build would replace standard input with the sensor
 * bus driver; everything else stays the same.
 *
 *   envmond -c /etc/envmond.conf [--drain-timeout SECONDS] [--no-hysteresis] [-v]
 *
 * Signals: SIGTERM/SIGINT stop after flushing; SIGHUP reloads the configuration.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "engine.h"
#include "log.h"
#include "net.h"
#include "spool.h"

static volatile sig_atomic_t stop_requested, reload_requested;

static void on_signal(int sig)
{
    if (sig == SIGHUP)
        reload_requested = 1;
    else
        stop_requested = 1;
}

static struct spool spool; /* large (in-flight table), so not on the stack */

static int emit(void *ctx, const char *msg, size_t len)
{
    return spool_append((struct spool *)ctx, msg, len);
}

static unsigned long long take_seq(void *ctx)
{
    return spool_take_seq((struct spool *)ctx);
}

static long long backlog(void *ctx)
{
    return spool_backlog((struct spool *)ctx);
}

static void flatten(struct config *cfg)
{
    for (int i = 0; i < cfg->nsensors; i++) {
        cfg->sensors[i].hysteresis = 0;
        cfg->sensors[i].delay = 0;
    }
}

static void pump(struct link *l, struct spool *sp, const struct config *cfg)
{
    if (l->fd < 0 && link_maybe_connect(l, cfg->collector_host, cfg->collector_port) && !l->connecting) {
        l->backoff = 0.25;
        spool_rewind(sp);
    }
    if (l->fd < 0 || l->connecting)
        return;
    for (int rounds = 0; rounds < 64; rounds++) {
        const char *buf;
        size_t len = spool_peek(sp, &buf);
        if (len == 0)
            break;
        long n = link_send(l, buf, len);
        if (n < 0) {
            spool_rewind(sp);
            return;
        }
        if (n == 0)
            break;
        spool_sent(sp, (size_t)n);
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: envmond -c CONFIG [--drain-timeout SECONDS] [--no-hysteresis] [-v]\n");
}

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    double drain_timeout = 10;
    int no_hysteresis = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            config_path = argv[++i];
        else if (strcmp(argv[i], "--drain-timeout") == 0 && i + 1 < argc)
            drain_timeout = atof(argv[++i]);
        else if (strcmp(argv[i], "--no-hysteresis") == 0)
            no_hysteresis = 1;
        else if (strcmp(argv[i], "-v") == 0)
            log_threshold = LOG_DEBUG;
        else {
            usage();
            return 2;
        }
    }
    if (!config_path) {
        usage();
        return 2;
    }

    static struct config cfg;
    char err[256];
    if (config_load(config_path, &cfg, err, sizeof err) < 0) {
        log_msg(LOG_ERROR, "%s", err);
        return 2;
    }
    if (no_hysteresis)
        flatten(&cfg);
    if (spool_open(&spool, cfg.spool_dir, cfg.spool_max_bytes) < 0) {
        log_msg(LOG_ERROR, "cannot open spool in %s: %s", cfg.spool_dir, strerror(errno));
        return 2;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    static struct engine engine;
    engine_init(&engine, &cfg, emit, take_seq, &spool);
    engine.backlog = backlog;
    struct link link;
    link_init(&link);
    log_msg(LOG_INFO, "device %s: %d sensors, collector %s:%d, %lld bytes waiting in spool", cfg.device_id,
            cfg.nsensors, cfg.collector_host, cfg.collector_port, spool_backlog(&spool));

    int fl = fcntl(STDIN_FILENO, F_GETFL);
    fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
    char inbuf[64 * 1024];
    size_t inlen = 0;
    int input_open = 1;
    double drain_deadline = 0;

    while (!stop_requested) {
        if (reload_requested) {
            reload_requested = 0;
            static struct config fresh;
            if (config_load(config_path, &fresh, err, sizeof err) == 0) {
                if (no_hysteresis)
                    flatten(&fresh);
                engine_reload(&engine, &fresh);
                cfg = fresh;
                log_msg(LOG_INFO, "configuration reloaded: %d sensors", cfg.nsensors);
            } else {
                log_msg(LOG_ERROR, "reload failed, keeping old configuration: %s", err);
            }
        }

        pump(&link, &spool, &cfg);

        if (!input_open) {
            if (spool_backlog(&spool) == 0)
                break;
            if (now_monotonic() > drain_deadline) {
                log_msg(LOG_WARN, "drain timeout: %lld bytes left in spool for next start", spool_backlog(&spool));
                break;
            }
        }

        struct pollfd fds[2];
        int nfds = 0;
        if (input_open && spool_backlog(&spool) < cfg.spool_max_bytes)
            fds[nfds++] = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN};
        int link_slot = -1;
        if (link.fd >= 0) {
            short ev = POLLIN;
            const char *dummy;
            if (link.connecting || spool_peek(&spool, &dummy) > 0)
                ev |= POLLOUT;
            link_slot = nfds;
            fds[nfds++] = (struct pollfd){.fd = link.fd, .events = ev};
        }
        int rc = poll(fds, (nfds_t)nfds, 200);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            log_msg(LOG_ERROR, "poll: %s", strerror(errno));
            break;
        }

        if (link_slot >= 0 && fds[link_slot].revents) {
            if (link.connecting && (fds[link_slot].revents & (POLLOUT | POLLERR | POLLHUP))) {
                if (link_finish_connect(&link))
                    spool_rewind(&spool);
            } else if (fds[link_slot].revents & (POLLIN | POLLERR | POLLHUP)) {
                long long acked = link_read_acks(&link);
                if (acked > 0)
                    spool_ack(&spool, (unsigned long long)acked);
                else if (acked < 0)
                    spool_rewind(&spool);
            }
        }

        if (input_open && nfds > 0 && fds[0].fd == STDIN_FILENO && (fds[0].revents & (POLLIN | POLLHUP))) {
            ssize_t n = read(STDIN_FILENO, inbuf + inlen, sizeof inbuf - inlen);
            if (n > 0) {
                inlen += (size_t)n;
                size_t start = 0;
                for (size_t i = 0; i < inlen; i++) {
                    if (inbuf[i] == '\n') {
                        engine_feed(&engine, inbuf + start, i - start);
                        start = i + 1;
                    }
                }
                memmove(inbuf, inbuf + start, inlen - start);
                inlen -= start;
                if (inlen == sizeof inbuf) { /* a "line" filling the whole buffer: discard it */
                    engine.rejected[REJ_TOO_LONG]++;
                    inlen = 0;
                }
            } else if (n == 0) {
                if (inlen)
                    engine_feed(&engine, inbuf, inlen);
                engine_flush(&engine);
                input_open = 0;
                drain_deadline = now_monotonic() + drain_timeout;
                log_msg(LOG_INFO, "input closed; delivering %lld spooled bytes", spool_backlog(&spool));
            } else if (errno != EAGAIN && errno != EINTR) {
                input_open = 0;
                drain_deadline = now_monotonic() + drain_timeout;
            }
        }
    }

    if (input_open)
        engine_flush(&engine);
    spool_close(&spool);
    fprintf(stderr,
            "{\"event\":\"exit\",\"last_seq\":%llu,\"acked_seq\":%llu,\"accepted\":%llu,\"rejected\":%llu,"
            "\"dropped\":%llu,\"alarms\":%llu,\"reconnects\":%llu,\"backlog_bytes\":%lld",
            spool.next_seq - 1, spool.acked_seq, engine.accepted, engine_rejected_total(&engine), engine.dropped,
            engine.alarms, link.reconnects, spool_backlog(&spool));
    for (int r = 0; r < REJECT_REASONS; r++)
        fprintf(stderr, ",\"rej_%s\":%llu", reject_name((enum reject_reason)r), engine.rejected[r]);
    fprintf(stderr, "}\n");
    return spool_backlog(&spool) == 0 ? 0 : 3;
}
