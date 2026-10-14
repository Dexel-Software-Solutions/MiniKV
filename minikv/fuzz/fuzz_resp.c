/* libFuzzer harness: feed arbitrary bytes to the RESP parser and check that
 * a successful parse never reports more bytes than given and that every
 * argument points inside the input buffer. */
#include <stddef.h>
#include <stdint.h>
#include "resp.h"

static rcmd cmd;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const char *err = NULL;
    size_t used = 0;
    int r = resp_parse((const char *)data, size, &used, &cmd, &err);
    if (r == RESP_OK) {
        if (used > size) __builtin_trap();
        for (int i = 0; i < cmd.argc; i++) {
            const char *p = cmd.argv[i].p;
            if (p < (const char *)data || p + cmd.argv[i].n > (const char *)data + size)
                __builtin_trap();
        }
    }
    return 0;
}
