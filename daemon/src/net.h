/* Non-blocking TCP link to the collector, with exponential reconnect back-off. */
#ifndef ENVMOND_NET_H
#define ENVMOND_NET_H

#include <stddef.h>

struct link {
    int fd;                 /* -1 when disconnected */
    int connecting;         /* non-blocking connect in progress */
    double next_attempt;    /* monotonic time of the next connect attempt */
    double backoff;         /* seconds */
    char rx[1024];
    size_t rx_len;
    unsigned long long reconnects;
};

void link_init(struct link *l);
double now_monotonic(void);

/* Start a connection if due. Returns 1 if a connect was started. */
int link_maybe_connect(struct link *l, const char *host, int port);

/* Call when poll() reports the socket writable during connect. Returns 1 when connected. */
int link_finish_connect(struct link *l);

void link_drop(struct link *l, const char *why);

/* Write as much as the socket accepts. Returns bytes written, or -1 after dropping the link. */
long link_send(struct link *l, const char *buf, size_t len);

/* Read acknowledgements. Returns the highest "ACK <seq>" received (0 if none), -1 if the link dropped. */
long long link_read_acks(struct link *l);

#endif
