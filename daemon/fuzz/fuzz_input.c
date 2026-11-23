/* libFuzzer harness: arbitrary bytes into the config parser and the sample/engine path.
 * Any crash, out-of-bounds access or undefined behaviour is reported by the sanitizers. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "engine.h"

static int sink(void *ctx, const char *msg, size_t len) { (void)ctx; (void)msg; (void)len; return 0; }
static unsigned long long seq(void *ctx) { (void)ctx; static unsigned long long s; return ++s; }

static const char *CFG =
    "[device]\nid = d\nreport_interval = 60\nstale_after = 60\n"
    "[sensor t]\nkind = temperature\nwarn_high = 27\ncrit_high = 32\n"
    "[sensor h]\nkind = humidity\nwarn_low = 20\n[sensor l]\nkind = leak\n";

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static char text[8192];
    if (size >= sizeof text)
        size = sizeof text - 1;
    memcpy(text, data, size);
    text[size] = '\0';

    static struct config cfg;
    char err[128];
    config_parse(text, &cfg, err, sizeof err); /* must never crash, whatever it returns */

    static struct config base;
    static int ready;
    if (!ready) {
        config_parse(CFG, &base, err, sizeof err);
        ready = 1;
    }
    static struct engine e;
    engine_init(&e, &base, sink, seq, NULL);
    size_t start = 0;
    for (size_t i = 0; i <= size; i++) {
        if (i == size || data[i] == '\n') {
            engine_feed(&e, (const char *)data + start, i - start);
            start = i + 1;
        }
    }
    engine_flush(&e);
    return 0;
}
