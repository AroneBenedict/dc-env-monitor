#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

#define BACKOFF_MIN 0.25
#define BACKOFF_MAX 30.0

double now_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void link_init(struct link *l)
{
    memset(l, 0, sizeof *l);
    l->fd = -1;
    l->backoff = BACKOFF_MIN;
}

void link_drop(struct link *l, const char *why)
{
    if (l->fd >= 0) {
        log_msg(LOG_WARN, "collector link down (%s); retrying in %.2fs", why, l->backoff);
        close(l->fd);
    }
    l->fd = -1;
    l->connecting = 0;
    l->rx_len = 0;
    l->next_attempt = now_monotonic() + l->backoff;
    l->backoff = l->backoff * 2 > BACKOFF_MAX ? BACKOFF_MAX : l->backoff * 2;
}

int link_maybe_connect(struct link *l, const char *host, int port)
{
    if (l->fd >= 0 || now_monotonic() < l->next_attempt)
        return 0;
    char service[8];
    snprintf(service, sizeof service, "%d", port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, service, &hints, &res) != 0 || !res) {
        l->next_attempt = now_monotonic() + l->backoff;
        return 0;
    }
    int fd = socket(res->ai_family, res->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return 0;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    l->fd = fd;
    if (rc == 0) {
        l->connecting = 0;
        return 1;
    }
    if (errno == EINPROGRESS) {
        l->connecting = 1;
        return 1;
    }
    link_drop(l, strerror(errno));
    return 0;
}

int link_finish_connect(struct link *l)
{
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(l->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err) {
        link_drop(l, err ? strerror(err) : "connect failed");
        return 0;
    }
    l->connecting = 0;
    l->backoff = BACKOFF_MIN;
    l->reconnects++;
    log_msg(LOG_INFO, "connected to collector");
    return 1;
}

long link_send(struct link *l, const char *buf, size_t len)
{
    if (l->fd < 0 || l->connecting || len == 0)
        return 0;
    ssize_t n = send(l->fd, buf, len, MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;
        link_drop(l, strerror(errno));
        return -1;
    }
    return (long)n;
}

long long link_read_acks(struct link *l)
{
    if (l->fd < 0 || l->connecting)
        return 0;
    long long best = 0;
    for (;;) {
        ssize_t n = recv(l->fd, l->rx + l->rx_len, sizeof l->rx - 1 - l->rx_len, 0);
        if (n == 0) {
            link_drop(l, "collector closed the connection");
            return -1;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            link_drop(l, strerror(errno));
            return -1;
        }
        l->rx_len += (size_t)n;
        l->rx[l->rx_len] = '\0';
        char *start = l->rx, *nl;
        while ((nl = strchr(start, '\n'))) {
            *nl = '\0';
            unsigned long long seq;
            if (sscanf(start, "ACK %llu", &seq) == 1 && (long long)seq > best)
                best = (long long)seq;
            start = nl + 1;
        }
        l->rx_len = strlen(start);
        memmove(l->rx, start, l->rx_len);
        if (l->rx_len >= sizeof l->rx - 1) { /* garbage without newlines: protocol error */
            link_drop(l, "protocol error");
            return -1;
        }
    }
    return best;
}
