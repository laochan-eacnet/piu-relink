#include "resources/resource.h"

#include "resources/group_paths.h"
#include "resources/path_casefold.h"
#include "resources/song_paths.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int phoenixtools_resolve_test(const char *guest_path, char *resolved,
                              size_t capacity)
{
    return pt_path_resolve_plain(guest_path, resolved, capacity);
}

int phoenixtools_resolve_song_video_test(const char *guest_path,
                                         char *output, size_t capacity)
{
    return pt_song_resolve_video(guest_path, output, capacity);
}

int phoenixtools_resolve_song_audio_test(const char *guest_path,
                                         char *output, size_t capacity)
{
    return pt_song_resolve_audio(guest_path, output, capacity);
}

int phoenixtools_resolve_group_file_test(const char *group,
                                         const char *guest_path,
                                         char *output, size_t capacity)
{
    return pt_group_set_current_directory(group) == 0 ?
           pt_group_resolve_file(guest_path, output, capacity) : -1;
}

int phoenixtools_select_media_test(const char *guest_path, const char *variant,
                                   char *output, size_t capacity)
{
    const char *selected;

    if (!output || !capacity || !pt_group_select(guest_path, variant))
        return -1;
    selected = pt_group_current_directory();
    if (strlen(selected) + 1u > capacity)
        return -1;
    strcpy(output, selected);
    return 0;
}

int phoenixtools_group_root_abi_test(void)
{
    struct phoenix_resource_node *node = pt_group_root_node();
    void *const *range = (void *const *)((const unsigned char *)node +
                                         sizeof(*node));

    return node && !node->records && !range[0] && !range[1] ? 0 : -1;
}

int phoenixtools_read_test(const char *guest_path, uint32_t expected_size)
{
    char resolved[4096];
    struct stat status;

    if (pt_resource_resolve_read_path(guest_path, resolved, sizeof(resolved)) != 0 ||
        stat(resolved, &status) != 0 || status.st_size < 0)
        return -1;
    if (expected_size && (uint64_t)status.st_size != expected_size)
        return -3;
    return (int)status.st_size;
}
