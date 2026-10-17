#define _GNU_SOURCE

#include "phoenix/patch.h"
#include "phoenix/addresses.h"
#include "phoenix/patch_engine.h"
#include "core/runtime.h"
#include "config/config.h"
#include "gameplay/catalog.h"
#include "gameplay/custom_channel.h"
#include "media/media_hooks.h"
#include "input/input.h"
#include "resources/resource.h"
#include "safety/safety.h"
#include "safety/network_card.h"

#include <dlfcn.h>
#include <unistd.h>

__attribute__((constructor))
static void phoenixtools_initialize(void)
{
    static const unsigned char open_bytes[7] =
        {0x53, 0x83, 0xEC, 0x28, 0x8B, 0x48, 0x10};
    static const unsigned char read_bytes[7] =
        {0x55, 0x57, 0x56, 0x53, 0x83, 0xEC, 0x4C};
    static const unsigned char read_range_bytes[10] =
        {0x55, 0x57, 0x56, 0x53, 0x81, 0xEC, 0x4C, 0x10, 0x00, 0x00};
    static const unsigned char scan_bytes[6] =
        {0x55, 0x89, 0xE5, 0x57, 0x56, 0x53};
    static const unsigned char video_loader_bytes[7] =
        {0x55, 0x57, 0x56, 0x53, 0x83, 0xEC, 0x7C};
    static const unsigned char command_bytes[7] =
        {0x55, 0x57, 0x56, 0x53, 0x83, 0xEC, 0x3C};
    static const unsigned char glx_plt[6] =
        {0xFF, 0x25, 0xFC, 0x63, 0x21, 0x08};
    static const unsigned char swap_call[5] =
        {0xE8, 0xBE, 0x84, 0xFE, 0xFF};
    static const unsigned char skip_swap[5] =
        {0x31, 0xC0, 0x90, 0x90, 0x90};
    static const unsigned char check_branch[2] = {0x75, 0x6F};
    static const unsigned char force_check[2] = {0xEB, 0x6F};
    static const unsigned char update_state[10] =
        {0xC7, 0x05, 0xC0, 0x1A, 0x79, 0x09, 0, 0, 0, 0};
    static const unsigned char update[10] =
        {0x55, 0x57, 0x56, 0x53, 0x81, 0xEC, 0x5C, 0x40, 0, 0};
    static const unsigned char disk_id[7] =
        {0x53, 0x81, 0xEC, 0x18, 0x02, 0, 0};
    static const unsigned char lock_response[6] =
        {0x55, 0x89, 0xE5, 0x53, 0x57, 0x56};
    static const unsigned char port_probe[6] =
        {0x57, 0x56, 0x53, 0x83, 0xEC, 0x20};
    static const unsigned char mixer[7] =
        {0x53, 0x81, 0xEC, 0x88, 0, 0, 0};
    static const unsigned char resource_next[5] =
        {0x83, 0xEC, 0x1C, 0x89, 0xC2};
    static const unsigned char resource_stream[6] =
        {0x53, 0x83, 0xEC, 0x18, 0x89, 0xD3};
    static const unsigned char media_stream[6] =
        {0x55, 0x57, 0x56, 0x53, 0x83, 0xEC};
    static const unsigned char audio_calls[10][5] = {
        {0xE8,0xC9,0xB0,0xFE,0xFF}, {0xE8,0x9A,0xB0,0xFE,0xFF},
        {0xE8,0x70,0xB0,0xFE,0xFF}, {0xE8,0x4E,0xB0,0xFE,0xFF},
        {0xE8,0x63,0xAF,0xFE,0xFF}, {0xE8,0x7B,0xE8,0xF6,0xFF},
        {0xE8,0x4C,0xE8,0xF6,0xFF}, {0xE8,0x22,0xE8,0xF6,0xFF},
        {0xE8,0x00,0xE8,0xF6,0xFF}, {0xE8,0x15,0xE7,0xF6,0xFF}
    };
    static void *const audio_sites[10] = {
        (void *)0x08070BCCu, (void *)0x08070BFBu, (void *)0x08070C25u,
        (void *)0x08070C47u, (void *)0x08070D32u, (void *)0x080ED41Au,
        (void *)0x080ED449u, (void *)0x080ED473u, (void *)0x080ED495u,
        (void *)0x080ED580u
    };
    static const unsigned char market_call[5] =
        {0xE8, 0xBB, 0xC7, 0x08, 0};
    static const unsigned char basic_mode = 0;
    static const unsigned char full_mode = 1;
    static const unsigned char key_press[7] =
        {0xC6,0x80,0xE0,0xEE,0x1B,0x0A,0x01};
    static const unsigned char key_release[7] =
        {0xC6,0x80,0xE0,0xEE,0x1B,0x0A,0x00};
    static const unsigned char key_noop[7] =
        {0x90,0x90,0x90,0x90,0x90,0x90,0x90};
    static const unsigned char video_call_1[5] =
        {0xE8, 0xC7, 0x04, 0x02, 0};
    static const unsigned char video_call_2[5] =
        {0xE8, 0x88, 0x04, 0x02, 0};
    static const unsigned char song_directory_call[5] =
        {0xE8, 0x6E, 0x45, 0xF8, 0xFF};
    static const unsigned char packet_send_transform[5] =
        {0xE8, 0xA1, 0xDE, 0x09, 0x00};
    static const unsigned char packet_recv_transform[5] =
        {0xE8, 0x78, 0x94, 0x09, 0x00};
    static const unsigned char packet_no_transform[5] =
        {0x90, 0x90, 0x90, 0x90, 0x90};
    static const unsigned char channel_case_12[9] =
        {0x83, 0xF8, 0x01, 0x0F, 0x8E, 0x0A, 0xF4, 0xFF, 0xFF};
    static const unsigned char channel_native_key[18] = "CHANNEL_NX_TO_NXA";
    static const unsigned char channel_custom_key[18] = "CHANNEL_CUSTOM";
    const struct pt_config *config;
    unsigned int index;
    int failed = 0;

    pt_real_fopen = dlsym(RTLD_NEXT, "fopen");
    pt_real_open = dlsym(RTLD_NEXT, "open");
    pt_real_mkdir = dlsym(RTLD_NEXT, "mkdir");
    pt_real_curl_easy_setopt = dlsym(RTLD_NEXT, "curl_easy_setopt");
    pt_config_load();
    pt_sandbox_initialize();
    config = pt_config_get();
    if (!pt_patch_target_mapped((uintptr_t)PHOENIX_OPEN_VA, sizeof(open_bytes)))
        return;
    pt_input_initialize();
    failed |= pt_network_card_initialize();
    if (config->custom_channel) {
        failed |= pt_patch_jump(PHOENIX_CHANNEL_CASE_12_VA, hook_custom_channel,
                                channel_case_12, sizeof(channel_case_12));
        failed |= pt_patch_bytes(PHOENIX_CHANNEL_CUSTOM_KEY_VA, channel_native_key,
                                 channel_custom_key, sizeof(channel_native_key));
    }
    if (config->guest_full_mode)
        failed |= pt_patch_bytes((void *)0x08098CB3u, &basic_mode,
                                 &full_mode, sizeof(basic_mode));
    if (config->virtual_lxio) {
        failed |= pt_patch_bytes((void *)0x0805871Cu, key_press, key_noop,
                                 sizeof(key_press));
        failed |= pt_patch_bytes((void *)0x08058792u, key_release, key_noop,
                                 sizeof(key_release));
    }
    if (pt_network_private_base(config->api_base)) {
        /*
         * Only the network trailer calls, not the shared SenseLock functions.
         * Their results are unused: each is followed by mutex_unlock, which
         * clobbers EAX/flags before any subsequent use. Stack arguments occupy
         * the caller's reserved frame, so a five-byte NOP needs no ABI shim.
         * RC4, trailer magic/digest validation and frame bounds stay intact.
         */
        failed |= pt_patch_bytes((void *)0x0808382Au, packet_send_transform,
                                 packet_no_transform, sizeof(packet_send_transform));
        failed |= pt_patch_bytes((void *)0x08088BA3u, packet_recv_transform,
                                 packet_no_transform, sizeof(packet_recv_transform));
    }
    failed |= pt_patch_jump(PHOENIX_OPEN_VA, hook_open_entry, open_bytes, sizeof(open_bytes));
    failed |= pt_patch_jump(PHOENIX_READ_VA, hook_read_entry, read_bytes, sizeof(read_bytes));
    failed |= pt_patch_jump(PHOENIX_READ_RANGE_VA, hook_read_range_entry, read_range_bytes, sizeof(read_range_bytes));
    failed |= pt_patch_jump(PHOENIX_SCAN_VA, hook_scan_entry, scan_bytes, sizeof(scan_bytes));
    failed |= pt_patch_jump(PHOENIX_VIDEO_LOADER_VA, hook_video_loader, video_loader_bytes, sizeof(video_loader_bytes));
    failed |= pt_patch_jump(PHOENIX_PLAYMOVIE_LOADER_VA, hook_playmovie_loader, video_loader_bytes, sizeof(video_loader_bytes));
    failed |= pt_patch_jump(PHOENIX_COMMAND_VA, hook_command_entry, command_bytes, sizeof(command_bytes));
    failed |= pt_patch_jump(PHOENIX_GLX_SWAP_BUFFERS_PLT, hook_glx_swap_buffers, glx_plt, sizeof(glx_plt));
    failed |= pt_patch_bytes(PHOENIX_SYSTEMCHECK_DONE_BRANCH_VA, check_branch, force_check, sizeof(check_branch));
    failed |= pt_patch_jump(PHOENIX_UPDATE_STATE_VA, hook_update_state, update_state, sizeof(update_state));
    failed |= pt_patch_jump(PHOENIX_UPDATE_VA, hook_update_entry, update, sizeof(update));
    failed |= pt_patch_jump(PHOENIX_DISK_ID_VA, hook_disk_id_entry, disk_id, sizeof(disk_id));
    failed |= pt_patch_jump(PHOENIX_LOCK_RESPONSE_VA, hook_lock_response, lock_response, sizeof(lock_response));
    failed |= pt_patch_jump(PHOENIX_PORT_IO_PROBE_VA, hook_port_io_probe, port_probe, sizeof(port_probe));
    failed |= pt_patch_jump(PHOENIX_MIXER_HELPER_VA, hook_mixer_helper, mixer, sizeof(mixer));
    failed |= pt_patch_jump(PHOENIX_MIXER_HELPER_2_VA, hook_mixer_helper, mixer, sizeof(mixer));
    failed |= pt_patch_jump(PHOENIX_RESOURCE_NEXT_VA, hook_resource_next, resource_next, sizeof(resource_next));
    failed |= pt_patch_jump(PHOENIX_RESOURCE_STREAM_VA, hook_resource_stream, resource_stream, sizeof(resource_stream));
    failed |= pt_patch_jump(PHOENIX_MEDIA_STREAM_VA, hook_media_stream, media_stream, sizeof(media_stream));
    for (index = 0; index < 10u; ++index)
        failed |= pt_patch_call(audio_sites[index], hook_audio_group_select, audio_calls[index]);
    failed |= pt_patch_call(PHOENIX_MARKET_FINALIZE_CALL_VA, hook_gameplay_finalize, market_call);
    failed |= pt_patch_call(PHOENIX_GRADE_SONG_DIRECTORY_CALL_VA, hook_song_directory, song_directory_call);
    failed |= pt_patch_call((void *)0x080DF19Au, hook_game_video_exists, video_call_1);
    failed |= pt_patch_call((void *)0x080DF1D9u, hook_game_video_exists, video_call_2);
    failed |= pt_patch_bytes(PHOENIX_SWAP_INTERVAL_CALL_VA, swap_call, skip_swap, sizeof(swap_call));
    if (failed) {
        pt_log("patch", "text patching failed; refusing partial startup");
        _exit(126);
    }
    pt_patches_installed = 1;
    pt_log("ready", "hooks installed");
}

int phoenixtools_is_patched(void)
{
    return pt_patches_installed;
}
