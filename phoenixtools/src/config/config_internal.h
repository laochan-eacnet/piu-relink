#pragma once

#include "config/config.h"

#include <stddef.h>

#define PT_CUSTOM_ID_MIN_DEFAULT 0x7F00u
#define PT_CUSTOM_ID_MAX_DEFAULT 0x7FFFu

extern struct pt_config pt_config_state;

void pt_config_set_defaults(void);
void pt_config_load_path(const char *path);
void pt_config_normalize_name(const char *input, char *output, size_t capacity);
int pt_config_parse_region(const char *value, unsigned int *region);
int pt_config_parse_boolean(const char *value, int *result);
int pt_config_parse_song_id(const char *value, uint32_t *result);
int pt_config_parse_port(const char *value, uint16_t *result);
int pt_config_copy_value(char *destination, size_t capacity, const char *value);
int pt_config_apply(const char *section, const char *key, const char *value,
                    const char *path, unsigned int line_number);

