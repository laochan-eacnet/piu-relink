/* Guest heap: 16-byte aligned (glibc i386 MALLOC_ALIGNMENT) blocks from a private Win32 heap, so
 * guest heap corruption cannot take lxrt's CRT heap down with it.
 *
 * Usable size mirrors glibc i386 chunk rounding: a request of n bytes gets
 * max(16, (n + 4 + 15) & ~15) - 4 usable bytes. Guests that overrun a block by a few bytes
 * (harmless under glibc) therefore stay harmless here.
 *
 * [debug] heapcheck: exact-size blocks followed by a 16-byte guard; lx_free/lx_realloc verify the
 * guard and log the block, its allocation caller and the freeing caller on overflow. */
#include "lxrt.h"

#include <string.h>

/* Header immediately before the user pointer. */
typedef struct { void *block; size_t size; size_t requested; uintptr_t caller; } hdr;
static_assert(sizeof(hdr) == 16, "header keeps 16-byte alignment");

#define GUARD 16
#define GUARD_BYTE 0xFD
static HANDLE heap;
static int heapcheck = -1;

static HANDLE guest_heap(void)
{
    if (!heap) {
        HANDLE h = HeapCreate(0, 0, 0);
        if (InterlockedCompareExchangePointer(&heap, h, NULL))
            HeapDestroy(h);
        heapcheck = lx_config_bool("debug", "heapcheck");
    }
    return heap;
}

static size_t usable_for(size_t n)
{
    if (heapcheck)
        return n;
    size_t chunk = (n + 4 + 15) & ~(size_t)15;
    return (chunk < 16 ? 16 : chunk) - 4;
}

static void check_guard(const void *p, uintptr_t freer)
{
    const hdr *h = (const hdr *)p - 1;
    const unsigned char *g = (const unsigned char *)p + h->size;
    for (int i = 0; i < GUARD; i++)
        if (g[i] != GUARD_BYTE) {
            lx_log("HEAPCHECK: overflow past %p (size %u, guard byte %d = %#x), allocated from %p, freed from %p",
                   p, (unsigned)h->size, i, g[i], (void *)h->caller, (void *)freer);
            return;
        }
}

static void *alloc_aligned(size_t n, size_t align, uintptr_t caller)
{
    HANDLE hp = guest_heap();
    if (align < 16)
        align = 16;
    if (n > (size_t)-1 / 2) {
        lx_set_errno(LX_ENOMEM);
        return NULL;
    }
    size_t usable = usable_for(n);
    char *block = HeapAlloc(hp, 0, usable + align + sizeof(hdr) + (heapcheck ? GUARD : 0));
    if (!block) {
        lx_set_errno(LX_ENOMEM);
        return NULL;
    }
    uintptr_t user = ((uintptr_t)block + sizeof(hdr) + align - 1) & ~(uintptr_t)(align - 1);
    hdr *h = (hdr *)user - 1;
    h->block = block;
    h->size = usable;
    h->requested = n;
    h->caller = caller;
    if (heapcheck)
        memset((char *)user + usable, GUARD_BYTE, GUARD);
    return (void *)user;
}

void *lx_malloc(size_t n)
{
    return alloc_aligned(n, 16, (uintptr_t)__builtin_return_address(0));
}

void lx_free(void *p)
{
    if (!p)
        return;
    if (heapcheck)
        check_guard(p, (uintptr_t)__builtin_return_address(0));
    HeapFree(heap, 0, ((hdr *)p - 1)->block);
}

void *lx_calloc(size_t n, size_t m)
{
    if (m && n > (size_t)-1 / m) {
        lx_set_errno(LX_ENOMEM);
        return NULL;
    }
    void *p = alloc_aligned(n * m, 16, (uintptr_t)__builtin_return_address(0));
    if (p)
        memset(p, 0, n * m);
    return p;
}

void *lx_realloc(void *p, size_t n)
{
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    if (!p)
        return alloc_aligned(n, 16, caller);
    if (!n) {
        lx_free(p);
        return NULL;
    }
    hdr *h = (hdr *)p - 1;
    if (heapcheck)
        check_guard(p, caller);
    if (!heapcheck && n <= h->size && (uintptr_t)p - (uintptr_t)h->block == 16) {
        h->requested = n;
        return p;
    }
    void *q = alloc_aligned(n, 16, caller);
    if (!q)
        return NULL;
    memcpy(q, p, h->requested < n ? h->requested : n);
    HeapFree(heap, 0, h->block);
    return q;
}

char *lx_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = lx_malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static char *strndup_(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *d = lx_malloc(len + 1);
    if (d) {
        memcpy(d, s, len);
        d[len] = 0;
    }
    return d;
}

static int posix_memalign_(void **out, size_t align, size_t n)
{
    if (!align || (align & (align - 1)) || align % sizeof(void *))
        return LX_EINVAL;
    void *p = alloc_aligned(n, align, (uintptr_t)__builtin_return_address(0));
    if (!p)
        return LX_ENOMEM;
    *out = p;
    return 0;
}

static void *memalign_(size_t align, size_t n)
{
    if (!align || (align & (align - 1))) {
        lx_set_errno(LX_EINVAL);
        return NULL;
    }
    return alloc_aligned(n, align, (uintptr_t)__builtin_return_address(0));
}

static void *valloc_(size_t n)
{
    return alloc_aligned(n, 4096, (uintptr_t)__builtin_return_address(0));
}

static size_t malloc_usable_size_(void *p)
{
    return p ? ((hdr *)p - 1)->size : 0;
}

static void cfree_(void *p)
{
    lx_free(p);
}

LX_SYMTAB(mem,
    LX_SYM("malloc", lx_malloc),
    LX_SYM("free", lx_free),
    LX_SYM("cfree", cfree_),
    LX_SYM("calloc", lx_calloc),
    LX_SYM("realloc", lx_realloc),
    LX_SYM("strdup", lx_strdup),
    LX_SYM("__strdup", lx_strdup),
    LX_SYM("strndup", strndup_),
    LX_SYM("__strndup", strndup_),
    LX_SYM("posix_memalign", posix_memalign_),
    LX_SYM("memalign", memalign_),
    LX_SYM("aligned_alloc", memalign_),
    LX_SYM("valloc", valloc_),
    LX_SYM("malloc_usable_size", malloc_usable_size_),
);
