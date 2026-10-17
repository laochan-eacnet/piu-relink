#include "phoenix/patch_engine.h"

#include "core/log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int pt_patch_target_mapped(uintptr_t address, size_t length)
{
    FILE *maps;
    char line[512];
    unsigned long start;
    unsigned long end;
    char permissions[5];

    maps = fopen("/proc/self/maps", "r");
    if (!maps)
        return 0;
    while (fgets(line, sizeof(line), maps)) {
        if (sscanf(line, "%lx-%lx %4s", &start, &end, permissions) == 3 &&
            address >= start && address + length <= end &&
            strchr(permissions, 'x')) {
            fclose(maps);
            return 1;
        }
    }
    fclose(maps);
    return 0;
}

static int protect_range(void *target, size_t length, int protection)
{
    long page_size = sysconf(_SC_PAGESIZE);
    uintptr_t page;
    uintptr_t end;

    if (page_size <= 0)
        page_size = 4096;
    page = (uintptr_t)target & ~((uintptr_t)page_size - 1u);
    end = ((uintptr_t)target + length + (uintptr_t)page_size - 1u) &
          ~((uintptr_t)page_size - 1u);
    if (mprotect((void *)page, end - page, protection) != 0) {
        pt_log("patch", "mprotect failed for %p: %s", target, strerror(errno));
        return -1;
    }
    return 0;
}

static int write_patch(void *target, const unsigned char *bytes, size_t length)
{
    if (protect_range(target, length, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return -1;
    memcpy(target, bytes, length);
    __builtin___clear_cache((char *)target, (char *)target + length);
    return protect_range(target, length, PROT_READ | PROT_EXEC);
}

static int validate(void *target, const unsigned char *expected, size_t length)
{
    if (!pt_patch_target_mapped((uintptr_t)target, length) ||
        memcmp(target, expected, length) != 0) {
        pt_log("patch", "unsupported bytes at %p", target);
        return -1;
    }
    return 0;
}

int pt_patch_jump(void *target, void *replacement,
                  const unsigned char *expected, size_t stolen)
{
    unsigned char bytes[16];
    int32_t relative;

    if (stolen < 5u || stolen > sizeof(bytes) ||
        validate(target, expected, stolen) != 0)
        return -1;
    memset(bytes, 0x90, stolen);
    bytes[0] = 0xe9;
    relative = (int32_t)((uintptr_t)replacement - ((uintptr_t)target + 5u));
    memcpy(bytes + 1, &relative, sizeof(relative));
    return write_patch(target, bytes, stolen);
}

int pt_patch_call(void *target, void *replacement,
                  const unsigned char expected[5])
{
    unsigned char bytes[5];
    int32_t relative;

    if (validate(target, expected, sizeof(bytes)) != 0)
        return -1;
    bytes[0] = 0xe8;
    relative = (int32_t)((uintptr_t)replacement - ((uintptr_t)target + 5u));
    memcpy(bytes + 1, &relative, sizeof(relative));
    return write_patch(target, bytes, sizeof(bytes));
}

int pt_patch_bytes(void *target, const unsigned char *expected,
                   const unsigned char *replacement, size_t length)
{
    return validate(target, expected, length) == 0 ?
           write_patch(target, replacement, length) : -1;
}
