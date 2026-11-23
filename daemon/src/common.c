#include "common.h"

#include <string.h>

int copy_str(char *dst, size_t dst_size, const char *src)
{
    size_t n = strlen(src);
    if (dst_size == 0)
        return -1;
    if (n >= dst_size) {
        memcpy(dst, src, dst_size - 1);
        dst[dst_size - 1] = '\0';
        return -1;
    }
    memcpy(dst, src, n + 1);
    return 0;
}

int valid_id(const char *s)
{
    size_t n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                 c == '-';
        if (!ok || n >= MAX_ID - 1)
            return 0;
    }
    return n > 0;
}
