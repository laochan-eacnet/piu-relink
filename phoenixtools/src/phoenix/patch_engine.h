#pragma once

#include <stddef.h>
#include <stdint.h>

int pt_patch_target_mapped(uintptr_t address, size_t length);
int pt_patch_jump(void *target, void *replacement,
                  const unsigned char *expected, size_t stolen);
int pt_patch_call(void *target, void *replacement,
                  const unsigned char expected[5]);
int pt_patch_bytes(void *target, const unsigned char *expected,
                   const unsigned char *replacement, size_t length);
