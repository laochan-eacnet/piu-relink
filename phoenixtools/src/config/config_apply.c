#include "config/config_internal.h"

#include "core/log.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

struct string_option {
    const char *name;
    size_t offset;
    size_t capacity;
};

#define INPUT_OPTION(key, field) \
    {key, offsetof(struct pt_input_bindings, field), \
     sizeof(((struct pt_input_bindings *)0)->field)}

static const struct string_option INPUT_OPTIONS[] = {
    INPUT_OPTION("upper_left", upper_left),
    INPUT_OPTION("upper_right", upper_right),
    INPUT_OPTION("center", center),
    INPUT_OPTION("lower_left", lower_left),
    INPUT_OPTION("lower_right", lower_right),
    INPUT_OPTION("p2_upper_left", p2_upper_left),
    INPUT_OPTION("p2_upper_right", p2_upper_right),
    INPUT_OPTION("p2_center", p2_center),
    INPUT_OPTION("p2_lower_left", p2_lower_left),
    INPUT_OPTION("p2_lower_right", p2_lower_right),
    INPUT_OPTION("service", service),
    INPUT_OPTION("test", test),
    INPUT_OPTION("coin1", coin1),
    INPUT_OPTION("card1", card1),
    INPUT_OPTION("card2", card2)
};

static void invalid(const char *path, unsigned int line, const char *key,
                    const char *value)
{
    pt_log("config", "%s:%u: invalid value for %s: %s", path, line, key, value);
}

static int apply_input_key(const char *key, const char *value)
{
    size_t index;
    char *base = (char *)&pt_config_state.input;

    for (index = 0; index < sizeof(INPUT_OPTIONS) / sizeof(INPUT_OPTIONS[0]); ++index) {
        const struct string_option *option = &INPUT_OPTIONS[index];
        if (!strcasecmp(key, option->name))
            return pt_config_copy_value(base + option->offset,
                                        option->capacity, value);
    }
    return 1;
}

static int copy_path(char *destination, size_t capacity,
                     const char *config_path, const char *value)
{
    char relative[PHOENIXTOOLS_PATH_MAX];
    char directory[PHOENIXTOOLS_PATH_MAX];
    char *slash;
    int written;

    if (!*value) {
        destination[0] = '\0';
        return 0;
    }
    if (pt_config_copy_value(relative, sizeof(relative), value) != 0)
        return -1;
    if (relative[0] == '/')
        return pt_config_copy_value(destination, capacity, relative);
    if (strlen(config_path) >= sizeof(directory))
        return -1;
    strcpy(directory, config_path);
    slash = strrchr(directory, '/');
    if (slash)
        *slash = '\0';
    else
        strcpy(directory, ".");
    written = snprintf(destination, capacity, "%s/%s", directory, relative);
    return written >= 0 && (size_t)written < capacity ? 0 : -1;
}

static int apply_path(const char *key, const char *value, const char *path)
{
    if (!strcasecmp(key, "plain_root"))
        return copy_path(pt_config_state.plain_root,
                         sizeof(pt_config_state.plain_root), path, value);
    if (!strcasecmp(key, "overlay_root"))
        return copy_path(pt_config_state.overlay_root,
                         sizeof(pt_config_state.overlay_root), path, value);
    if (!strcasecmp(key, "runtime_dir"))
        return copy_path(pt_config_state.runtime_dir,
                         sizeof(pt_config_state.runtime_dir), path, value);
    if (!strcasecmp(key, "virtual_disk"))
        return copy_path(pt_config_state.virtual_disk,
                         sizeof(pt_config_state.virtual_disk), path, value);
    return 1;
}

int pt_config_apply(const char *section, const char *key, const char *value,
                    const char *path, unsigned int line_number)
{
    int result = 1;

    if (!strcmp(section, "offline") && !strcasecmp(key, "region"))
        result = pt_config_parse_region(value, &pt_config_state.region_slot);
    else if (!strcmp(section, "offline") && !strcasecmp(key, "hold_lock_screen"))
        result = pt_config_parse_boolean(value, &pt_config_state.hold_lock_screen);
    else if (!strcmp(section, "offline") && !strcasecmp(key, "verbose"))
        result = pt_config_parse_boolean(value, &pt_config_state.verbose);
    else if (!strcmp(section, "gameplay") && !strcasecmp(key, "unlock_all_songs"))
        result = pt_config_parse_boolean(value, &pt_config_state.unlock_all_songs);
    else if (!strcmp(section, "gameplay") && !strcasecmp(key, "unlock_all_charts"))
        result = pt_config_parse_boolean(value, &pt_config_state.unlock_all_charts);
    else if (!strcmp(section, "gameplay") && !strcasecmp(key, "guest_full_mode"))
        result = pt_config_parse_boolean(value, &pt_config_state.guest_full_mode);
    else if (!strcmp(section, "catalog") && !strcasecmp(key, "custom_channel"))
        result = pt_config_parse_boolean(value, &pt_config_state.custom_channel);
    else if (!strcmp(section, "catalog") && !strcasecmp(key, "custom_id_min"))
        result = pt_config_parse_song_id(value, &pt_config_state.custom_id_min);
    else if (!strcmp(section, "catalog") && !strcasecmp(key, "custom_id_max"))
        result = pt_config_parse_song_id(value, &pt_config_state.custom_id_max);
    else if (!strcmp(section, "input") && !strcasecmp(key, "virtual_lxio"))
        result = pt_config_parse_boolean(value, &pt_config_state.virtual_lxio);
    else if (!strcmp(section, "input") && !strcasecmp(key, "virtual_card_reader"))
        result = pt_config_parse_boolean(value, &pt_config_state.virtual_card_reader);
    else if (!strcmp(section, "input"))
        result = apply_input_key(key, value);
    else if (!strcmp(section, "paths"))
        result = apply_path(key, value, path);
    else if (!strcmp(section, "server") && !strcasecmp(key, "api_base"))
        result = *value ? pt_config_copy_value(pt_config_state.api_base,
                                               sizeof(pt_config_state.api_base), value) : 0;
    else if (!strcmp(section, "server") && !strcasecmp(key, "cdn_base"))
        result = *value ? pt_config_copy_value(pt_config_state.cdn_base,
                                               sizeof(pt_config_state.cdn_base), value) : 0;
    else if (!strcmp(section, "server") && !strcasecmp(key, "game_port"))
        result = pt_config_parse_port(value, &pt_config_state.game_port);

    if (result < 0)
        invalid(path, line_number, key, value);
    return result;
}
