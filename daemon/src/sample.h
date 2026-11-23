/* Sensor input lines: "<unix_time> <sensor_id> <value>", one sample per line. */
#ifndef ENVMOND_SAMPLE_H
#define ENVMOND_SAMPLE_H

#include "common.h"

struct sample {
    long long ts;
    char sensor[MAX_ID];
    double value;
};

enum parse_result { PARSE_OK = 0, PARSE_EMPTY, PARSE_BAD_FORMAT, PARSE_BAD_ID, PARSE_BAD_NUMBER, PARSE_TOO_LONG };

/* Parse one line (without or with trailing newline). Never reads past len bytes. */
enum parse_result sample_parse(const char *line, size_t len, struct sample *out);

const char *parse_result_name(enum parse_result r);

#endif
