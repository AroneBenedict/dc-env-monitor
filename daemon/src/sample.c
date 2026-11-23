#include "sample.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

const char *parse_result_name(enum parse_result r)
{
    static const char *names[] = {"ok", "empty", "bad_format", "bad_id", "bad_number", "too_long"};
    return (unsigned)r < sizeof names / sizeof names[0] ? names[r] : "unknown";
}

enum parse_result sample_parse(const char *line, size_t len, struct sample *out)
{
    char buf[MAX_LINE];
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        len--;
    if (len == 0)
        return PARSE_EMPTY;
    if (len >= sizeof buf)
        return PARSE_TOO_LONG;
    memcpy(buf, line, len);
    buf[len] = '\0';
    if (memchr(buf, '\0', len))
        return PARSE_BAD_FORMAT; /* embedded NUL */

    char *save = NULL;
    char *ts_s = strtok_r(buf, " \t", &save);
    char *id = strtok_r(NULL, " \t", &save);
    char *val_s = strtok_r(NULL, " \t", &save);
    char *extra = strtok_r(NULL, " \t", &save);
    if (!ts_s || !id || !val_s || extra)
        return PARSE_BAD_FORMAT;
    if (!valid_id(id))
        return PARSE_BAD_ID;

    char *end;
    errno = 0;
    long long ts = strtoll(ts_s, &end, 10);
    if (errno || *end || ts < 946684800LL || ts > 4102444800LL) /* 2000-01-01 .. 2100-01-01 */
        return PARSE_BAD_NUMBER;
    errno = 0;
    double v = strtod(val_s, &end);
    if (errno || *end || !isfinite(v))
        return PARSE_BAD_NUMBER;

    out->ts = ts;
    copy_str(out->sensor, sizeof out->sensor, id);
    out->value = v;
    return PARSE_OK;
}
