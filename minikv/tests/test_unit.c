#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "dict.h"
#include "resp.h"

static rcmd cmd;

static void test_resp(void) {
    const char *full = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nhello\r\n";
    size_t len = strlen(full), used;
    const char *err;

    assert(resp_parse(full, len, &used, &cmd, &err) == RESP_OK);
    assert(used == len && cmd.argc == 3);
    assert(cmd.argv[2].n == 5 && !memcmp(cmd.argv[2].p, "hello", 5));

    /* restartable: every strict prefix is INCOMPLETE */
    for (size_t i = 0; i < len; i++)
        assert(resp_parse(full, i, &used, &cmd, &err) == RESP_INCOMPLETE);

    /* pipelining: only the first command is consumed */
    {
        const char *two = "*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n";
        assert(resp_parse(two, strlen(two), &used, &cmd, &err) == RESP_OK);
        assert(used == 14);
    }
    /* binary-safe bulk containing CRLF inside the payload */
    {
        const char *b = "*2\r\n$3\r\nGET\r\n$4\r\na\r\nb\r\n";
        assert(resp_parse(b, strlen(b), &used, &cmd, &err) == RESP_OK);
        assert(cmd.argv[1].n == 4);
    }
    /* inline */
    {
        const char *in = "SET  a   b\r\n";
        assert(resp_parse(in, strlen(in), &used, &cmd, &err) == RESP_OK);
        assert(cmd.argc == 3 && cmd.argv[2].p[0] == 'b');
    }
    /* malformed input must be rejected, not crash */
    {
        const char *bad[] = {"*x\r\n", "*1\r\n$-5\r\n", "*1\r\n:5\r\n",
                             "*1\r\n$3\r\nabcXY", "*99999\r\n",
                             "*1\r\n$99999999999\r\n"};
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
            assert(resp_parse(bad[i], strlen(bad[i]), &used, &cmd, &err) == RESP_ERR);
    }
    puts("resp: ok");
}

static void test_dict(void) {
    dict *d = dict_new();
    char k[32], v[32];
    const int N = 200000;
    int saw_rehash = 0;

    assert(d);
    for (int i = 0; i < N; i++) {
        int kl = snprintf(k, sizeof k, "key:%d", i);
        int vl = snprintf(v, sizeof v, "val:%d", i);
        assert(dict_set(d, k, (size_t)kl, v, (size_t)vl));
        if (d->rehashidx >= 0) saw_rehash = 1;
        if (i % 997 == 0) { /* lookups stay correct in the middle of a rehash */
            int j = i / 2;
            kl = snprintf(k, sizeof k, "key:%d", j);
            entry *e = dict_find(d, k, (size_t)kl);
            assert(e);
            vl = snprintf(v, sizeof v, "val:%d", j);
            assert(e->vlen == (size_t)vl && !memcmp(e->val, v, e->vlen));
        }
    }
    assert(saw_rehash && dict_size(d) == (size_t)N);

    for (int i = 1; i < N; i += 2) { /* delete odd keys */
        int kl = snprintf(k, sizeof k, "key:%d", i);
        assert(dict_delete(d, k, (size_t)kl) == 1);
        assert(dict_delete(d, k, (size_t)kl) == 0);
    }
    assert(dict_size(d) == (size_t)N / 2);
    for (int i = 0; i < N; i++) {
        int kl = snprintf(k, sizeof k, "key:%d", i);
        assert((dict_find(d, k, (size_t)kl) != NULL) == (i % 2 == 0));
    }
    /* overwrite keeps a single entry; empty key / empty value are legal */
    assert(dict_set(d, "", 0, "", 0));
    assert(dict_set(d, "", 0, "x", 1));
    assert(dict_find(d, "", 0)->vlen == 1);

    dict_clear(d);
    assert(dict_size(d) == 0);
    assert(dict_set(d, "a", 1, "b", 1));
    dict_free(d);
    puts("dict: ok");
}

int main(void) {
    test_resp();
    test_dict();
    return 0;
}
