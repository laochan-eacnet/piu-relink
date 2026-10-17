#pragma once

#include <stddef.h>

int pt_song_resolve_video(const char *guest_path, char *resolved,
                          size_t capacity);
int pt_song_resolve_audio(const char *guest_path, char *resolved,
                          size_t capacity);
int pt_resource_resolve_read_path(const char *guest_path, char *resolved,
                                  size_t capacity);
int pt_song_graphics_directory(const char *chart_directory, char *resolved,
                               size_t capacity);
