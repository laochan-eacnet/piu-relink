#pragma once

#include <stddef.h>
#include <stdint.h>

void hook_open_entry(void);
void hook_read_entry(void);
void hook_read_range_entry(void);
void hook_scan_entry(void);
void hook_resource_next(void);
void hook_resource_stream(void);
void hook_audio_group_select(void);
void hook_video_loader(void);
void hook_playmovie_loader(void);
void hook_song_directory(void);

int phoenixtools_resolve_test(const char *guest_path, char *resolved,
                              size_t capacity);
int phoenixtools_resolve_song_video_test(const char *guest_path, char *output,
                                         size_t capacity);
int phoenixtools_resolve_song_audio_test(const char *guest_path, char *output,
                                         size_t capacity);
int phoenixtools_resolve_group_file_test(const char *group,
                                         const char *guest_path,
                                         char *output, size_t capacity);
int phoenixtools_select_media_test(const char *guest_path, const char *variant,
                                   char *output, size_t capacity);
int phoenixtools_read_test(const char *guest_path, uint32_t expected_size);
