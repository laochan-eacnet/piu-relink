#pragma once

#include <stddef.h>

int pt_path_resolve_file(const char *root, const char *guest_path,
                         char *resolved, size_t capacity);
int pt_path_resolve_directory(const char *root, const char *guest_path,
                              char *resolved, size_t capacity);
int pt_path_resolve_plain(const char *guest_path, char *resolved,
                          size_t capacity);
int pt_path_resolve_unique_basename(const char *guest_path, char *resolved,
                                    size_t capacity);
