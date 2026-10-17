/* ELF binder: module registry, global scope, symbol lookup, relocation, init/fini, TLS,
 * entry. Modules are PE images produced by relink.py; their DllMain stubs call lx_register()
 * and the EXE entry stub calls lx_start(). */
#include "lxrt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

lx_module lx_modules[LX_MAX_MODULES];
unsigned lx_nmodules;
lx_module *lx_scope[LX_MAX_MODULES];
unsigned lx_nscope;
static lx_module *init_order[LX_MAX_MODULES];
static unsigned ninit;
static unsigned next_tls_modid = 1;

enum {
    PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7,
    DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_INIT = 12,
    DT_FINI = 13, DT_SONAME = 14, DT_REL = 17, DT_RELSZ = 18, DT_JMPREL = 23, DT_INIT_ARRAY = 25,
    DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28,
    DT_GNU_HASH = 0x6ffffef5, DT_VERSYM = 0x6ffffff0, DT_VERDEF = 0x6ffffffc, DT_VERDEFNUM = 0x6ffffffd,
    DT_VERNEED = 0x6ffffffe, DT_VERNEEDNUM = 0x6fffffff,
    STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2, STB_GNU_UNIQUE = 10,
    STT_TLS = 6, STT_GNU_IFUNC = 10, SHN_UNDEF = 0,
    R_386_NONE = 0, R_386_32 = 1, R_386_PC32 = 2, R_386_COPY = 5, R_386_GLOB_DAT = 6,
    R_386_JMP_SLOT = 7, R_386_RELATIVE = 8, R_386_TLS_TPOFF = 14, R_386_TLS_DTPMOD32 = 35,
    R_386_TLS_DTPOFF32 = 36, R_386_TLS_TPOFF32 = 37, R_386_IRELATIVE = 42,
};

#define ELF32_ST_BIND(i) ((i) >> 4)
#define ELF32_ST_TYPE(i) ((i) & 0xf)

/* ---- registration ---------------------------------------------------------------------- */

static void module_path(lx_module *m)
{
    wchar_t w[MAX_PATH];
    DWORD n = GetModuleFileNameW(m->hmod, w, MAX_PATH);
    w[n < MAX_PATH ? n : MAX_PATH - 1] = 0;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, m->path, sizeof m->path, NULL, NULL);
    for (char *p = m->path; *p; p++)
        if (*p == '\\')
            *p = '/';
}

static const char *vername_def(lx_module *m, const Elf32_Verdef *vd)
{
    const Elf32_Verdaux *aux = (const Elf32_Verdaux *)((const char *)vd + vd->vd_aux);
    return m->strtab + aux->vda_name;
}

static void parse_dynamic(lx_module *m)
{
    uintptr_t b = m->bias;
    uintptr_t verdef = 0, verneed = 0;
    unsigned verdefnum = 0, verneednum = 0;
    unsigned needed_off[LX_MAX_NEEDED];
    for (const Elf32_Dyn *d = m->dyn; d->d_tag; d++) {
        uint32_t v = d->d_val;
        switch (d->d_tag) {
        case DT_NEEDED:
            if (m->nneeded >= LX_MAX_NEEDED)
                lx_fatal("%s: too many DT_NEEDED", m->name);
            needed_off[m->nneeded++] = v;
            break;
        case DT_HASH: m->hash = (const uint32_t *)(b + v); break;
        case DT_GNU_HASH: m->gnu_hash = (const uint32_t *)(b + v); break;
        case DT_STRTAB: m->strtab = (const char *)(b + v); break;
        case DT_SYMTAB: m->symtab = (const Elf32_Sym *)(b + v); break;
        case DT_REL: m->rel = (const Elf32_Rel *)(b + v); break;
        case DT_RELSZ: m->relsz = v; break;
        case DT_JMPREL: m->jmprel = (const Elf32_Rel *)(b + v); break;
        case DT_PLTRELSZ: m->pltrelsz = v; break;
        case DT_INIT: m->init = b + v; break;
        case DT_FINI: m->fini = b + v; break;
        case DT_INIT_ARRAY: m->init_array = b + v; break;
        case DT_INIT_ARRAYSZ: m->init_arraysz = v; break;
        case DT_FINI_ARRAY: m->fini_array = b + v; break;
        case DT_FINI_ARRAYSZ: m->fini_arraysz = v; break;
        case DT_VERSYM: m->versym = (const uint16_t *)(b + v); break;
        case DT_VERDEF: verdef = b + v; break;
        case DT_VERDEFNUM: verdefnum = v; break;
        case DT_VERNEED: verneed = b + v; break;
        case DT_VERNEEDNUM: verneednum = v; break;
        }
    }
    for (unsigned i = 0; i < m->nneeded; i++)
        m->needed[i] = m->strtab + needed_off[i];

    /* symbol count: from DT_HASH nchain, else walk GNU hash chains */
    if (m->hash) {
        m->nsyms = m->hash[1];
    } else if (m->gnu_hash) {
        const uint32_t *h = m->gnu_hash;
        uint32_t nbuckets = h[0], symoffset = h[1], bloom = h[2];
        const uint32_t *buckets = h + 4 + bloom;
        const uint32_t *chain = buckets + nbuckets;
        uint32_t max = 0;
        for (uint32_t i = 0; i < nbuckets; i++)
            if (buckets[i] > max)
                max = buckets[i];
        if (max >= symoffset) {
            while (!(chain[max - symoffset] & 1))
                max++;
            m->nsyms = max + 1;
        } else {
            m->nsyms = symoffset;
        }
    }

    /* version index -> name */
    unsigned maxidx = 1;
    const Elf32_Verdef *vd = (const Elf32_Verdef *)verdef;
    for (unsigned i = 0; vd && i < verdefnum; i++) {
        if (vd->vd_ndx > maxidx)
            maxidx = vd->vd_ndx;
        vd = vd->vd_next ? (const Elf32_Verdef *)((const char *)vd + vd->vd_next) : NULL;
    }
    const Elf32_Verneed *vn = (const Elf32_Verneed *)verneed;
    for (unsigned i = 0; vn && i < verneednum; i++) {
        const Elf32_Vernaux *a = (const Elf32_Vernaux *)((const char *)vn + vn->vn_aux);
        for (unsigned j = 0; j < vn->vn_cnt; j++) {
            if ((a->vna_other & 0x7fff) > maxidx)
                maxidx = a->vna_other & 0x7fff;
            a = (const Elf32_Vernaux *)((const char *)a + a->vna_next);
        }
        vn = vn->vn_next ? (const Elf32_Verneed *)((const char *)vn + vn->vn_next) : NULL;
    }
    m->nvernames = maxidx + 1;
    m->vernames = calloc(m->nvernames, sizeof(char *));
    vd = (const Elf32_Verdef *)verdef;
    for (unsigned i = 0; vd && i < verdefnum; i++) {
        if (!(vd->vd_flags & 1)) /* VER_FLG_BASE names the file, not a version */
            m->vernames[vd->vd_ndx & 0x7fff] = vername_def(m, vd);
        vd = vd->vd_next ? (const Elf32_Verdef *)((const char *)vd + vd->vd_next) : NULL;
    }
    vn = (const Elf32_Verneed *)verneed;
    for (unsigned i = 0; vn && i < verneednum; i++) {
        const Elf32_Vernaux *a = (const Elf32_Vernaux *)((const char *)vn + vn->vn_aux);
        for (unsigned j = 0; j < vn->vn_cnt; j++) {
            m->vernames[a->vna_other & 0x7fff] = m->strtab + a->vna_name;
            a = (const Elf32_Vernaux *)((const char *)a + a->vna_next);
        }
        vn = vn->vn_next ? (const Elf32_Verneed *)((const char *)vn + vn->vn_next) : NULL;
    }
}

LX_EXPORT void lx_register(lx_module_desc *desc)
{
    if (desc->magic != LX_MODULE_MAGIC || desc->version != 1)
        lx_fatal("lx_register: bad module descriptor at %p", (void *)desc);
    if (lx_nmodules >= LX_MAX_MODULES)
        lx_fatal("lx_register: too many modules");
    lx_module *m = &lx_modules[lx_nmodules++];
    memset(m, 0, sizeof *m);
    m->desc = desc;
    m->hmod = (HMODULE)(uintptr_t)desc->image_base;
    m->name = (const char *)(uintptr_t)desc->name;
    m->bias = desc->bias;
    m->phdr = (const Elf32_Phdr *)(uintptr_t)desc->phdr;
    m->phnum = desc->phnum;
    m->dyn = (const Elf32_Dyn *)(uintptr_t)desc->dynamic;
    desc->canary = lx_canary_value;
    module_path(m);
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)((const char *)m->hmod + ((const IMAGE_DOS_HEADER *)m->hmod)->e_lfanew);
    m->map_start = (uintptr_t)m->hmod;
    m->map_end = m->map_start + nt->OptionalHeader.SizeOfImage;
    for (unsigned i = 0; i < m->phnum; i++) {
        const Elf32_Phdr *p = &m->phdr[i];
        if (p->p_type == PT_TLS && p->p_memsz) {
            m->tls_modid = next_tls_modid++;
            m->tls_image = (const void *)(m->bias + p->p_vaddr);
            m->tls_filesz = p->p_filesz;
            m->tls_memsz = p->p_memsz;
            m->tls_align = p->p_align ? p->p_align : 1;
        }
    }
    if (m->dyn)
        parse_dynamic(m);
    if (lx_trace)
        lx_log("register %s at %p bias %#x (%s)", m->name, (void *)m->hmod, (unsigned)m->bias, m->path);
}

lx_module *lx_module_by_addr(uintptr_t addr)
{
    for (unsigned i = 0; i < lx_nmodules; i++)
        if (addr >= lx_modules[i].map_start && addr < lx_modules[i].map_end)
            return &lx_modules[i];
    return NULL;
}

static lx_module *module_by_soname(const char *name)
{
    for (unsigned i = 0; i < lx_nmodules; i++)
        if (!strcmp(lx_modules[i].name, name))
            return &lx_modules[i];
    return NULL;
}

/* ---- symbol lookup --------------------------------------------------------------------- */

static uint32_t sysv_hash(const char *s)
{
    uint32_t h = 0, g;
    while (*s) {
        h = (h << 4) + (unsigned char)*s++;
        g = h & 0xf0000000u;
        if (g)
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static uint32_t gnu_hash(const char *s)
{
    uint32_t h = 5381;
    while (*s)
        h = h * 33 + (unsigned char)*s++;
    return h;
}

/* Does definition `idx` in `m` satisfy a request for `version` (NULL = unversioned)? */
static int version_ok(lx_module *m, unsigned idx, const char *version)
{
    if (!m->versym)
        return 1;
    uint16_t vs = m->versym[idx];
    unsigned ndx = vs & 0x7fff;
    if (!version)
        return !(vs & 0x8000);          /* unversioned refs bind to the default version */
    if (ndx <= 1)
        return 1;                        /* local/global base definition matches anything */
    const char *have = ndx < m->nvernames ? m->vernames[ndx] : NULL;
    return !have || !strcmp(have, version);
}

static int sym_defined(const Elf32_Sym *s)
{
    unsigned bind = ELF32_ST_BIND(s->st_info);
    if (s->st_shndx == SHN_UNDEF)
        return 0;
    if (bind != STB_GLOBAL && bind != STB_WEAK && bind != STB_GNU_UNIQUE)
        return 0;
    return s->st_value != 0 || ELF32_ST_TYPE(s->st_info) == STT_TLS;
}

static const Elf32_Sym *module_lookup(lx_module *m, const char *name, const char *version)
{
    if (!m->symtab)
        return NULL;
    if (m->gnu_hash) {
        const uint32_t *h = m->gnu_hash;
        uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2], shift = h[3];
        const uint32_t *bloom = h + 4;
        const uint32_t *buckets = bloom + bloom_size;
        const uint32_t *chain = buckets + nbuckets;
        uint32_t hv = gnu_hash(name);
        uint32_t word = bloom[(hv / 32) % bloom_size];
        if (!((word >> (hv % 32)) & (word >> ((hv >> shift) % 32)) & 1))
            return NULL;
        uint32_t i = buckets[hv % nbuckets];
        if (i < symoffset)
            return NULL;
        for (;; i++) {
            uint32_t ch = chain[i - symoffset];
            if ((ch | 1) == (hv | 1)) {
                const Elf32_Sym *s = &m->symtab[i];
                if (sym_defined(s) && !strcmp(m->strtab + s->st_name, name) && version_ok(m, i, version))
                    return s;
            }
            if (ch & 1)
                break;
        }
        return NULL;
    }
    if (m->hash) {
        uint32_t nbucket = m->hash[0];
        const uint32_t *bucket = m->hash + 2, *chain = bucket + nbucket;
        for (uint32_t i = bucket[sysv_hash(name) % nbucket]; i; i = chain[i]) {
            const Elf32_Sym *s = &m->symtab[i];
            if (sym_defined(s) && !strcmp(m->strtab + s->st_name, name) && version_ok(m, i, version))
                return s;
        }
    }
    return NULL;
}

typedef struct { lx_module *mod; const Elf32_Sym *sym; void *native; } lx_def;

/* Search the global scope (skipping `skip`, starting after `after`), then native providers. */
static int lookup(const char *name, const char *version, lx_module *after, lx_module *skip, lx_def *out)
{
    unsigned i = 0;
    if (after) {
        while (i < lx_nscope && lx_scope[i] != after)
            i++;
        i++;
    }
    for (; i < lx_nscope; i++) {
        lx_module *m = lx_scope[i];
        if (m == skip)
            continue;
        const Elf32_Sym *s = module_lookup(m, name, version);
        if (s) {
            out->mod = m;
            out->sym = s;
            out->native = NULL;
            return 1;
        }
    }
    int found = 0;
    void *p = lx_native_lookup(name, version, &found);
    if (found) {
        out->mod = NULL;
        out->sym = NULL;
        out->native = p;
        return 1;
    }
    return 0;
}

static uintptr_t def_addr(const lx_def *d)
{
    if (!d->mod)
        return (uintptr_t)d->native;
    /* SHN_ABS (0xfff1) values are absolute, everything else is relative to the load bias */
    uintptr_t a = (d->sym->st_shndx == 0xfff1 ? 0 : d->mod->bias) + d->sym->st_value;
    if (ELF32_ST_TYPE(d->sym->st_info) == STT_GNU_IFUNC)
        a = ((uintptr_t(*)(void))a)();
    return a;
}

void *lx_lookup_global(const char *name, const char *version, lx_module *after, int *found)
{
    lx_def d;
    *found = lookup(name, version, after, NULL, &d);
    return *found ? (void *)def_addr(&d) : NULL;
}

LX_EXPORT void *lx_guest_symbol(const char *name)
{
    int found;
    void *p = lx_lookup_global(name, NULL, NULL, &found);
    return found ? p : NULL;
}

/* ---- unresolved-symbol trap stubs ---------------------------------------------------- */

static unsigned char *trap_page;
static size_t trap_used, trap_cap;

static LX_NORETURN void __cdecl trap_hit(const char *name)
{
    lx_fatal("call to unimplemented function '%s'", name);
}

static void *make_trap(const char *name)
{
    /* push imm32(name) ; mov eax, trap_hit ; call eax ; int3   (16 bytes) */
    if (!trap_page || trap_used + 16 > trap_cap) {
        trap_cap = 1 << 16;
        trap_page = VirtualAlloc(NULL, trap_cap, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        trap_used = 0;
        if (!trap_page)
            lx_fatal("trap stub allocation failed");
    }
    unsigned char *p = trap_page + trap_used;
    trap_used += 16;
    p[0] = 0x68; *(uint32_t *)(p + 1) = (uint32_t)(uintptr_t)name;
    p[5] = 0xB8; *(uint32_t *)(p + 6) = (uint32_t)(uintptr_t)trap_hit;
    p[10] = 0xFF; p[11] = 0xD0; p[12] = 0xCC; p[13] = 0xCC; p[14] = 0xCC; p[15] = 0xCC;
    FlushInstructionCache(GetCurrentProcess(), p, 16);
    return p;
}

/* ---- relocation ------------------------------------------------------------------------ */

static const char *sym_version(lx_module *m, unsigned idx)
{
    if (!m->versym)
        return NULL;
    unsigned ndx = m->versym[idx] & 0x7fff;
    return ndx > 1 && ndx < m->nvernames ? m->vernames[ndx] : NULL;
}

static uintptr_t resolve(lx_module *m, unsigned symidx, int type, lx_def *d)
{
    const Elf32_Sym *s = &m->symtab[symidx];
    const char *name = m->strtab + s->st_name;
    if (ELF32_ST_BIND(s->st_info) == STB_LOCAL) {
        d->mod = m; d->sym = s; d->native = NULL;
        return def_addr(d);
    }
    if (lookup(name, sym_version(m, symidx), NULL, type == R_386_COPY ? m : NULL, d))
        return def_addr(d);
    if (ELF32_ST_BIND(s->st_info) == STB_WEAK) {
        d->mod = NULL; d->native = NULL;
        return 0;
    }
    const char *ver = sym_version(m, symidx);
    if (type == R_386_JMP_SLOT || ELF32_ST_TYPE(s->st_info) == 2 /* STT_FUNC */) {
        if (lx_trace)
            lx_log("%s: unresolved function %s%s%s -> trap", m->name, name, ver ? "@" : "", ver ? ver : "");
    } else {
        lx_log("%s: unresolved data symbol %s%s%s", m->name, name, ver ? "@" : "", ver ? ver : "");
    }
    d->mod = NULL; d->native = NULL;
    return (uintptr_t)make_trap(name);
}

/* ---- native data interposed by R_386_COPY ----------------------------------------------
 * When the EXE copies a native data object (optind, stdout, tzname, ...), the copy becomes the
 * one true definition, as with glibc. Native code that mutates or re-reads such objects must go
 * through lx_data_addr(&native_var). */
#define MAX_DATA_ALIASES 64
static struct { void *native, *guest; } data_aliases[MAX_DATA_ALIASES];
static unsigned ndata_aliases;

static void add_data_alias(void *native, void *guest)
{
    if (ndata_aliases == MAX_DATA_ALIASES)
        lx_fatal("too many COPY-interposed native data objects");
    data_aliases[ndata_aliases].native = native;
    data_aliases[ndata_aliases].guest = guest;
    ndata_aliases++;
}

void *lx_data_addr(void *native)
{
    for (unsigned i = 0; i < ndata_aliases; i++)
        if (data_aliases[i].native == native)
            return data_aliases[i].guest;
    return native;
}

static void apply_rels(lx_module *m, const Elf32_Rel *rel, unsigned size, int copy_pass)
{
    for (unsigned i = 0; i < size / sizeof(Elf32_Rel); i++) {
        uint32_t *P = (uint32_t *)(m->bias + rel[i].r_offset);
        unsigned type = rel[i].r_info & 0xff, symidx = rel[i].r_info >> 8;
        lx_def d = {0};
        if (copy_pass != (type == R_386_COPY))
            continue;
        switch (type) {
        case R_386_NONE:
        case R_386_RELATIVE: /* pre-applied by relink.py */
            break;
        case R_386_32:
            *P += symidx ? resolve(m, symidx, type, &d) : m->bias;
            break;
        case R_386_PC32:
            *P += resolve(m, symidx, type, &d) - (uintptr_t)P;
            break;
        case R_386_GLOB_DAT:
        case R_386_JMP_SLOT:
            *P = resolve(m, symidx, type, &d);
            break;
        case R_386_COPY: {
            uintptr_t src = resolve(m, symidx, type, &d);
            if (!d.mod && !d.native)
                lx_fatal("%s: COPY of undefined %s", m->name, m->strtab + m->symtab[symidx].st_name);
            memcpy(P, (void *)src, m->symtab[symidx].st_size);
            if (d.native)
                add_data_alias((void *)src, P);
            break;
        }
        case R_386_TLS_DTPMOD32:
            if (!symidx) {
                *P = m->tls_modid;
            } else {
                resolve(m, symidx, type, &d);
                *P = d.mod ? d.mod->tls_modid : 0;
            }
            break;
        case R_386_TLS_DTPOFF32:
            if (symidx) {
                resolve(m, symidx, type, &d);
                *P += d.mod ? d.sym->st_value : 0;
            }
            break;
        case R_386_IRELATIVE:
            *P = ((uint32_t(*)(void))(m->bias + *P))();
            break;
        case R_386_TLS_TPOFF:
        case R_386_TLS_TPOFF32:
            lx_fatal("%s: initial-exec TLS relocation %u at %p is not supported", m->name, type, (void *)P);
        default:
            lx_fatal("%s: unsupported relocation type %u at %p", m->name, type, (void *)P);
        }
    }
}

static void unprotect(lx_module *m, DWORD *old)
{
    if (!VirtualProtect((void *)m->map_start, m->map_end - m->map_start, PAGE_EXECUTE_READWRITE, old))
        lx_fatal("%s: VirtualProtect failed (%lu)", m->name, GetLastError());
}

/* Restore per-section protections from the PE section table. */
static void reprotect(lx_module *m)
{
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)((const char *)m->hmod + ((const IMAGE_DOS_HEADER *)m->hmod)->e_lfanew);
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    DWORD old;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        DWORD c = s->Characteristics, prot;
        int x = !!(c & IMAGE_SCN_MEM_EXECUTE), w = !!(c & IMAGE_SCN_MEM_WRITE);
        prot = x ? (w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ) : (w ? PAGE_READWRITE : PAGE_READONLY);
        VirtualProtect((char *)m->hmod + s->VirtualAddress, s->Misc.VirtualSize, prot, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), (void *)m->map_start, m->map_end - m->map_start);
}

static void relocate(lx_module *m)
{
    DWORD old;
    if (m->relocated)
        return;
    m->relocated = 1;
    unprotect(m, &old);
    apply_rels(m, m->rel, m->relsz, 0);
    apply_rels(m, m->jmprel, m->pltrelsz, 0);
    reprotect(m);
}

static void apply_copies(lx_module *m)
{
    DWORD old;
    unprotect(m, &old);
    apply_rels(m, m->rel, m->relsz, 1);
    reprotect(m);
}

/* ---- init / fini ----------------------------------------------------------------------- */

typedef void (*init_fn)(int, char **, char **);
static int g_argc;
static char **g_argv;

static void visit_init(lx_module *m)
{
    if (m->initialized)
        return;
    m->initialized = 1;
    for (unsigned i = 0; i < m->nneeded; i++) {
        lx_module *dep = module_by_soname(m->needed[i]);
        if (dep && !(dep->desc->flags & (LX_MOD_EXE | LX_MOD_PRELOAD)))
            visit_init(dep);
    }
    init_order[ninit++] = m;
}

static void run_init(lx_module *m)
{
    if (lx_trace)
        lx_log("init %s", m->name);
    if (m->init)
        ((init_fn)m->init)(g_argc, g_argv, lx_environ);
    for (unsigned i = 0; i < m->init_arraysz / 4; i++) {
        uintptr_t f = ((uintptr_t *)m->init_array)[i];
        if (f && f != (uintptr_t)-1)
            ((init_fn)f)(g_argc, g_argv, lx_environ);
    }
}

/* Called from exit() after atexit handlers: DSO destructors in reverse init order. */
void lx_run_finalizers(void)
{
    for (unsigned k = ninit; k-- > 0;) {
        lx_module *m = init_order[k];
        if (m->finalized || (m->desc->flags & LX_MOD_EXE))
            continue;
        m->finalized = 1;
        for (unsigned i = m->fini_arraysz / 4; i-- > 0;) {
            uintptr_t f = ((uintptr_t *)m->fini_array)[i];
            if (f && f != (uintptr_t)-1)
                ((lx_voidfn)f)();
        }
        if (m->fini)
            ((lx_voidfn)m->fini)();
    }
}

/* ---- TLS (general dynamic) --------------------------------------------------------------
 * Each thread owns a DTV (array indexed by module id) stored in a TlsAlloc slot; blocks are
 * created lazily from the module's PT_TLS image. */
static DWORD dtv_slot = TLS_OUT_OF_INDEXES;

void *lx_tls_get_addr(unsigned modid, unsigned offset)
{
    void **dtv = TlsGetValue(dtv_slot);
    if (!dtv) {
        dtv = calloc(LX_MAX_MODULES + 1, sizeof(void *));
        TlsSetValue(dtv_slot, dtv);
    }
    if (modid == 0 || modid > LX_MAX_MODULES)
        lx_fatal("__tls_get_addr: bad module id %u", modid);
    if (!dtv[modid]) {
        lx_module *m = NULL;
        for (unsigned i = 0; i < lx_nmodules; i++)
            if (lx_modules[i].tls_modid == modid)
                m = &lx_modules[i];
        if (!m)
            lx_fatal("__tls_get_addr: unknown module id %u", modid);
        unsigned al = m->tls_align < 16 ? 16 : m->tls_align;
        char *blk = _aligned_malloc(m->tls_memsz ? m->tls_memsz : 1, al);
        memcpy(blk, m->tls_image, m->tls_filesz);
        memset(blk + m->tls_filesz, 0, m->tls_memsz - m->tls_filesz);
        dtv[modid] = blk;
    }
    return (char *)dtv[modid] + offset;
}

void lx_tls_thread_exit(void)
{
    if (dtv_slot == TLS_OUT_OF_INDEXES)
        return;
    void **dtv = TlsGetValue(dtv_slot);
    if (!dtv)
        return;
    for (unsigned i = 0; i <= LX_MAX_MODULES; i++)
        if (dtv[i])
            _aligned_free(dtv[i]);
    free(dtv);
    TlsSetValue(dtv_slot, NULL);
}

typedef struct { unsigned long ti_module, ti_offset; } tls_index;

/* glibc i386: ___tls_get_addr takes the tls_index* in EAX (regparm(1)); __tls_get_addr on stack. */
static void *__attribute__((regparm(1))) tls_get_addr_regparm(tls_index *ti)
{
    return lx_tls_get_addr(ti->ti_module, ti->ti_offset);
}

static void *__cdecl tls_get_addr_stack(tls_index *ti)
{
    return lx_tls_get_addr(ti->ti_module, ti->ti_offset);
}

/* ---- dl* API --------------------------------------------------------------------------- */

typedef struct {
    uintptr_t dlpi_addr;
    const char *dlpi_name;
    const Elf32_Phdr *dlpi_phdr;
    uint16_t dlpi_phnum;
    uint16_t pad_;                 /* i386: long long members are 4-byte aligned */
    unsigned long long dlpi_adds, dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
} __attribute__((packed, aligned(4))) lx_dl_phdr_info;
static_assert(sizeof(lx_dl_phdr_info) == 40, "dl_phdr_info i386 size");
static_assert(offsetof(lx_dl_phdr_info, dlpi_adds) == 16, "dlpi_adds offset");

static unsigned long long dl_adds;

static int dl_iterate_phdr(int (*cb)(lx_dl_phdr_info *, size_t, void *), void *data)
{
    for (unsigned i = 0; i < lx_nscope; i++) {
        lx_module *m = lx_scope[i];
        lx_dl_phdr_info info;
        memset(&info, 0, sizeof info);
        info.dlpi_addr = m->bias;
        info.dlpi_name = (m->desc->flags & LX_MOD_EXE) ? "" : m->path;
        info.dlpi_phdr = m->phdr;
        info.dlpi_phnum = (uint16_t)m->phnum;
        info.dlpi_adds = dl_adds;
        info.dlpi_subs = 0;
        info.dlpi_tls_modid = m->tls_modid;
        info.dlpi_tls_data = m->tls_modid ? lx_tls_get_addr(m->tls_modid, 0) : NULL;
        int r = cb(&info, sizeof info, data);
        if (r)
            return r;
    }
    return 0;
}

#define LX_RTLD_NEXT ((void *)-1)
#define LX_RTLD_DEFAULT ((void *)0)
static char dl_error_buf[512];
static int dl_error_set;
/* dlopen handles: lx_module* for guest modules; (lx_module*)&dl_global for the main program. */
static int dl_global;

static void dl_seterr(const char *fmt, const char *arg)
{
    snprintf(dl_error_buf, sizeof dl_error_buf, fmt, arg);
    dl_error_set = 1;
}

static void *dlopen_(const char *file, int mode)
{
    (void)mode;
    if (!file)
        return &dl_global;
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    lx_module *m = module_by_soname(base);
    if (m)
        return m;
    /* native-provided "system" libraries: the global namespace serves them */
    static const char *const system_libs[] = {
        "libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1", "libGL.so.1",
        "libGLU.so.1", "libX11.so.6", "libXxf86vm.so.1", "libasound.so.2", NULL };
    for (const char *const *s = system_libs; *s; s++)
        if (!strcmp(*s, base))
            return &dl_global;
    dl_seterr("%s: cannot open shared object file: No such file or directory", file);
    return NULL;
}

static void *dlsym_impl(void *handle, const char *name, const char *version, uintptr_t caller)
{
    lx_def d;
    int ok;
    if (handle == LX_RTLD_NEXT) {
        /* search the scope after the module containing the caller */
        ok = lookup(name, version, lx_module_by_addr(caller), NULL, &d);
    } else if (handle == LX_RTLD_DEFAULT || handle == &dl_global) {
        ok = lookup(name, version, NULL, NULL, &d);
    } else {
        lx_module *m = handle;
        const Elf32_Sym *s = module_lookup(m, name, version);
        ok = s != NULL;
        if (ok) {
            d.mod = m; d.sym = s; d.native = NULL;
        }
    }
    if (!ok) {
        dl_seterr("undefined symbol: %s", name);
        return NULL;
    }
    if (d.mod && ELF32_ST_TYPE(d.sym->st_info) == STT_TLS)
        return lx_tls_get_addr(d.mod->tls_modid, d.sym->st_value);
    return (void *)def_addr(&d);
}

static void *dlsym_(void *handle, const char *name)
{
    return dlsym_impl(handle, name, NULL, (uintptr_t)__builtin_return_address(0));
}

static void *dlvsym_(void *handle, const char *name, const char *version)
{
    return dlsym_impl(handle, name, version, (uintptr_t)__builtin_return_address(0));
}

static int dlclose_(void *h)
{
    (void)h;
    return 0;
}

static char *dlerror_(void)
{
    if (!dl_error_set)
        return NULL;
    dl_error_set = 0;
    return dl_error_buf;
}

typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } lx_Dl_info;

static int dladdr_(const void *addr, lx_Dl_info *info)
{
    lx_module *m = lx_module_by_addr((uintptr_t)addr);
    memset(info, 0, sizeof *info);
    if (!m) {
        static char native_name[MAX_PATH * 3];
        uintptr_t base;
        if (lx_native_owner((uintptr_t)addr, native_name, sizeof native_name, &base)) {
            info->dli_fname = native_name;
            info->dli_fbase = (void *)base;
            return 1;
        }
        return 0;
    }
    info->dli_fname = m->path;
    info->dli_fbase = (void *)m->bias;
    /* nearest preceding dynamic symbol */
    const Elf32_Sym *best = NULL;
    for (unsigned i = 1; i < m->nsyms; i++) {
        const Elf32_Sym *s = &m->symtab[i];
        if (!sym_defined(s) || ELF32_ST_TYPE(s->st_info) == STT_TLS)
            continue;
        uintptr_t a = m->bias + s->st_value;
        if (a <= (uintptr_t)addr && (a + s->st_size > (uintptr_t)addr || !s->st_size) &&
            (!best || s->st_value > best->st_value))
            best = s;
    }
    if (best) {
        info->dli_sname = m->strtab + best->st_name;
        info->dli_saddr = (void *)(m->bias + best->st_value);
    }
    return 1;
}

static int dladdr1_(const void *addr, lx_Dl_info *info, void **extra, int flags)
{
    (void)flags;
    if (extra)
        *extra = NULL;
    return dladdr_(addr, info);
}

LX_SYMTAB(loader,
    LX_SYM("___tls_get_addr", tls_get_addr_regparm),
    LX_SYM("__tls_get_addr", tls_get_addr_stack),
    LX_SYM("dl_iterate_phdr", dl_iterate_phdr),
    LX_SYM("dlopen", dlopen_),
    LX_SYM("dlmopen", dlopen_),
    LX_SYM("dlsym", dlsym_),
    LX_SYM("dlvsym", dlvsym_),
    LX_SYM("dlclose", dlclose_),
    LX_SYM("dlerror", dlerror_),
    LX_SYM("dladdr", dladdr_),
    LX_SYM("dladdr1", dladdr1_),
);

/* ---- lx_start -------------------------------------------------------------------------- */

static void build_scope(lx_module *exe)
{
    lx_scope[lx_nscope++] = exe;
    for (unsigned i = 0; i < lx_nmodules; i++)
        if (lx_modules[i].desc->flags & LX_MOD_PRELOAD)
            lx_scope[lx_nscope++] = &lx_modules[i];
    /* breadth-first over DT_NEEDED, like ld.so */
    for (unsigned q = 0; q < lx_nscope; q++) {
        lx_module *m = lx_scope[q];
        for (unsigned i = 0; i < m->nneeded; i++) {
            lx_module *dep = module_by_soname(m->needed[i]);
            if (!dep)
                continue; /* provided natively */
            unsigned k;
            for (k = 0; k < lx_nscope && lx_scope[k] != dep; k++)
                ;
            if (k == lx_nscope)
                lx_scope[lx_nscope++] = dep;
        }
    }
    /* modules registered but not reachable (shouldn't happen) still get a scope slot */
    for (unsigned i = 0; i < lx_nmodules; i++) {
        unsigned k;
        for (k = 0; k < lx_nscope && lx_scope[k] != &lx_modules[i]; k++)
            ;
        if (k == lx_nscope)
            lx_scope[lx_nscope++] = &lx_modules[i];
    }
}

enum { AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5, AT_PAGESZ = 6, AT_BASE = 7, AT_FLAGS = 8,
       AT_ENTRY = 9, AT_UID = 11, AT_EUID = 12, AT_GID = 13, AT_EGID = 14, AT_HWCAP = 16, AT_CLKTCK = 17,
       AT_SECURE = 23, AT_RANDOM = 25, AT_EXECFN = 31 };

extern void lx_process_init(int *argc, char ***argv);

LX_EXPORT LX_NORETURN void lx_start(lx_module_desc *exe_desc)
{
    lx_register(exe_desc);
    lx_module *exe = &lx_modules[lx_nmodules - 1];
    dtv_slot = TlsAlloc();
    lx_process_init(&g_argc, &g_argv);
    build_scope(exe);
    if (lx_trace)
        for (unsigned i = 0; i < lx_nscope; i++)
            lx_log("scope[%u] %s", i, lx_scope[i]->name);

    /* 1. relocate DLLs, 2. EXE (COPY last, after its sources are relocated) */
    for (unsigned i = 1; i < lx_nscope; i++)
        relocate(lx_scope[i]);
    relocate(exe);
    apply_copies(exe);

    /* init: dependencies first, preload modules last before the EXE (whose init runs via
     * __libc_csu_init from __libc_start_main). */
    for (unsigned i = 1; i < lx_nscope; i++)
        if (!(lx_scope[i]->desc->flags & LX_MOD_PRELOAD))
            visit_init(lx_scope[i]);
    for (unsigned i = 1; i < lx_nscope; i++)
        if (lx_scope[i]->desc->flags & LX_MOD_PRELOAD) {
            lx_scope[i]->initialized = 0;
            visit_init(lx_scope[i]);
        }
    for (unsigned i = 0; i < ninit; i++)
        run_init(init_order[i]);
    exe->initialized = 1;
    init_order[ninit++] = exe;

    /* Linux initial stack: argc, argv[], NULL, envp[], NULL, auxv[] */
    unsigned nenv = 0;
    while (lx_environ[nenv])
        nenv++;
    static unsigned char random_bytes[16];
    for (int i = 0; i < 16; i++)
        random_bytes[i] = (unsigned char)(lx_canary_value >> ((i & 3) * 8)) ^ (unsigned char)(i * 37);
    uintptr_t auxv[] = {
        AT_PHDR, (uintptr_t)exe->phdr, AT_PHENT, sizeof(Elf32_Phdr), AT_PHNUM, exe->phnum,
        AT_PAGESZ, 4096, AT_BASE, 0, AT_FLAGS, 0, AT_ENTRY, exe->desc->entry,
        AT_UID, 1000, AT_EUID, 1000, AT_GID, 1000, AT_EGID, 1000, AT_HWCAP, 0x178bfbff,
        AT_CLKTCK, 100, AT_SECURE, 0, AT_RANDOM, (uintptr_t)random_bytes,
        AT_EXECFN, (uintptr_t)g_argv[0], AT_NULL, 0 };
    unsigned words = 1 + g_argc + 1 + nenv + 1 + LX_COUNT(auxv);
    uintptr_t *sp = (uintptr_t *)(((uintptr_t)_alloca(words * 4 + 64) + 15) & ~(uintptr_t)15);
    unsigned k = 0;
    sp[k++] = g_argc;
    for (int i = 0; i < g_argc; i++)
        sp[k++] = (uintptr_t)g_argv[i];
    sp[k++] = 0;
    for (unsigned i = 0; i < nenv; i++)
        sp[k++] = (uintptr_t)lx_environ[i];
    sp[k++] = 0;
    for (unsigned i = 0; i < LX_COUNT(auxv); i++)
        sp[k++] = auxv[i];
    if (lx_trace)
        lx_log("entering %s at %#x", exe->name, exe->desc->entry);
    /* Linux starts processes with x87 CW 0x037F (64-bit mantissa); Windows uses 0x027F (53-bit). */
    static const uint16_t linux_fpucw = 0x037F;
    __asm__ volatile("fldcw %0" :: "m"(linux_fpucw));
    __asm__ volatile(
        "movl %0, %%esp\n\t"
        "xorl %%ebp, %%ebp\n\t"
        "xorl %%edx, %%edx\n\t"
        "jmpl *%1"
        :: "r"(sp), "r"(exe->desc->entry) : "memory");
    __builtin_unreachable();
}
