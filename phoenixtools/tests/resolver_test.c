#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*resolve_fn)(const char *, char *, size_t);
typedef int (*read_fn)(const char *, uint32_t);
typedef int (*parse_region_fn)(const char *);
typedef int (*config_region_fn)(void);
typedef int (*group_root_abi_fn)(void);
typedef int (*select_media_fn)(const char *, const char *, char *, size_t);
typedef int (*resolve_group_file_fn)(const char *, const char *, char *, size_t);

int main(int argc, char **argv)
{
    void *module;
    resolve_fn resolve;
    resolve_fn resolve_song_video;
    resolve_fn resolve_song_audio;
    resolve_fn resolve_native_read;
    resolve_fn resolve_song_directory;
    read_fn read_file;
    parse_region_fn parse_region;
    config_region_fn config_region;
    group_root_abi_fn group_root_abi;
    select_media_fn select_media;
    resolve_group_file_fn resolve_group_file;
    char output[4096];
    char media_output[4096];
    char group_path[4096];
    unsigned long expected = 0;
    int read_size;

    if (argc < 4 || argc > 5) {
        fprintf(stderr, "usage: %s PHOENIXTOOLS_SO PLAIN_ROOT GUEST_PATH [EXPECTED_SIZE]\n", argv[0]);
        return 2;
    }
    if (argc == 5)
        expected = strtoul(argv[4], NULL, 0);

    module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 3;
    }
    resolve = (resolve_fn)dlsym(module, "phoenixtools_resolve_test");
    resolve_song_video =
        (resolve_fn)dlsym(module, "phoenixtools_resolve_song_video_test");
    resolve_song_audio =
        (resolve_fn)dlsym(module, "phoenixtools_resolve_song_audio_test");
    resolve_native_read =
        (resolve_fn)dlsym(module, "pt_resource_resolve_read_path");
    resolve_song_directory =
        (resolve_fn)dlsym(module, "pt_song_graphics_directory");
    read_file = (read_fn)dlsym(module, "phoenixtools_read_test");
    parse_region = (parse_region_fn)dlsym(module, "phoenixtools_parse_region_test");
    config_region = (config_region_fn)dlsym(module, "phoenixtools_config_region_test");
    group_root_abi = (group_root_abi_fn)dlsym(
        module, "phoenixtools_group_root_abi_test");
    select_media = (select_media_fn)dlsym(module, "phoenixtools_select_media_test");
    resolve_group_file = (resolve_group_file_fn)dlsym(
        module, "phoenixtools_resolve_group_file_test");
    if (!resolve || !resolve_song_video || !resolve_song_audio ||
        !resolve_native_read || !resolve_song_directory || !read_file ||
        !parse_region || !config_region ||
        !group_root_abi || !select_media || !resolve_group_file) {
        fprintf(stderr, "dlsym: %s\n", dlerror());
        return 4;
    }
    if (parse_region("China") != 2 || parse_region("Latin America") != 3 ||
        parse_region("Middle_East") != 9 || parse_region("10") != 10 ||
        parse_region("invalid") != -1 || config_region() < 0 ||
        config_region() > 10 || group_root_abi() != 0) {
        fprintf(stderr, "configuration or VFS root ABI test failed\n");
        return 7;
    }
    if (select_media("video/title", ".V", media_output,
                     sizeof(media_output)) != 0) {
        fprintf(stderr, "media selector test failed\n");
        return 8;
    }
    printf("media=%s\n", media_output);
    if (select_media("user_interface/play/skin/00", "", media_output,
                     sizeof(media_output)) != 0) {
        fprintf(stderr, "bare resource group selector test failed\n");
        return 9;
    }
    if (resolve_song_video("D/E0001870/E0001870/V", media_output,
                           sizeof(media_output)) != 0 ||
        resolve_song_video("D/E0001870/E0001870", media_output,
                           sizeof(media_output)) != 0 ||
        resolve_song_video("D/E0001870/E0001870/P/V", media_output,
                           sizeof(media_output)) != 0 ||
        resolve_song_video("D/E0001870/P/V", media_output,
                           sizeof(media_output)) != 0 ||
        select_media("D/E0001870/E0001870/V", "V", media_output,
                     sizeof(media_output)) != 0 ||
        select_media("D/E0001870/E0001870", "V", media_output,
                     sizeof(media_output)) != 0) {
        fprintf(stderr, "song video layout selector test failed\n");
        return 10;
    }
    if (resolve_song_video("D/1870/1870", media_output,
                           sizeof(media_output)) != 0 ||
        strstr(media_output, "/D/1870/V/.V2") == NULL ||
        resolve_song_video("D/1870/1870/V", media_output,
                           sizeof(media_output)) != 0 ||
        strstr(media_output, "/D/1870/V/.V2") == NULL ||
        resolve_song_video("D/1870/1870/P/V", media_output,
                           sizeof(media_output)) != 0 ||
        strstr(media_output, "/D/1870/P/V/.V2") == NULL ||
        resolve_song_video("D/E0001870/E0001870/V", media_output,
                           sizeof(media_output)) != 0 ||
        strstr(media_output, "/D/1870/V/.V2") == NULL) {
        fprintf(stderr, "song full/preview video distinction failed\n");
        return 12;
    }
    printf("song-video=%s\n", media_output);
    if (resolve_song_audio("D/E0001425/E0001425/P/A", media_output,
                           sizeof(media_output)) != 0 ||
        resolve_song_audio("D/E0001425/P/A", media_output,
                           sizeof(media_output)) != 0 ||
        resolve_song_audio("D/1425/P/A", media_output,
                           sizeof(media_output)) != 0 ||
        select_media("D/E0001425/E0001425/P/A", "A", media_output,
                     sizeof(media_output)) != 0) {
        fprintf(stderr, "song audio layout selector test failed\n");
        return 11;
    }
    if (resolve_song_audio("D/1870/1870/A", media_output,
                           sizeof(media_output)) != 0 ||
        strstr(media_output, "/D/1870/A/.A") == NULL ||
        resolve_song_audio("D/1870/1870/P/A", media_output,
                           sizeof(media_output)) != 0 ||
        strstr(media_output, "/D/1870/P/A/.A") == NULL ||
        resolve_song_audio("D/E0001870/E0001870/A", media_output,
                           sizeof(media_output)) != 0 ||
        strstr(media_output, "/D/1870/A/.A") == NULL) {
        fprintf(stderr, "song full/preview audio distinction failed\n");
        return 13;
    }
    printf("song-audio=%s\n", media_output);
    if (resolve_native_read("D/1869/1869/P/.G", media_output,
                            sizeof(media_output)) != 0 ||
        strstr(media_output, "/D/1869/P/.G") == NULL) {
        fprintf(stderr, "song preview graphics layout selector failed\n");
        return 14;
    }
    printf("song-graphics=%s\n", media_output);
    if (snprintf(group_path, sizeof(group_path), "%s/D/1869/P",
                 argv[2]) < 0 ||
        resolve_group_file(group_path, "P/.g", media_output,
                           sizeof(media_output)) != 0 ||
        strstr(media_output, "/D/1869/P/.G") == NULL) {
        fprintf(stderr, "prefixed group-relative song graphics test failed\n");
        return 15;
    }
    printf("group-song-graphics=%s\n", media_output);
    /* Result screen: the game changes into the chart folder "D/<id>/<id>/" and opens
     * "P/.G" relative to it; event ids whose own folder has no image use the masked id. */
    if (resolve_song_directory("D/E0001870/E0001870/", group_path,
                               sizeof(group_path)) != 0 ||
        strlen(group_path) < 7u ||
        strcmp(group_path + strlen(group_path) - 7u, "/D/1870") != 0 ||
        resolve_group_file(group_path, "P/.g", media_output,
                           sizeof(media_output)) != 0 ||
        strstr(media_output, "/D/1870/P/.G") == NULL) {
        fprintf(stderr, "song directory relative graphics test failed\n");
        return 16;
    }
    printf("song-directory-graphics=%s\n", media_output);
    if (resolve(argv[3], output, sizeof(output)) != 0) {
        fprintf(stderr, "not found: %s\n", argv[3]);
        return 5;
    }
    read_size = read_file(argv[3], (uint32_t)expected);
    if (read_size < 0) {
        fprintf(stderr, "read test failed: %d\n", read_size);
        return 6;
    }
    printf("%s\nsize=%d\n", output, read_size);
    dlclose(module);
    return 0;
}
