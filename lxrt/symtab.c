/* Native symbol tables: lxrt's own groups (generated list) + provider DLLs (lxgfx, lxsnd). */
#include "lxrt.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Group names are pasted into identifiers: drop CRT macros that share a group's name. */
#undef setjmp
#undef errno

#define X(group) LX_SYMTAB_DECL(group)
#define LX_SYMTAB_DECL(group) extern const lx_sym lx_syms_##group[]; extern const unsigned lx_nsyms_##group;
#include "symtab_list.h"
#undef LX_SYMTAB_DECL
#undef X

typedef struct { const lx_sym *syms; const unsigned *n; } group;
static const group builtin_groups[] = {
#define X(g) { lx_syms_##g, &lx_nsyms_##g },
#include "symtab_list.h"
#undef X
};

static const lx_sym **table;   /* sorted by name, then versioned before unversioned */
static unsigned ntable;

#define MAX_PROVIDERS 8
static struct { HMODULE h; uintptr_t lo, hi; } providers[MAX_PROVIDERS];
static unsigned nproviders;
static uintptr_t self_lo, self_hi;

static int cmp(const void *a, const void *b)
{
    const lx_sym *x = *(const lx_sym *const *)a, *y = *(const lx_sym *const *)b;
    int c = strcmp(x->name, y->name);
    if (c)
        return c;
    if (!x->version != !y->version)
        return x->version ? -1 : 1;
    return 0;
}

static void image_range(HMODULE h, uintptr_t *lo, uintptr_t *hi)
{
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)((const char *)h + ((const IMAGE_DOS_HEADER *)h)->e_lfanew);
    *lo = (uintptr_t)h;
    *hi = *lo + nt->OptionalHeader.SizeOfImage;
}

void lx_native_init(void)
{
    static const char *const names[] = { "lxgfx.dll", "lxsnd.dll" };
    const lx_sym *ptabs[MAX_PROVIDERS];
    unsigned pcounts[MAX_PROVIDERS];
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)lx_native_init, &self);
    image_range(self, &self_lo, &self_hi);
    wchar_t dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, dir, MAX_PATH);
    while (n && dir[n - 1] != L'\\' && dir[n - 1] != L'/')
        n--;
    unsigned total = 0;
    for (unsigned i = 0; i < LX_COUNT(builtin_groups); i++)
        total += *builtin_groups[i].n;
    for (unsigned i = 0; i < LX_COUNT(names); i++) {
        wchar_t path[MAX_PATH];
        if (n + strlen(names[i]) + 1 >= MAX_PATH)
            continue;
        wmemcpy(path, dir, n);
        MultiByteToWideChar(CP_UTF8, 0, names[i], -1, path + n, MAX_PATH - n);
        HMODULE h = LoadLibraryW(path);
        if (!h) {
            if (lx_trace)
                lx_log("provider %s not loaded (%lu)", names[i], GetLastError());
            continue;
        }
        lx_provider_symbols_fn fn = (lx_provider_symbols_fn)(void *)GetProcAddress(h, "lx_provider_symbols");
        if (!fn) {
            lx_log("provider %s has no lx_provider_symbols", names[i]);
            continue;
        }
        providers[nproviders].h = h;
        image_range(h, &providers[nproviders].lo, &providers[nproviders].hi);
        ptabs[nproviders] = fn(&pcounts[nproviders]);
        total += pcounts[nproviders];
        if (lx_trace)
            lx_log("provider %s: %u symbols", names[i], pcounts[nproviders]);
        nproviders++;
    }
    table = malloc(total * sizeof *table);
    /* lxrt's own definitions are inserted first so they win ties (stable merge below). */
    for (unsigned i = 0; i < LX_COUNT(builtin_groups); i++)
        for (unsigned j = 0; j < *builtin_groups[i].n; j++)
            table[ntable++] = &builtin_groups[i].syms[j];
    for (unsigned p = 0; p < nproviders; p++)
        for (unsigned j = 0; j < pcounts[p]; j++)
            table[ntable++] = &ptabs[p][j];
    /* insertion-stable sort: qsort is not stable, so sort indices by (name, versioned-first, order) */
    unsigned *order = malloc(ntable * sizeof *order);
    for (unsigned i = 0; i < ntable; i++)
        order[i] = i;
    const lx_sym **sorted = malloc(ntable * sizeof *sorted);
    /* simple merge sort on (cmp, original index) */
    for (unsigned width = 1; width < ntable; width *= 2) {
        unsigned *tmp = malloc(ntable * sizeof *tmp);
        for (unsigned lo = 0; lo < ntable; lo += 2 * width) {
            unsigned mid = lo + width < ntable ? lo + width : ntable;
            unsigned hi = lo + 2 * width < ntable ? lo + 2 * width : ntable;
            unsigned a = lo, b = mid, k = lo;
            while (a < mid && b < hi) {
                int c = cmp(&table[order[a]], &table[order[b]]);
                tmp[k++] = (c <= 0) ? order[a++] : order[b++];
            }
            while (a < mid)
                tmp[k++] = order[a++];
            while (b < hi)
                tmp[k++] = order[b++];
        }
        free(order);
        order = tmp;
    }
    for (unsigned i = 0; i < ntable; i++)
        sorted[i] = table[order[i]];
    free(order);
    free(table);
    table = sorted;
}

void *lx_native_lookup(const char *name, const char *version, int *found)
{
    unsigned lo = 0, hi = ntable;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (strcmp(table[mid]->name, name) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    const lx_sym *fallback = NULL;
    for (unsigned i = lo; i < ntable && !strcmp(table[i]->name, name); i++) {
        const lx_sym *s = table[i];
        if (!s->version) {
            if (!fallback)
                fallback = s;
        } else if (version && !strcmp(s->version, version)) {
            *found = 1;
            return s->addr;
        } else if (!version && !fallback) {
            fallback = s; /* unversioned request: first (default) versioned entry */
        }
    }
    *found = fallback != NULL;
    return fallback ? fallback->addr : NULL;
}

int lx_native_owner(uintptr_t addr, char *name, size_t cap, uintptr_t *base)
{
    HMODULE h = NULL;
    if (addr >= self_lo && addr < self_hi) {
        h = (HMODULE)self_lo;
    } else {
        for (unsigned i = 0; i < nproviders; i++)
            if (addr >= providers[i].lo && addr < providers[i].hi)
                h = providers[i].h;
    }
    if (!h)
        return 0;
    wchar_t w[MAX_PATH];
    DWORD n = GetModuleFileNameW(h, w, MAX_PATH);
    w[n < MAX_PATH ? n : MAX_PATH - 1] = 0;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, name, (int)cap, NULL, NULL);
    for (char *p = name; *p; p++)
        if (*p == '\\')
            *p = '/';
    *base = (uintptr_t)h;
    return 1;
}
