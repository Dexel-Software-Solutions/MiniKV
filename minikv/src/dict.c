/* Author:  Demiyan Dissanayake <demiyandissanayake@gmail.com>
 * Project: MiniKV - https://github.com/Dexel-Software-Solutions
 * License: MIT (see LICENSE)
 */
#define _GNU_SOURCE
#include "dict.h"

#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#define DICT_INIT_SIZE 4

/* FNV-1a with a per-dict random seed and a murmur3 finalizer so the low bits
 * (used for bucket selection) are well mixed.
 * NOTE: for hostile networks use SipHash to be fully HashDoS-proof; the random
 * seed already makes bucket collisions unpredictable to a remote attacker. */
static uint64_t hash_bytes(uint64_t seed, const char *k, size_t n) {
    uint64_t h = 1469598103934665603ULL ^ seed;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)k[i];
        h *= 1099511628211ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

static void entry_free(entry *e) {
    free(e->key);
    free(e->val);
    free(e);
}

static char *dup_n(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static void table_free(dtable *t) {
    for (size_t i = 0; i < t->size; i++) {
        entry *e = t->t[i];
        while (e) {
            entry *next = e->next;
            entry_free(e);
            e = next;
        }
    }
    free(t->t);
    t->t = NULL;
    t->size = t->used = 0;
}

dict *dict_new(void) {
    dict *d = calloc(1, sizeof *d);
    uint64_t s[2] = {0, 0};
    if (!d) return NULL;
    d->rehashidx = -1;
    if (getrandom(s, sizeof s, 0) != (ssize_t)sizeof s) {
        s[0] = (uint64_t)time(NULL) * 0x9e3779b97f4a7c15ULL;
        s[1] = (uint64_t)(uintptr_t)d;
    }
    d->seed = s[0];
    d->rng = s[1] | 1;
    return d;
}

void dict_clear(dict *d) {
    table_free(&d->ht[0]);
    table_free(&d->ht[1]);
    d->rehashidx = -1;
}

void dict_free(dict *d) {
    if (!d) return;
    dict_clear(d);
    free(d);
}

size_t dict_size(const dict *d) { return d->ht[0].used + d->ht[1].used; }

/* Move up to n non-empty buckets from ht[0] to ht[1]. */
static void rehash_step(dict *d, int n) {
    long empty_visits = (long)n * 10;
    if (d->rehashidx < 0) return;

    while (n-- > 0 && d->ht[0].used != 0) {
        entry *e;
        while (d->ht[0].t[d->rehashidx] == NULL) {
            d->rehashidx++;
            if (--empty_visits == 0) return;
        }
        e = d->ht[0].t[d->rehashidx];
        while (e) {
            entry *next = e->next;
            size_t idx = e->hash & (d->ht[1].size - 1);
            e->next = d->ht[1].t[idx];
            d->ht[1].t[idx] = e;
            d->ht[0].used--;
            d->ht[1].used++;
            e = next;
        }
        d->ht[0].t[d->rehashidx] = NULL;
        d->rehashidx++;
    }

    if (d->ht[0].used == 0) { /* finished: ht[1] becomes the main table */
        free(d->ht[0].t);
        d->ht[0] = d->ht[1];
        d->ht[1].t = NULL;
        d->ht[1].size = d->ht[1].used = 0;
        d->rehashidx = -1;
    }
}

static int expand_if_needed(dict *d) {
    size_t sz;
    entry **nt;

    if (d->rehashidx >= 0) return 0;
    if (d->ht[0].size == 0) {
        d->ht[0].t = calloc(DICT_INIT_SIZE, sizeof(entry *));
        if (!d->ht[0].t) return -1;
        d->ht[0].size = DICT_INIT_SIZE;
        return 0;
    }
    if (d->ht[0].used < d->ht[0].size) return 0; /* load factor < 1 */

    sz = d->ht[0].size * 2;
    nt = calloc(sz, sizeof(entry *));
    if (!nt) return -1;
    d->ht[1].t = nt;
    d->ht[1].size = sz;
    d->ht[1].used = 0;
    d->rehashidx = 0;
    return 0;
}

entry *dict_find(dict *d, const char *k, size_t n) {
    uint64_t h;
    if (d->ht[0].size == 0) return NULL;
    rehash_step(d, 1);
    h = hash_bytes(d->seed, k, n);
    for (int i = 0; i < 2; i++) {
        dtable *t = &d->ht[i];
        if (t->size == 0) continue; /* ht[1] is unallocated unless rehashing */
        for (entry *e = t->t[h & (t->size - 1)]; e; e = e->next)
            if (e->hash == h && e->klen == n && memcmp(e->key, k, n) == 0)
                return e;
    }
    return NULL;
}

entry *dict_set(dict *d, const char *k, size_t n, const char *v, size_t vn) {
    entry *e = dict_find(d, k, n);
    dtable *t;
    char *nv;

    if (e) {
        nv = dup_n(v, vn);
        if (!nv) return NULL;
        free(e->val);
        e->val = nv;
        e->vlen = vn;
        return e;
    }

    if (expand_if_needed(d) < 0) return NULL;
    e = malloc(sizeof *e);
    if (!e) return NULL;
    e->key = dup_n(k, n);
    e->val = dup_n(v, vn);
    if (!e->key || !e->val) {
        entry_free(e);
        return NULL;
    }
    e->klen = n;
    e->vlen = vn;
    e->hash = hash_bytes(d->seed, k, n);
    e->expire_at = -1;

    /* While rehashing, all new keys go to the new table. */
    t = (d->rehashidx >= 0) ? &d->ht[1] : &d->ht[0];
    e->next = t->t[e->hash & (t->size - 1)];
    t->t[e->hash & (t->size - 1)] = e;
    t->used++;
    return e;
}

int dict_delete(dict *d, const char *k, size_t n) {
    entry *e = dict_find(d, k, n); /* also performs a rehash step */
    if (!e) return 0;
    dict_delete_entry(d, e);
    return 1;
}

void dict_delete_entry(dict *d, entry *e) {
    for (int i = 0; i < 2; i++) {
        dtable *t = &d->ht[i];
        entry **pp;
        if (t->size == 0) continue;
        pp = &t->t[e->hash & (t->size - 1)];
        while (*pp) {
            if (*pp == e) {
                *pp = e->next;
                t->used--;
                entry_free(e);
                return;
            }
            pp = &(*pp)->next;
        }
    }
}

static uint64_t next_rand(dict *d) { /* xorshift64 */
    d->rng ^= d->rng << 13;
    d->rng ^= d->rng >> 7;
    d->rng ^= d->rng << 17;
    return d->rng;
}

entry *dict_random_bucket(dict *d) {
    uint64_t r;
    dtable *t = &d->ht[0];
    if (t->size == 0) return NULL;
    r = next_rand(d);
    if (d->rehashidx >= 0 && (r & 1)) t = &d->ht[1];
    return t->t[(r >> 1) & (t->size - 1)];
}

void dict_foreach(dict *d, void (*fn)(entry *, void *), void *ctx) {
    for (int i = 0; i < 2; i++) {
        dtable *t = &d->ht[i];
        for (size_t b = 0; b < t->size; b++)
            for (entry *e = t->t[b]; e; e = e->next) fn(e, ctx);
    }
}
