/* JSON-lines messages from the device to the collector. Every message carries a sequence
 * number so the collector can drop resent duplicates. */
#ifndef ENVMOND_MESSAGE_H
#define ENVMOND_MESSAGE_H

#include <stddef.h>

#include "alarm.h"

int msg_reading(char *buf, size_t size, unsigned long long seq, const char *device, const char *sensor,
                const char *kind, long long ts, long n, double min, double avg, double max);

int msg_alarm(char *buf, size_t size, unsigned long long seq, const char *device, const char *sensor,
              const char *kind, const struct alarm_event *ev);

int msg_status(char *buf, size_t size, unsigned long long seq, const char *device, long long ts,
               unsigned long long accepted, unsigned long long rejected, unsigned long long dropped,
               long long spool_bytes);

/* Sequence number at the start of a message line, or 0 if absent. */
unsigned long long msg_seq(const char *line);

#endif
