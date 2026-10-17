#pragma once
/* Contract between lxrt.dll (ELF binder) and native provider DLLs (lxrt itself, lxgfx.dll, lxsnd.dll,
 * lxusb.dll).
 *
 * A provider exports exactly one C entry point:
 *     __declspec(dllexport) const lx_sym *lx_provider_symbols(unsigned *count);
 * returning a table of Linux symbol names and the native addresses that implement them.
 * Functions in the table are called directly by i386 SysV guest code:
 *   - plain cdecl (args on stack, caller cleanup, EAX/EDX/ST0 return);
 *   - NEVER return structs by value (SysV uses a hidden pointer popped by the callee);
 *   - every guest-visible struct must use the i386 SysV layout (long long/double 4-aligned);
 *   - providers are built with -mstackrealign -mstack-alignment=16 so calls back into guest
 *     code are made from 16-byte aligned frames.
 * Data symbols (e.g. `stdin`) point at the object itself, exactly like an ELF definition.
 * `version` is NULL for "any version", else an ELF version name such as "GLIBC_2.3.2";
 * a versioned entry wins over an unversioned one with the same name for that version.
 */
typedef struct lx_sym {
    const char *name;
    const char *version;
    void *addr;
} lx_sym;

/* lxrt.dll export for providers, valid once binding has finished (i.e. from guest calls):
 *     void *lx_guest_symbol(const char *name);
 * Address of the global-scope definition of `name` (EXE, preload, guest DLLs, then native
 * providers), as an unversioned dlsym(RTLD_DEFAULT) would return; NULL when undefined. For a
 * data object the EXE copied via R_386_COPY this is the EXE's live copy. */
typedef void *(*lx_guest_symbol_fn)(const char *name);

typedef const lx_sym *(*lx_provider_symbols_fn)(unsigned *count);

/* Module descriptor emitted by relink.py into section .lxdata (all fields are VAs after
 * Windows base relocation). Layout is fixed: relink.py writes it byte-for-byte. */
#define LX_MODULE_MAGIC 0x314d584cu /* 'LXM1' */
#define LX_MOD_EXE 1u
#define LX_MOD_PRELOAD 2u

typedef struct lx_module_desc {
    unsigned magic;
    unsigned version;      /* 1 */
    unsigned flags;        /* LX_MOD_* */
    unsigned image_base;   /* HMODULE */
    unsigned bias;         /* runtime VA = ELF vaddr + bias (0 for the EXE) */
    unsigned elf_header;   /* VA of the mapped Elf32_Ehdr */
    unsigned phdr;         /* VA of program headers */
    unsigned phnum;
    unsigned dynamic;      /* VA of PT_DYNAMIC */
    unsigned entry;        /* VA of e_entry, 0 if none */
    unsigned canary_slot;  /* VA of `canary` below; target of rewritten %gs:0x14 */
    unsigned name;         /* VA of soname / file name (UTF-8) */
    unsigned canary;       /* stack protector value, set by lxrt before guest code runs */
} lx_module_desc;
