/* Author:  Demiyan Dissanayake <demiyandissanayake@gmail.com>
 * Project: MiniKV - https://github.com/Dexel-Software-Solutions
 * License: MIT (see LICENSE)
 */
/* dict.h - chained hash table with INCREMENTAL rehashing.
 *
 * When the table needs to grow, a second table is allocated and buckets are
 * migrated a few at a time on every operation, so no single request ever pays
 * an O(N) resize pause (a classic latency spike in naive implementations).
 */
#ifndef DICT_H
#define DICT_H

#include <stddef.h>
#include <stdint.h>

typedef struct entry {
    struct entry *next;
    uint64_t hash;
    char *key;
    size_t klen;
    char *val;
    size_t vlen;
    long long expire_at; /* absolute unix time in ms, -1 = no expiry */
} entry;

typedef struct {
    entry **t;
    size_t size; /* power of two, 0 = unallocated */
    size_t used;
} dtable;

typedef struct dict {
    dtable ht[2];
    long rehashidx; /* -1 = not rehashing */
    uint64_t seed;
    uint64_t rng;
} dict;

dict *dict_new(void);
void dict_free(dict *d);
void dict_clear(dict *d);

entry *dict_find(dict *d, const char *k, size_t n);
/* Insert or overwrite the value (expire_at kept on overwrite, -1 on insert).
 * Returns NULL on out-of-memory (dict unchanged). */
entry *dict_set(dict *d, const char *k, size_t n, const char *v, size_t vn);
int dict_delete(dict *d, const char *k, size_t n);
void dict_delete_entry(dict *d, entry *e);
size_t dict_size(const dict *d);

/* Head of a random bucket chain (may be NULL). Used for active expiry. */
entry *dict_random_bucket(dict *d);
/* Visit every entry. The callback must not modify the dict. */
void dict_foreach(dict *d, void (*fn)(entry *, void *), void *ctx);

#endif
