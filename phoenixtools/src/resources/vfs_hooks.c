#include "resources/resource.h"

#include "core/limits.h"
#include "phoenix/addresses.h"
#include "resources/group_paths.h"
#include "resources/song_paths.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>

static void configure_vfs_slot(unsigned int slot)
{
    volatile uintptr_t *context;
    struct phoenix_resource_node **root_slot;

    if (slot >= 3u)
        return;
    context = (volatile uintptr_t *)(0x09791A60u + slot * 20u);
    root_slot = pt_group_root_slot();
    context[1] = (uintptr_t)root_slot;
    context[2] = (uintptr_t)(root_slot + 1);
    context[3] = (uintptr_t)(root_slot + 1);
    context[4] = (uintptr_t)pt_group_root_node();
}

__attribute__((noinline, used))
static int hook_scan_c(void)
{
    unsigned int slot;
    for (slot = 0; slot < 3u; ++slot)
        configure_vfs_slot(slot);
    return 1;
}

__attribute__((naked, noinline))
void hook_scan_entry(void)
{
    __asm__ volatile("call hook_scan_c\n\tret\n\t");
}

__attribute__((naked, noinline))
void hook_resource_next(void)
{
    __asm__ volatile("xor %eax, %eax\n\tret\n\t");
}

__attribute__((noinline, used))
static void *hook_audio_group_select_c(void *slot, const char *component)
{
    size_t length;

    (void)slot;
    if (!component || !*component)
        return NULL;
    if (strchr(component, '/') || strchr(component, '\\'))
        return pt_group_select(component, "A") ? pt_group_root_node() : NULL;
    length = strlen(component);
    if (*pt_group_current_directory() &&
        (!strcasecmp(component, "A") ||
         (length == 2u && component[0] >= 'A' && component[0] <= 'Z' &&
          component[1] >= 'A' && component[1] <= 'Z')))
        return pt_group_root_node();
    return NULL;
}

__attribute__((naked, noinline))
void hook_audio_group_select(void)
{
    __asm__ volatile(
        "push %ecx\n\tpush %edx\n\tpush %eax\n\t"
        "call hook_audio_group_select_c\n\tadd $8, %esp\n\t"
        "pop %ecx\n\tret\n\t");
}

__attribute__((noinline, used))
static int hook_resource_stream_c(const char *guest_path, const char *variant)
{
    return pt_group_select(guest_path, variant);
}

__attribute__((naked, noinline))
void hook_resource_stream(void)
{
    __asm__ volatile(
        "push %edx\n\tpush %eax\n\tcall hook_resource_stream_c\n\t"
        "add $8, %esp\n\tret\n\t");
}

typedef void *(__attribute__((regparm(3))) *directory_lookup_fn)(void *vfs,
                                                                 const char *path,
                                                                 void *node);

/* The result screen changes into the song's chart folder ("D/<id>/<id>/") through
 * the archive directory tree before loading "P/.G". phoenixtools serves files from
 * the extracted tree behind a record-less root node, so that tree walk always failed
 * and the screen skipped the song image (black background). Resolve the song
 * directory on disk, make it the current group directory and hand back the root
 * node; anything else keeps the original lookup. */
__attribute__((noinline, used))
static void *hook_song_directory_c(void *vfs, const char *guest_path, void *node)
{
    char resolved[PHOENIXTOOLS_PATH_MAX];

    if (pt_song_graphics_directory(guest_path, resolved, sizeof(resolved)) == 0 &&
        pt_group_set_current_directory(resolved) == 0)
        return pt_group_root_node();
    return ((directory_lookup_fn)PHOENIX_DIRECTORY_LOOKUP_VA)(vfs, guest_path, node);
}

__attribute__((naked, noinline))
void hook_song_directory(void)
{
    __asm__ volatile(
        "push %ecx\n\tpush %edx\n\tpush %eax\n\t"
        "call hook_song_directory_c\n\tadd $12, %esp\n\tret\n\t");
}

__attribute__((noinline, used))
static void ensure_video_slot(unsigned int slot)
{
    configure_vfs_slot(slot);
}

__attribute__((naked, noinline))
void hook_video_loader(void)
{
    __asm__ volatile(
        "push %ecx\n\tpush %edx\n\tpush %eax\n\tpush %eax\n\t"
        "call ensure_video_slot\n\tadd $4, %esp\n\tpop %eax\n\t"
        "pop %edx\n\tpop %ecx\n\tpush %ebp\n\tpush %edi\n\t"
        "push %esi\n\tpush %ebx\n\tsub $0x7c, %esp\n\t"
        "mov $0x08070203, %ebp\n\tjmp *%ebp\n\t");
}

__attribute__((naked, noinline))
void hook_playmovie_loader(void)
{
    __asm__ volatile(
        "push %ecx\n\tpush %edx\n\tpush %eax\n\tpush %eax\n\t"
        "call ensure_video_slot\n\tadd $4, %esp\n\tpop %eax\n\t"
        "pop %edx\n\tpop %ecx\n\tpush %ebp\n\tpush %edi\n\t"
        "push %esi\n\tpush %ebx\n\tsub $0x7c, %esp\n\t"
        "mov $0x0806FB73, %ebp\n\tjmp *%ebp\n\t");
}
