#ifndef ENVMOND_LOG_H
#define ENVMOND_LOG_H

enum log_level { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR };

extern enum log_level log_threshold;

/* One line to stderr (journald picks it up under systemd). */
void log_msg(enum log_level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
