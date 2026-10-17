#include "resources/song_paths.h"

#include "config/config.h"
#include "core/limits.h"
#include "core/log.h"
#include "resources/path_casefold.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int resolve_candidate(const char *candidate, char *resolved,
                             size_t capacity)
{
    const struct pt_config *config = pt_config_get();
    const char *roots[2] = {config->overlay_root, config->plain_root};
    unsigned int index;

    for (index = 0; index < 2u; ++index) {
        if (*roots[index] && pt_path_resolve_file(
                roots[index], candidate, resolved, capacity) == 0)
            return 0;
    }
    return -1;
}

static void remove_duplicate_song_component(char *song)
{
    char component[PHOENIXTOOLS_PATH_MAX];
    char *last = strrchr(song, '/');

    if (last && strlen(last + 1u) < sizeof(component)) {
        char *previous;
        strcpy(component, last + 1u);
        *last = '\0';
        previous = strrchr(song, '/');
        if (!previous || strcasecmp(previous + 1u, component))
            *last = '/';
    }
}

static int mask_encoded_song(char *song)
{
    char *last = strrchr(song, '/');
    char *end = NULL;
    unsigned long encoded;
    unsigned long base;
    int written;

    if (!last || !last[1])
        return -1;
    encoded = strtoul(last + 1u, &end, 16);
    base = encoded & 0x0ffffffful;
    if (!end || *end || !base || base == encoded)
        return -1;
    written = snprintf(last + 1u,
                       PHOENIXTOOLS_PATH_MAX - (size_t)(last + 1u - song),
                       "%lX", base);
    return written >= 0 ? 0 : -1;
}

static int resolve_media(const char *guest_path, const char *component,
                         int preview_default, char *resolved, size_t capacity)
{
    char song[PHOENIXTOOLS_PATH_MAX];
    char candidate[PHOENIXTOOLS_PATH_MAX];
    char masked[PHOENIXTOOLS_PATH_MAX];
    const char *suffix;
    char *last;
    size_t length;
    int explicit_component = 0;
    int preview = preview_default;
    int written;

    if (!guest_path || !*guest_path || strlen(guest_path) >= sizeof(song))
        return -1;
    strcpy(song, guest_path);
    length = strlen(song);
    while (length && song[length - 1u] == '/')
        song[--length] = '\0';
    last = strrchr(song, '/');
    if (last && !strcasecmp(last + 1u, component)) {
        explicit_component = 1;
        *last = '\0';
    }
    last = strrchr(song, '/');
    if (last && !strcasecmp(last + 1u, "P")) {
        preview = 1;
        *last = '\0';
    } else if (explicit_component) {
        preview = 0;
    }
    remove_duplicate_song_component(song);
    suffix = !strcasecmp(component, "V") ?
             (preview ? "P/V/.V2" : "V/.V2") :
             (preview ? "P/A/.A" : "A/.A");
    written = snprintf(candidate, sizeof(candidate), "%s/%s", song, suffix);
    if (written < 0 || (size_t)written >= sizeof(candidate))
        return -1;
    if (resolve_candidate(candidate, resolved, capacity) == 0)
        return 0;

    strcpy(masked, song);
    if (mask_encoded_song(masked) != 0)
        return -1;
    written = snprintf(candidate, sizeof(candidate), "%s/%s", masked, suffix);
    return written >= 0 && (size_t)written < sizeof(candidate) ?
           resolve_candidate(candidate, resolved, capacity) : -1;
}

int pt_song_resolve_video(const char *guest_path, char *resolved,
                          size_t capacity)
{
    return resolve_media(guest_path, "V", 0, resolved, capacity);
}

int pt_song_resolve_audio(const char *guest_path, char *resolved,
                          size_t capacity)
{
    return resolve_media(guest_path, "A", 1, resolved, capacity);
}

static int resolve_preview_graphics(const char *guest_path, char *resolved,
                                    size_t capacity)
{
    char database[8];
    char song_id[64];
    char repeated_id[64];
    char preview[8];
    char payload[16];
    char extra;
    char candidate[PHOENIXTOOLS_PATH_MAX];
    char *end = NULL;
    unsigned long encoded;
    unsigned long base;
    int written;

    if (!guest_path || sscanf(guest_path,
        "%7[^/]/%63[^/]/%63[^/]/%7[^/]/%15[^/]%c", database, song_id,
        repeated_id, preview, payload, &extra) != 5 ||
        strcasecmp(database, "D") || strcasecmp(song_id, repeated_id) ||
        strcasecmp(preview, "P") || strcasecmp(payload, ".G"))
        return -1;
    written = snprintf(candidate, sizeof(candidate), "D/%s/P/.G", song_id);
    if (written >= 0 && (size_t)written < sizeof(candidate) &&
        resolve_candidate(candidate, resolved, capacity) == 0)
        return 0;
    encoded = strtoul(song_id, &end, 16);
    base = encoded & 0x0ffffffful;
    if (!end || *end || !base || base == encoded)
        return -1;
    written = snprintf(candidate, sizeof(candidate), "D/%lX/P/.G", base);
    return written >= 0 && (size_t)written < sizeof(candidate) ?
           resolve_candidate(candidate, resolved, capacity) : -1;
}

int pt_resource_resolve_read_path(const char *guest_path, char *resolved,
                                  size_t capacity)
{
    if (!guest_path || !*guest_path || guest_path[0] == '/' ||
        strchr(guest_path, '\\'))
        return -1;
    if (pt_path_resolve_plain(guest_path, resolved, capacity) == 0 ||
        resolve_preview_graphics(guest_path, resolved, capacity) == 0 ||
        pt_song_resolve_audio(guest_path, resolved, capacity) == 0)
        return 0;
    return pt_song_resolve_video(guest_path, resolved, capacity);
}

/* Song root directory holding the song's preview image, for a chart folder the game
 * names "D/<id>/<id>/": resolved exactly like the game's own "P/.G" existence check
 * (masked event ids included), so a following relative "P/.G" open finds the image. */
int pt_song_graphics_directory(const char *chart_directory, char *resolved,
                               size_t capacity)
{
    char graphics[PHOENIXTOOLS_PATH_MAX];
    size_t length = chart_directory ? strlen(chart_directory) : 0u;
    char *slash;
    int written;

    while (length && chart_directory[length - 1u] == '/')
        --length;
    if (!length)
        return -1;
    written = snprintf(graphics, sizeof(graphics), "%.*s/P/.G", (int)length,
                       chart_directory);
    if (written < 0 || (size_t)written >= sizeof(graphics) ||
        pt_resource_resolve_read_path(graphics, resolved, capacity) != 0)
        return -1;
    slash = strrchr(resolved, '/');            /* .../D/<id>/P/.G -> .../D/<id>/P */
    if (slash)
        *slash = '\0';
    slash = strrchr(resolved, '/');            /* -> .../D/<id> */
    if (!slash)
        return -1;
    *slash = '\0';
    return 0;
}
