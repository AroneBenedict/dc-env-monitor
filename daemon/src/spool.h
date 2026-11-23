/* Store-and-forward spool.
 *
 * Every outgoing message is appended to spool.log before it is sent. The collector replies
 * "ACK <seq>" once a message is committed to its database; only then is the message
 * considered delivered. If the collector or the network is down, messages wait on disk and
 * are resent in order on reconnect, across daemon restarts too. Resends can produce
 * duplicates, which the collector discards by (device, seq): at-least-once delivery plus
 * idempotent storage gives each message exactly once in the database.
 */
#ifndef ENVMOND_SPOOL_H
#define ENVMOND_SPOOL_H

#include <stddef.h>

#include "common.h"

#define SPOOL_INFLIGHT 65536

struct inflight {
    unsigned long long seq;
    long long end;          /* offset just past this message */
};

struct spool {
    char log_path[MAX_PATH + 16];
    char state_path[MAX_PATH + 16];
    int fd;
    long long size;          /* bytes in spool.log */
    long long ack_off;       /* everything before this offset is delivered */
    long long send_off;      /* everything before this offset has been sent on the current connection */
    long long max_bytes;
    long long compact_at;    /* truncate the log once it is fully acknowledged and at least this big */
    unsigned long long next_seq;
    unsigned long long acked_seq;
    struct inflight q[SPOOL_INFLIGHT];
    size_t q_head, q_len;
    char chunk[64 * 1024];
    size_t chunk_len;         /* bytes of whole lines read into chunk[] */
    size_t chunk_pos;         /* bytes of chunk[] already written to the socket */
    size_t line_start;        /* start of the line currently being sent */
    long long chunk_base;     /* file offset of chunk[0] */
};

int spool_open(struct spool *sp, const char *dir, long long max_bytes);
void spool_close(struct spool *sp);

/* Reserve the next sequence number. */
unsigned long long spool_take_seq(struct spool *sp);

/* Append one complete message line. Returns -1 if the spool is full or the write failed. */
int spool_append(struct spool *sp, const char *line, size_t len);

/* Start sending again from the first unacknowledged byte (after a reconnect). */
void spool_rewind(struct spool *sp);

/* Bytes ready to send, up to the end of the last whole line. *out points into the spool. */
size_t spool_peek(struct spool *sp, const char **out);

/* Record that n bytes from spool_peek() were written to the socket. */
void spool_sent(struct spool *sp, size_t n);

/* The collector has committed everything up to and including seq. */
void spool_ack(struct spool *sp, unsigned long long seq);

/* Persist the acknowledged offset and sequence counters (atomic rename). */
int spool_save_state(struct spool *sp);

long long spool_backlog(const struct spool *sp);

#endif
