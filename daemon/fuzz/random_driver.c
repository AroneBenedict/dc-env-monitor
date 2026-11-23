/* Fallback fuzz driver for toolchains without libFuzzer: random and mutated inputs fed to the
 * same LLVMFuzzerTestOneInput, built with AddressSanitizer and UBSan. Deterministic per seed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static uint64_t state = 88172645463325252ULL;
static uint64_t rnd(void) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; }

static const char *SEEDS[] = {
    "1735689600 t 24.5\n1735689660 h 45\n1735689720 l 1\n",
    "[device]\nid = x\n[sensor a]\nkind = temperature\nwarn_high = 27\ncrit_high = 32\n",
    "1735689600 t 1e308\n1735689600 t -0\n1735689601 t 32.0000001\n",
};
static const char ALPHABET[] = "0123456789 .-+eE\n\t[]=#;abcdhlt_kinds\r\x00\xff";

int main(int argc, char **argv)
{
    long iterations = argc > 1 ? atol(argv[1]) : 1000000;
    static uint8_t buf[4096];
    for (long it = 0; it < iterations; it++) {
        size_t len;
        if (rnd() % 3 == 0) { /* fully random */
            len = rnd() % 512;
            for (size_t i = 0; i < len; i++)
                buf[i] = (uint8_t)(rnd() % 4 ? ALPHABET[rnd() % (sizeof ALPHABET - 1)] : rnd());
        } else { /* mutate a seed */
            const char *seed = SEEDS[rnd() % 3];
            len = strlen(seed);
            memcpy(buf, seed, len);
            for (int m = (int)(rnd() % 8); m >= 0; m--) {
                size_t pos = len ? rnd() % len : 0;
                switch (rnd() % 3) {
                case 0: buf[pos] = (uint8_t)rnd(); break;
                case 1: if (len < sizeof buf - 64) { memmove(buf + pos + 1, buf + pos, len - pos); buf[pos] = (uint8_t)ALPHABET[rnd() % (sizeof ALPHABET - 1)]; len++; } break;
                default: if (len) { memmove(buf + pos, buf + pos + 1, len - pos - 1); len--; } break;
                }
            }
        }
        LLVMFuzzerTestOneInput(buf, len);
    }
    printf("%ld inputs, no crashes\n", iterations);
    return 0;
}
