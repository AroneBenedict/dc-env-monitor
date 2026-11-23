/* Shared limits and small helpers for envmond. */
#ifndef ENVMOND_COMMON_H
#define ENVMOND_COMMON_H

#include <stddef.h>

#define MAX_SENSORS 64
#define MAX_ID 48
#define MAX_LINE 256
#define MAX_MSG 512
#define MAX_PATH 256

/* Copy at most dst_size-1 bytes and always NUL-terminate. Returns 0 if src fitted, -1 if truncated. */
int copy_str(char *dst, size_t dst_size, const char *src);

/* A sensor or device id: 1..MAX_ID-1 characters from [A-Za-z0-9._-]. */
int valid_id(const char *s);

#endif
