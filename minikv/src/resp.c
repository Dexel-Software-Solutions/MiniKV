/* Author:  Demiyan Dissanayake <demiyandissanayake@gmail.com>
 * Project: MiniKV - https://github.com/Dexel-Software-Solutions
 * License: MIT (see LICENSE)
 */
#include "resp.h"

#include <string.h>

/* Index of the '\r' of the first "\r\n" at or after `from`, or -1. */
static long find_crlf(const char *b, size_t from, size_t len) {
    while (from < len) {
        const char *p = memchr(b + from, '\r', len - from);
        if (!p) return -1;
        size_t i = (size_t)(p - b);
        if (i + 1 >= len) return -1; /* need one more byte to see '\n' */
        if (b[i + 1] == '\n') return (long)i;
        from = i + 1;
    }
    return -1;
}

/* Strict decimal parse of a short header integer. */
static int parse_ll(const char *s, size_t n, long long *out) {
    size_t i = 0;
    int neg = 0;
    long long v = 0;
    if (n == 0 || n > 18) return -1;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
        if (n == 1) return -1;
    }
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    *out = neg ? -v : v;
    return 0;
}

#define HDR_LIMIT 32 /* a length header longer than this without CRLF is garbage */

static int parse_multibulk(const char *buf, size_t len, size_t *consumed,
                           rcmd *out, const char **err) {
    long i = find_crlf(buf, 0, len);
    long long n;
    size_t pos;

    if (i < 0) {
        if (len > HDR_LIMIT) {
            *err = "Protocol error: invalid multibulk length";
            return RESP_ERR;
        }
        return RESP_INCOMPLETE;
    }
    if (parse_ll(buf + 1, (size_t)i - 1, &n) < 0 || n > RESP_MAX_ARGS) {
        *err = "Protocol error: invalid multibulk length";
        return RESP_ERR;
    }
    pos = (size_t)i + 2;
    if (n <= 0) { /* "*0" / "*-1": empty command, just skip it */
        *consumed = pos;
        return RESP_OK;
    }

    for (long long k = 0; k < n; k++) {
        long long bl;
        long j;
        size_t start;

        if (pos >= len) return RESP_INCOMPLETE;
        if (buf[pos] != '$') {
            *err = "Protocol error: expected '$'";
            return RESP_ERR;
        }
        j = find_crlf(buf, pos, len);
        if (j < 0) {
            if (len - pos > HDR_LIMIT) {
                *err = "Protocol error: invalid bulk length";
                return RESP_ERR;
            }
            return RESP_INCOMPLETE;
        }
        if (parse_ll(buf + pos + 1, (size_t)j - pos - 1, &bl) < 0 || bl < 0 ||
            bl > (long long)RESP_MAX_BULK) {
            *err = "Protocol error: invalid bulk length";
            return RESP_ERR;
        }
        start = (size_t)j + 2;
        if (len - start < (size_t)bl + 2) return RESP_INCOMPLETE;
        if (buf[start + (size_t)bl] != '\r' || buf[start + (size_t)bl + 1] != '\n') {
            *err = "Protocol error: bulk not terminated by CRLF";
            return RESP_ERR;
        }
        out->argv[k].p = buf + start;
        out->argv[k].n = (size_t)bl;
        pos = start + (size_t)bl + 2;
    }
    out->argc = (int)n;
    *consumed = pos;
    return RESP_OK;
}

/* Inline commands ("SET a b\r\n") so telnet / nc work. No quoting support. */
static int parse_inline(const char *buf, size_t len, size_t *consumed,
                        rcmd *out, const char **err) {
    const char *nl = memchr(buf, '\n', len);
    size_t end, i = 0;

    if (!nl) {
        if (len > RESP_MAX_INLINE) {
            *err = "Protocol error: too big inline request";
            return RESP_ERR;
        }
        return RESP_INCOMPLETE;
    }
    end = (size_t)(nl - buf);
    *consumed = end + 1;
    if (end > 0 && buf[end - 1] == '\r') end--;

    while (i < end) {
        size_t s;
        while (i < end && (buf[i] == ' ' || buf[i] == '\t')) i++;
        if (i >= end) break;
        s = i;
        while (i < end && buf[i] != ' ' && buf[i] != '\t') i++;
        if (out->argc >= RESP_MAX_ARGS) {
            *err = "Protocol error: too many arguments";
            return RESP_ERR;
        }
        out->argv[out->argc].p = buf + s;
        out->argv[out->argc].n = i - s;
        out->argc++;
    }
    return RESP_OK;
}

int resp_parse(const char *buf, size_t len, size_t *consumed, rcmd *out,
               const char **err) {
    out->argc = 0;
    if (len == 0) return RESP_INCOMPLETE;
    if (buf[0] == '*') return parse_multibulk(buf, len, consumed, out, err);
    return parse_inline(buf, len, consumed, out, err);
}
