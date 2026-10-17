#define _GNU_SOURCE

#include "media/media_hooks.h"
#include "resources/resource.h"
#include "resources/group_paths.h"
#include "resources/song_paths.h"
#include "config/config.h"
#include "core/log.h"
#include "input/input.h"
#include "gameplay/catalog.h"

#include <stdint.h>
#include <string.h>

__attribute__((noinline, used))
static int hook_media_stream_c(unsigned int slot, const char *guest_path,
                               const char *variant)
{
    if (pt_config_verbose()) {
        static unsigned int calls;
        if (calls++ < 32 || (guest_path &&
            (guest_path[0] == 'D' || guest_path[0] == 'd')))
            pt_log("media-request", "slot=%u path=%s variant=%s", slot,
                     guest_path ? guest_path : "(null)",
                     variant ? variant : "(null)");
    }
    return pt_group_select(guest_path, variant);
}

__attribute__((naked, noinline))
void hook_media_stream(void)
{
    __asm__ volatile(
        "push %ecx\n\t"
        "push %edx\n\t"
        "push %eax\n\t"
        "call hook_media_stream_c\n\t"
        "add $12, %esp\n\t"
        "ret\n\t");
}

__attribute__((noinline, used))
static const char *hook_command_c(const char *command)
{
    if (command && !strcmp(command, "TITLE"))
        pt_input_install_phoenix_bindings();
    if (command && (!strcmp(command, "SELECT_SYNC") ||
                    !strcmp(command, "SELECT")))
        pt_gameplay_log_catalog_state(command);
    if (pt_config_verbose()) {
        pt_log("command", "%s", command ? command : "(null)");
        if (command && !strncmp(command, "DEMO", 4))
            pt_log("bga-gate", "catalog=%u fallback=%u inhibit=%u/%u mode=%u",
                     *(volatile uint32_t *)0x0979E7ACu,
                     *(volatile unsigned char *)0x0979E7BCu,
                     *(volatile unsigned char *)0x0A1ADB6Cu,
                     *(volatile unsigned char *)0x0A1ADB6Du,
                     *(volatile unsigned char *)0x0962B93Eu);
    }
    return command;
}

__attribute__((noinline, used))
static int hook_game_video_exists_c(unsigned int slot, const char *guest_path)
{
    char resolved[4096];
    int exists = slot == 1u && guest_path &&
                 pt_resource_resolve_read_path(guest_path, resolved,
                                                  sizeof(resolved)) == 0;
    if (pt_config_verbose())
        pt_log("game-video-gate", "slot=%u path=%s exists=%d", slot,
                 guest_path ? guest_path : "(null)", exists);
    return exists;
}

__attribute__((naked, noinline))
void hook_game_video_exists(void)
{
    __asm__ volatile(
        "push %edx\n\t"
        "push %eax\n\t"
        "call hook_game_video_exists_c\n\t"
        "add $8, %esp\n\t"
        "ret\n\t");
}

__attribute__((naked, noinline))
void hook_command_entry(void)
{
    __asm__ volatile(
        "push %eax\n\t"
        "call hook_command_c\n\t"
        "add $4, %esp\n\t"
        "push %ebp\n\t"
        "push %edi\n\t"
        "push %esi\n\t"
        "push %ebx\n\t"
        "sub $0x3c, %esp\n\t"
        "mov $0x08060F9B, %ebp\n\t"
        "jmp *%ebp\n\t");
}
