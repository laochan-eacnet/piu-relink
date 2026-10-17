/* i386 glibc jmp_buf layout, with intentionally unmangled saved pointers: guest
 * code treats the buffer as opaque. Naked entry points snapshot the caller before
 * any compiler-generated prologue; asm uses Win32's decorated C names. setjmp
 * saves masks, _setjmp does not, and __sigsetjmp obeys its flag. __longjmp_chk
 * restores identically without glibc's fortify bounds check on our custom stack. */
#include "lxrt.h"

extern int lx_signal_mask(int how, const void *set, void *old);
extern void lx_signal_longjmp(uintptr_t target_stack);

typedef struct {
    uint32_t ebx, esi, edi, ebp, esp, eip;
    int32_t mask_was_saved;
    uint32_t mask[32];
} lx_jmp_buf;

static_assert(sizeof(lx_jmp_buf) == 156, "i386 glibc jmp_buf size");
static_assert(offsetof(lx_jmp_buf, ebx) == 0, "jmp_buf ebx offset");
static_assert(offsetof(lx_jmp_buf, esi) == 4, "jmp_buf esi offset");
static_assert(offsetof(lx_jmp_buf, edi) == 8, "jmp_buf edi offset");
static_assert(offsetof(lx_jmp_buf, ebp) == 12, "jmp_buf ebp offset");
static_assert(offsetof(lx_jmp_buf, esp) == 16, "jmp_buf esp offset");
static_assert(offsetof(lx_jmp_buf, eip) == 20, "jmp_buf eip offset");
static_assert(offsetof(lx_jmp_buf, mask_was_saved) == 24, "jmp_buf mask flag offset");
static_assert(offsetof(lx_jmp_buf, mask) == 28, "jmp_buf signal mask offset");
static_assert(sizeof(((lx_jmp_buf *)0)->mask) == 128, "jmp_buf signal mask size");

/* Referenced from basic asm, so retain these even though C has no direct calls. */
__attribute__((used)) void lx_setjmp_save_mask(lx_jmp_buf *env)
{
    env->mask_was_saved = lx_signal_mask(2, NULL, env->mask) == 0;
}

__attribute__((used)) void lx_longjmp_prepare(lx_jmp_buf *env)
{
    lx_signal_longjmp(env->esp);
    if (env->mask_was_saved && lx_signal_mask(2, env->mask, NULL) < 0)
        lx_fatal("cannot restore saved signal mask in longjmp");
}

/* EAX is an internal save-mask argument; ECX/EDX are caller-clobbered scratch.
 * Saved ESP is the stack immediately after the guest call's return address. */
__attribute__((naked, used, returns_twice)) int lx_setjmp_common(lx_jmp_buf *env)
{
    __asm__ volatile(
        "movl 4(%esp), %ecx\n\t"
        "movl %ebx, 0(%ecx)\n\t"
        "movl %esi, 4(%ecx)\n\t"
        "movl %edi, 8(%ecx)\n\t"
        "movl %ebp, 12(%ecx)\n\t"
        "leal 4(%esp), %edx\n\t"
        "movl %edx, 16(%ecx)\n\t"
        "movl 0(%esp), %edx\n\t"
        "movl %edx, 20(%ecx)\n\t"
        "movl $0, 24(%ecx)\n\t"
        "testl %eax, %eax\n\t"
        "jz 1f\n\t"
        "pushl %ecx\n\t"
        "calll _lx_setjmp_save_mask\n\t"
        "addl $4, %esp\n\t"
        "1:\n\t"
        "xorl %eax, %eax\n\t"
        "retl\n\t"
    );
}

__attribute__((naked, returns_twice)) int lx_setjmp_plain(lx_jmp_buf *env)
{
    __asm__ volatile(
        "xorl %eax, %eax\n\t"
        "jmp _lx_setjmp_common\n\t"
    );
}

__attribute__((naked, returns_twice)) int lx_setjmp_mask(lx_jmp_buf *env)
{
    __asm__ volatile(
        "movl $1, %eax\n\t"
        "jmp _lx_setjmp_common\n\t"
    );
}

__attribute__((naked, returns_twice)) int lx_sigsetjmp(lx_jmp_buf *env, int save_mask)
{
    __asm__ volatile(
        "movl 8(%esp), %eax\n\t"
        "jmp _lx_setjmp_common\n\t"
    );
}

/* All glibc longjmp entry points honor mask_was_saved, including _longjmp.
 * __longjmp_chk shares the restoration path: no glibc stack-bound fortify check
 * is possible for the custom guest startup stack, and no pointer mangling is used. */
__attribute__((naked, noreturn)) void lx_longjmp(lx_jmp_buf *env, int value)
{
    __asm__ volatile(
        "pushl 4(%esp)\n\t"
        "calll _lx_longjmp_prepare\n\t"
        "addl $4, %esp\n\t"
        "movl 4(%esp), %edx\n\t"
        "movl 8(%esp), %eax\n\t"
        "testl %eax, %eax\n\t"
        "jnz 1f\n\t"
        "incl %eax\n\t"
        "1:\n\t"
        "movl 0(%edx), %ebx\n\t"
        "movl 4(%edx), %esi\n\t"
        "movl 8(%edx), %edi\n\t"
        "movl 12(%edx), %ebp\n\t"
        "movl 16(%edx), %esp\n\t"
        "jmp *20(%edx)\n\t"
    );
}

LX_SYMTAB(jmpbuf,
    LX_SYM("_setjmp", lx_setjmp_plain),
    LX_SYM("setjmp", lx_setjmp_mask),
    LX_SYM("__sigsetjmp", lx_sigsetjmp),
    LX_SYM("sigsetjmp", lx_sigsetjmp),
    LX_SYM("longjmp", lx_longjmp),
    LX_SYM("_longjmp", lx_longjmp),
    LX_SYM("siglongjmp", lx_longjmp),
    LX_SYM("__longjmp_chk", lx_longjmp),
);
