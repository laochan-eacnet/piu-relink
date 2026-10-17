/* Process-lifetime metadata cache for [immutable] mounts (phoenixtools.ini).
 *
 * phoenixtools resolves game resources with stat/lstat + opendir/readdir over the 35 GB read-only
 * data tree (component-wise case folding, plus one indexing walk per root for bare basenames).
 * On Linux the dentry cache makes that cheap; on Windows each call is a file-system
 * round trip. For mounts declared immutable we cache stat results (including ENOENT) and
 * directory listings by case-folded host path. Entries are never invalidated: the mount is
 * read-only by contract (phoenixtools opens plain_root read-only).
 *
 * Values are opaque byte blobs owned by the cache (struct stat64 images, dirent arrays). */
#include "lxrt.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct entry {
    struct entry *next;
    uint64_t hash;
    int kind;            /* LX_META_STAT / LX_META_DIR */
    int err;             /* 0 or Linux errno (negative stat results) */
    void *value;
    size_t size;
    wchar_t key[];
} entry;

#define NBUCKETS 16384
static entry *buckets[NBUCKETS];
static SRWLOCK lock = SRWLOCK_INIT;

/* NTFS-style case folding for the cache key: ASCII inline, CRT towlower only beyond ASCII. */
static wchar_t fold(wchar_t c)
{
    if (c == L'/')
        return L'\\';
    if (c < 0x80)
        return c >= L'A' && c <= L'Z' ? (wchar_t)(c + 32) : c;
    return (wchar_t)towlower(c);
}

static uint64_t fold_hash(const wchar_t *w)
{
    uint64_t h = 1469598103934665603ull;
    for (; *w; w++)
        h = (h ^ fold(*w)) * 1099511628211ull;
    return h;
}

static int fold_eq(const wchar_t *a, const wchar_t *b)
{
    for (; *a && *b; a++, b++)
        if (fold(*a) != fold(*b))
            return 0;
    return *a == *b;
}

int lx_meta_get(int kind, const wchar_t *host, void **value, size_t *size, int *err)
{
    uint64_t h = fold_hash(host);
    int found = 0;
    AcquireSRWLockShared(&lock);
    for (entry *e = buckets[h % NBUCKETS]; e; e = e->next)
        if (e->hash == h && e->kind == kind && fold_eq(e->key, host)) {
            *err = e->err;
            if (value) {
                *value = e->size ? malloc(e->size) : NULL;
                if (e->size)
                    memcpy(*value, e->value, e->size);
            }
            if (size)
                *size = e->size;
            found = 1;
            break;
        }
    ReleaseSRWLockShared(&lock);
    return found;
}

/* Cached value without copying; entries are never freed, so the pointer stays valid. */
const void *lx_meta_peek(int kind, const wchar_t *host, size_t *size, int *err)
{
    uint64_t h = fold_hash(host);
    const void *v = NULL;
    int found = 0;
    AcquireSRWLockShared(&lock);
    for (entry *e = buckets[h % NBUCKETS]; e; e = e->next)
        if (e->hash == h && e->kind == kind && fold_eq(e->key, host)) {
            v = e->value;
            *size = e->size;
            *err = e->err;
            found = 1;
            break;
        }
    ReleaseSRWLockShared(&lock);
    return found ? (v ? v : (const void *)host) : NULL;
}

void lx_meta_put(int kind, const wchar_t *host, const void *value, size_t size, int err)
{
    size_t klen = wcslen(host) + 1;
    entry *n = malloc(sizeof *n + klen * sizeof(wchar_t));
    if (!n)
        return;
    n->hash = fold_hash(host);
    n->kind = kind;
    n->err = err;
    n->size = size;
    n->value = size ? malloc(size) : NULL;
    if (size)
        memcpy(n->value, value, size);
    memcpy(n->key, host, klen * sizeof(wchar_t));
    AcquireSRWLockExclusive(&lock);
    for (entry *e = buckets[n->hash % NBUCKETS]; e; e = e->next)
        if (e->hash == n->hash && e->kind == kind && fold_eq(e->key, host)) {
            ReleaseSRWLockExclusive(&lock); /* another thread raced us; keep the first */
            free(n->value);
            free(n);
            return;
        }
    n->next = buckets[n->hash % NBUCKETS];
    buckets[n->hash % NBUCKETS] = n;
    ReleaseSRWLockExclusive(&lock);
}
