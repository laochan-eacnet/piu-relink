#include "resources/group_paths.h"

#include "config/config.h"
#include "core/limits.h"
#include "core/log.h"
#include "resources/path_casefold.h"
#include "resources/song_paths.h"

#include <stddef.h>
#include <string.h>
#include <strings.h>

struct phoenix_group_root {
    struct phoenix_resource_node node;
    struct phoenix_resource_record *records_end;
    struct phoenix_resource_record *records_capacity;
};

_Static_assert(offsetof(struct phoenix_group_root, records_end) == 16,
               "Phoenix group record range ABI changed");
_Static_assert(offsetof(struct phoenix_group_root, records_capacity) == 20,
               "Phoenix group record capacity ABI changed");

static struct phoenix_group_root dummy_root;
static struct phoenix_resource_node *root_node = &dummy_root.node;
static char current_directory[PHOENIXTOOLS_PATH_MAX];

static const char *language_code(void)
{
    static const char *const codes[11] = {
        "EN", "KR", "CN", "SP", "JP", "EN", "EN", "EN", "EN", "EN", "EN"
    };
    return codes[pt_config_get()->region_slot];
}

static int select_localized_payload(const char *base, const char *payload,
                                    char *selected, size_t capacity)
{
    const char *preferred = language_code();
    char localized[PHOENIXTOOLS_PATH_MAX];
    char manifest[PHOENIXTOOLS_PATH_MAX];
    int pass;

    for (pass = 0; pass < 2; ++pass) {
        const char *language = pass == 0 ? preferred : "EN";
        if (pass == 1 && !strcasecmp(preferred, "EN"))
            break;
        if (pt_path_resolve_directory(base, language, localized,
                                      sizeof(localized)) == 0 &&
            pt_path_resolve_file(localized, payload, manifest,
                                 sizeof(manifest)) == 0) {
            if (strlen(localized) + 1u > capacity)
                return -1;
            strcpy(selected, localized);
            return 0;
        }
    }
    if (pt_path_resolve_file(base, payload, manifest, sizeof(manifest)) == 0) {
        if (strlen(base) + 1u > capacity)
            return -1;
        strcpy(selected, base);
        return 0;
    }
    return -1;
}

int pt_group_select(const char *guest_path, const char *variant)
{
    const struct pt_config *config = pt_config_get();
    const char *roots[2] = {config->overlay_root, config->plain_root};
    const char *name = variant ? variant : "";
    const char *payload;
    char base[PHOENIXTOOLS_PATH_MAX];
    char media[PHOENIXTOOLS_PATH_MAX];
    char variant_directory[PHOENIXTOOLS_PATH_MAX];
    unsigned int root_index;

    if (!guest_path || !*guest_path)
        return 0;
    while (*name == '.')
        ++name;
    payload = !strcasecmp(name, "V") ? ".V2" :
              !strcasecmp(name, "A") ? ".A" : NULL;
    if (payload && ((!strcasecmp(name, "V") &&
         pt_song_resolve_video(guest_path, media, sizeof(media)) == 0) ||
        (!strcasecmp(name, "A") &&
         pt_song_resolve_audio(guest_path, media, sizeof(media)) == 0))) {
        char *slash = strrchr(media, '/');
        if (slash) {
            *slash = '\0';
            strcpy(current_directory, media);
            return 1;
        }
    }

    for (root_index = 0; root_index < 2u; ++root_index) {
        if (!*roots[root_index] || pt_path_resolve_directory(
                roots[root_index], guest_path, base, sizeof(base)) != 0)
            continue;
        if (payload && pt_path_resolve_directory(base, name, variant_directory,
                                                  sizeof(variant_directory)) == 0 &&
            select_localized_payload(variant_directory, payload,
                                     current_directory,
                                     sizeof(current_directory)) == 0)
            return 1;
        if (select_localized_payload(base, ".BGA", current_directory,
                                     sizeof(current_directory)) == 0)
            return 1;
        if (!payload) {
            strcpy(current_directory, base);
            return 1;
        }
    }
    current_directory[0] = '\0';
    if (pt_config_verbose())
        pt_log("resource", "group not found: %s", guest_path);
    return 0;
}

int pt_group_resolve_file(const char *guest_path, char *resolved,
                          size_t capacity)
{
    const char *relative = guest_path;
    const char *base;
    const char *slash;
    size_t base_length;

    if (!guest_path || !*guest_path || !current_directory[0] ||
        strchr(guest_path, '\\'))
        return -1;
    slash = strchr(guest_path, '/');
    if (slash) {
        base = strrchr(current_directory, '/');
        base = base ? base + 1u : current_directory;
        base_length = strlen(base);
        if (strncasecmp(guest_path, base, base_length) ||
            guest_path[base_length] != '/')
            /* Relative to the current directory, e.g. "P/.G" after the game
             * changed into a song directory. */
            return pt_path_resolve_file(current_directory, guest_path,
                                        resolved, capacity);
        relative = guest_path + base_length + 1u;
        if (!*relative || strchr(relative, '/'))
            return -1;
    }
    return pt_path_resolve_file(current_directory, relative, resolved, capacity);
}

const char *pt_group_current_directory(void)
{
    return current_directory;
}

int pt_group_set_current_directory(const char *directory)
{
    if (!directory || strlen(directory) >= sizeof(current_directory))
        return -1;
    strcpy(current_directory, directory);
    return 0;
}

void pt_group_set_current_from_file(const char *resolved_file)
{
    char *slash;

    if (!resolved_file || strlen(resolved_file) >= sizeof(current_directory)) {
        current_directory[0] = '\0';
        return;
    }
    strcpy(current_directory, resolved_file);
    slash = strrchr(current_directory, '/');
    if (slash)
        *slash = '\0';
}

struct phoenix_resource_node *pt_group_root_node(void)
{
    return &dummy_root.node;
}

struct phoenix_resource_node **pt_group_root_slot(void)
{
    return &root_node;
}
