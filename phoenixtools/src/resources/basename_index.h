#pragma once

#include <stddef.h>

/* Looks up a bare file name below `root` using index slot `slot` (0: overlay, 1: plain root).
 * Returns 1 and copies the path for a unique match, 0 when absent, -1 when the name is
 * ambiguous below the root, -2 when the path does not fit or the index is unavailable. */
int pt_basename_index_lookup(unsigned int slot, const char *root, const char *name,
                             char *resolved, size_t capacity);
