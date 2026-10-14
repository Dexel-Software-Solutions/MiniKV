/* Author:  Demiyan Dissanayake <demiyandissanayake@gmail.com>
 * Project: MiniKV - https://github.com/Dexel-Software-Solutions
 * License: MIT (see LICENSE)
 */
/* resp.h - zero-copy RESP2 / inline command parser.
 *
 * The parser never allocates and never modifies the input. Parsed arguments
 * point INTO the caller's buffer, so they are only valid until the buffer is
 * modified. It is restartable: if the buffer holds an incomplete command it
 * returns RESP_INCOMPLETE and the caller simply retries after reading more.
 */
#ifndef RESP_H
#define RESP_H

#include <stddef.h>

#define RESP_MAX_ARGS   1024
#define RESP_MAX_BULK   (64u * 1024 * 1024)
#define RESP_MAX_INLINE (64u * 1024)

typedef struct {
    const char *p;
    size_t n;
} rarg;

typedef struct {
    int argc;
    rarg argv[RESP_MAX_ARGS];
} rcmd;

#define RESP_OK 1
#define RESP_INCOMPLETE 0
#define RESP_ERR (-1)

/* Parse one command from buf[0..len).
 * On RESP_OK: *consumed = bytes used, out filled (argc may be 0 for an empty
 * command). On RESP_ERR: *err points to a static message. */
int resp_parse(const char *buf, size_t len, size_t *consumed, rcmd *out,
               const char **err);

#endif
