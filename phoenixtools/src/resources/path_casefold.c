#define _GNU_SOURCE

#include "resources/path_casefold.h"

#include "config/config.h"
#include "resources/basename_index.h"
#include "core/limits.h"
#include "core/log.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static int append_component(char *path, size_t capacity, const char *component)
{
    size_t used = strlen(path);
    size_t length = strlen(component);
    int slash = used != 0u && path[used - 1u] != '/';

    if (used + (size_t)slash + length + 1u > capacity)
        return -1;
    if (slash)
        path[used++] = '/';
    memcpy(path + used, component, length + 1u);
    return 0;
}

static int resolve_existing(const char *root, const char *guest_path,
                            char *resolved, size_t capacity, int regular)
{
    char normalized[PHOENIXTOOLS_PATH_MAX];
    char current[PHOENIXTOOLS_PATH_MAX];
    char candidate[PHOENIXTOOLS_PATH_MAX];
    char *save = NULL;
    char *component;
    size_t input;
    size_t output = 0;
    struct stat status;

    if (!root || !*root || !guest_path || !*guest_path ||
        strlen(root) >= sizeof(current) ||
        strlen(guest_path) >= sizeof(normalized))
        return -1;
    for (input = 0; guest_path[input]; ++input) {
        char character = guest_path[input] == '\\' ? '/' : guest_path[input];
        if (!output && character == '/')
            continue;
        if (output + 1u >= sizeof(normalized))
            return -1;
        normalized[output++] = character;
    }
    normalized[output] = '\0';
    strcpy(current, root);
    while (strlen(current) > 1u && current[strlen(current) - 1u] == '/')
        current[strlen(current) - 1u] = '\0';

    component = strtok_r(normalized, "/", &save);
    while (component) {
        DIR *directory;
        struct dirent64 *entry;
        const char *selected = component;
        char matched[NAME_MAX + 1];

        if (!strcmp(component, ".")) {
            component = strtok_r(NULL, "/", &save);
            continue;
        }
        if (!strcmp(component, "..") || strchr(component, '\n') ||
            strchr(component, '\r'))
            return -1;
        if (snprintf(candidate, sizeof(candidate), "%s", current) < 0 ||
            append_component(candidate, sizeof(candidate), component) != 0)
            return -1;
        if (stat(candidate, &status) != 0) {
            matched[0] = '\0';
            directory = opendir(current);
            if (!directory)
                return -1;
            while ((entry = readdir64(directory)) != NULL) {
                if (!strcasecmp(entry->d_name, component)) {
                    strncpy(matched, entry->d_name, sizeof(matched) - 1u);
                    matched[sizeof(matched) - 1u] = '\0';
                    break;
                }
            }
            closedir(directory);
            if (!matched[0])
                return -1;
            selected = matched;
        }
        if (append_component(current, sizeof(current), selected) != 0)
            return -1;
        component = strtok_r(NULL, "/", &save);
    }
    if (stat(current, &status) != 0 ||
        (regular ? !S_ISREG(status.st_mode) : !S_ISDIR(status.st_mode)) ||
        strlen(current) + 1u > capacity)
        return -1;
    strcpy(resolved, current);
    return 0;
}

int pt_path_resolve_file(const char *root, const char *guest_path,
                         char *resolved, size_t capacity)
{
    return resolve_existing(root, guest_path, resolved, capacity, 1);
}

int pt_path_resolve_directory(const char *root, const char *guest_path,
                              char *resolved, size_t capacity)
{
    return resolve_existing(root, guest_path, resolved, capacity, 0);
}

int pt_path_resolve_unique_basename(const char *guest_path, char *resolved,
                                    size_t capacity)
{
    const struct pt_config *config = pt_config_get();
    const char *roots[2] = {config->overlay_root, config->plain_root};
    unsigned int index;

    if (!guest_path || !*guest_path || strchr(guest_path, '/') ||
        strchr(guest_path, '\\'))
        return -1;
    for (index = 0; index < 2u; ++index) {
        int found;
        if (!*roots[index])
            continue;
        found = pt_basename_index_lookup(index, roots[index], guest_path,
                                         resolved, capacity);
        if (found == 1)
            return 0;
        if (found == -1 && pt_config_verbose())
            pt_log("resource", "ambiguous basename %s below %s",
                   guest_path, roots[index]);
    }
    return -1;
}

int pt_path_resolve_plain(const char *guest_path, char *resolved,
                          size_t capacity)
{
    const struct pt_config *config = pt_config_get();

    if (*config->overlay_root && pt_path_resolve_file(
            config->overlay_root, guest_path, resolved, capacity) == 0)
        return 0;
    return pt_path_resolve_file(config->plain_root, guest_path, resolved, capacity);
}
