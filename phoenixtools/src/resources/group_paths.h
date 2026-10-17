#pragma once

#include "phoenix/resource_abi.h"

#include <stddef.h>

int pt_group_select(const char *guest_path, const char *variant);
int pt_group_resolve_file(const char *guest_path, char *resolved,
                          size_t capacity);
const char *pt_group_current_directory(void);
void pt_group_set_current_from_file(const char *resolved_file);
int pt_group_set_current_directory(const char *directory);
struct phoenix_resource_node *pt_group_root_node(void);
struct phoenix_resource_node **pt_group_root_slot(void);
