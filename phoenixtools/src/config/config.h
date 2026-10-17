#pragma once

#include "core/limits.h"

#include <stdint.h>

struct pt_input_bindings {
    char upper_left[PHOENIXTOOLS_KEY_NAME_MAX];
    char upper_right[PHOENIXTOOLS_KEY_NAME_MAX];
    char center[PHOENIXTOOLS_KEY_NAME_MAX];
    char lower_left[PHOENIXTOOLS_KEY_NAME_MAX];
    char lower_right[PHOENIXTOOLS_KEY_NAME_MAX];
    char p2_upper_left[PHOENIXTOOLS_KEY_NAME_MAX];
    char p2_upper_right[PHOENIXTOOLS_KEY_NAME_MAX];
    char p2_center[PHOENIXTOOLS_KEY_NAME_MAX];
    char p2_lower_left[PHOENIXTOOLS_KEY_NAME_MAX];
    char p2_lower_right[PHOENIXTOOLS_KEY_NAME_MAX];
    char service[PHOENIXTOOLS_KEY_NAME_MAX];
    char test[PHOENIXTOOLS_KEY_NAME_MAX];
    char coin1[PHOENIXTOOLS_KEY_NAME_MAX];
    char card1[PHOENIXTOOLS_KEY_NAME_MAX];
    char card2[PHOENIXTOOLS_KEY_NAME_MAX];
};

struct pt_config {
    unsigned int region_slot;
    int hold_lock_screen;
    int verbose;
    int unlock_all_songs;
    int unlock_all_charts;
    int guest_full_mode;
    int custom_channel;
    uint32_t custom_id_min;
    uint32_t custom_id_max;
    int virtual_lxio;
    int virtual_card_reader;
    struct pt_input_bindings input;
    char plain_root[PHOENIXTOOLS_PATH_MAX];
    char overlay_root[PHOENIXTOOLS_PATH_MAX];
    char runtime_dir[PHOENIXTOOLS_PATH_MAX];
    char virtual_disk[PHOENIXTOOLS_PATH_MAX];
    char api_base[1024];
    char cdn_base[1024];
    uint16_t game_port;
    char loaded_from[PHOENIXTOOLS_PATH_MAX];
};

void pt_config_load(void);
const struct pt_config *pt_config_get(void);
const char *pt_region_name(unsigned int region_slot);
int pt_config_verbose(void);

/* Stable test-only exports loaded with dlsym by tests/config_test.c. */
int phoenixtools_parse_region_test(const char *value);
int phoenixtools_config_region_test(void);

