#include "spool.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "message.h"

static int write_all(int fd, const char *buf, size_t len)
{
    while (len) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

int spool_save_state(struct spool *sp)
{
    char tmp[sizeof sp->state_path + 4];
    snprintf(tmp, sizeof tmp, "%s.tmp", sp->state_path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    fprintf(f, "%lld %llu %llu\n", sp->ack_off, sp->next_seq, sp->acked_seq);
    if (fflush(f) || fsync(fileno(f))) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return rename(tmp, sp->state_path);
}

/* Drop a half-written last line left by a crash, then find the highest sequence number. */
static int recover(struct spool *sp)
{
    long long keep = 0, pos = 0;
    unsigned long long max_seq = 0;
    char line[MAX_MSG + 1];
    size_t used = 0;
    char buf[8192];
    if (lseek(sp->fd, 0, SEEK_SET) < 0)
        return -1;
    for (;;) {
        ssize_t n = read(sp->fd, buf, sizeof buf);
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        for (ssize_t i = 0; i < n; i++, pos++) {
            if (used < MAX_MSG)
                line[used++] = buf[i];
            if (buf[i] == '\n') {
                line[used] = '\0';
                unsigned long long seq = msg_seq(line);
                if (seq > max_seq)
                    max_seq = seq;
                keep = pos + 1;
                used = 0;
            }
        }
    }
    if (keep != pos && ftruncate(sp->fd, keep) < 0)
        return -1;
    sp->size = keep;
    if (max_seq + 1 > sp->next_seq)
        sp->next_seq = max_seq + 1;
    return 0;
}

int spool_open(struct spool *sp, const char *dir, long long max_bytes)
{
    memset(sp, 0, offsetof(struct spool, q));
    sp->q_head = sp->q_len = 0;
    sp->chunk_len = sp->chunk_pos = sp->line_start = 0;
    sp->chunk_base = 0;
    snprintf(sp->log_path, sizeof sp->log_path, "%s/spool.log", dir);
    snprintf(sp->state_path, sizeof sp->state_path, "%s/spool.state", dir);
    sp->max_bytes = max_bytes;
    sp->compact_at = 1024 * 1024;
    sp->next_seq = 1;

    FILE *f = fopen(sp->state_path, "r");
    if (f) {
        long long off;
        unsigned long long next, acked;
        if (fscanf(f, "%lld %llu %llu", &off, &next, &acked) == 3 && off >= 0) {
            sp->ack_off = off;
            sp->next_seq = next;
            sp->acked_seq = acked;
        }
        fclose(f);
    }
    sp->fd = open(sp->log_path, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (sp->fd < 0)
        return -1;
    if (recover(sp) < 0)
        return -1;
    if (sp->ack_off > sp->size)
        sp->ack_off = sp->size;
    sp->send_off = sp->ack_off;
    return 0;
}

void spool_close(struct spool *sp)
{
    if (sp->fd >= 0) {
        spool_save_state(sp);
        fsync(sp->fd);
        close(sp->fd);
        sp->fd = -1;
    }
}

unsigned long long spool_take_seq(struct spool *sp)
{
    return sp->next_seq++;
}

int spool_append(struct spool *sp, const char *line, size_t len)
{
    if (sp->size - sp->ack_off + (long long)len > sp->max_bytes)
        return -1;
    if (write_all(sp->fd, line, len) < 0)
        return -1;
    sp->size += (long long)len;
    return 0;
}

void spool_rewind(struct spool *sp)
{
    sp->send_off = sp->ack_off;
    sp->q_head = sp->q_len = 0;
    sp->chunk_len = sp->chunk_pos = 0;
}

size_t spool_peek(struct spool *sp, const char **out)
{
    if (sp->chunk_len == 0 && sp->send_off < sp->size && sp->q_len < SPOOL_INFLIGHT - 1024) {
        size_t want = sizeof sp->chunk;
        if ((long long)want > sp->size - sp->send_off)
            want = (size_t)(sp->size - sp->send_off);
        ssize_t n = pread(sp->fd, sp->chunk, want, sp->send_off);
        if (n > 0) {
            size_t len = (size_t)n;
            while (len && sp->chunk[len - 1] != '\n') /* only whole lines, so every line sits inside one chunk */
                len--;
            sp->chunk_len = len;
            sp->chunk_pos = 0;
            sp->line_start = 0;
            sp->chunk_base = sp->send_off;
        }
    }
    *out = sp->chunk + sp->chunk_pos;
    return sp->chunk_len - sp->chunk_pos;
}

void spool_sent(struct spool *sp, size_t n)
{
    /* Partial writes are normal on a non-blocking socket: the rest of the line goes out next
       time, from exactly where this write stopped. A message counts as in flight once its
       newline has been written. */
    size_t end = sp->chunk_pos + n;
    for (size_t i = sp->chunk_pos; i < end; i++) {
        if (sp->chunk[i] != '\n')
            continue;
        char line[MAX_MSG + 1];
        size_t len = i + 1 - sp->line_start;
        if (len > MAX_MSG)
            len = MAX_MSG;
        memcpy(line, sp->chunk + sp->line_start, len);
        line[len] = '\0';
        if (sp->q_len < SPOOL_INFLIGHT) {
            size_t slot = (sp->q_head + sp->q_len) % SPOOL_INFLIGHT;
            sp->q[slot].seq = msg_seq(line);
            sp->q[slot].end = sp->chunk_base + (long long)i + 1;
            sp->q_len++;
        }
        sp->line_start = i + 1;
    }
    sp->chunk_pos = end;
    sp->send_off = sp->chunk_base + (long long)end;
    if (sp->chunk_pos == sp->chunk_len)
        sp->chunk_len = sp->chunk_pos = 0;
}

void spool_ack(struct spool *sp, unsigned long long seq)
{
    int moved = 0;
    while (sp->q_len && sp->q[sp->q_head].seq <= seq) {
        sp->ack_off = sp->q[sp->q_head].end;
        sp->q_head = (sp->q_head + 1) % SPOOL_INFLIGHT;
        sp->q_len--;
        moved = 1;
    }
    if (seq > sp->acked_seq)
        sp->acked_seq = seq;
    if (moved && sp->ack_off == sp->size && sp->size >= sp->compact_at && sp->chunk_len == 0) {
        if (ftruncate(sp->fd, 0) == 0) {
            sp->size = sp->ack_off = sp->send_off = 0;
        }
    }
    if (moved)
        spool_save_state(sp);
}

long long spool_backlog(const struct spool *sp)
{
    return sp->size - sp->ack_off;
}
