#include "resources/resource.h"

#include "core/runtime.h"
#include "resources/group_paths.h"
#include "resources/path_casefold.h"
#include "resources/song_paths.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static int media_file(const char *path)
{
    size_t length = path ? strlen(path) : 0u;
    return (length >= 2u && !strcasecmp(path + length - 2u, ".A")) ||
           (length >= 2u && !strcasecmp(path + length - 2u, ".G")) ||
           (length >= 3u && !strcasecmp(path + length - 3u, ".V2"));
}

static int resolve_open_path(const char *guest_path, char *resolved,
                             size_t capacity)
{
    const char *relative = guest_path;

    if (guest_path && !strcasecmp(guest_path, ".V"))
        relative = ".V2";
    if (pt_group_resolve_file(relative, resolved, capacity) == 0 ||
        pt_path_resolve_plain(guest_path, resolved, capacity) == 0 ||
        pt_path_resolve_unique_basename(guest_path, resolved, capacity) == 0)
        return 0;
    if (pt_resource_resolve_read_path(guest_path, resolved, capacity) == 0) {
        if (media_file(resolved))
            pt_group_set_current_from_file(resolved);
        return 0;
    }
    return -1;
}

__attribute__((noinline, used))
static void *hook_open_c(void *vfs, const char *guest_path, const void *caller,
                         const void *parent_caller)
{
    char resolved[PHOENIXTOOLS_PATH_MAX];
    struct stat status;
    struct phoenix_resource_block *block;
    struct phoenix_resource_handle *handle;
    size_t path_length;

    (void)vfs;
    (void)caller;
    (void)parent_caller;
    if (resolve_open_path(guest_path, resolved, sizeof(resolved)) != 0 ||
        stat(resolved, &status) != 0 || status.st_size < 0 ||
        (uint64_t)status.st_size > UINT32_MAX)
        return NULL;

    path_length = strlen(resolved);
    block = malloc(sizeof(*block) + path_length + 1u);
    handle = malloc(sizeof(*handle));
    if (!block || !handle) {
        free(block);
        free(handle);
        return NULL;
    }
    memset(block, 0, sizeof(*block));
    block->node.records = &block->record;
    block->record.word[1] = (uint32_t)status.st_size;
    block->record.word[2] = (uint32_t)status.st_size;
    memcpy(block->path, resolved, path_length + 1u);
    handle->node = &block->node;
    handle->index = 0;
    handle->key_and_owner = block;
    return handle;
}

__attribute__((noinline, used))
static void *hook_read_c(struct phoenix_resource_node *node, uint32_t index,
                         void *output, const void *ignored_key)
{
    struct phoenix_resource_block *block = (struct phoenix_resource_block *)node;
    unsigned char *buffer = output;
    uint32_t size;
    FILE *file;
    size_t received;

    (void)ignored_key;
    if (!node || index != 0u || node->records != &block->record)
        return NULL;
    size = block->record.word[1];
    if (!buffer && !(buffer = malloc(size ? size : 1u)))
        return NULL;
    file = pt_real_fopen ? pt_real_fopen(block->path, "rb") : NULL;
    if (!file) {
        if (!output)
            free(buffer);
        return NULL;
    }
    received = size ? fread(buffer, 1, size, file) : 0u;
    fclose(file);
    if (received != size) {
        if (!output)
            free(buffer);
        return NULL;
    }
    return buffer;
}

__attribute__((naked, noinline))
void hook_open_entry(void)
{
    __asm__ volatile(
        "mov %esp, %ecx\n\tpush 80(%ecx)\n\tpush (%ecx)\n\t"
        "push %edx\n\tpush %eax\n\tcall hook_open_c\n\t"
        "add $16, %esp\n\tret\n\t");
}

__attribute__((naked, noinline))
void hook_read_entry(void)
{
    __asm__ volatile(
        "push 4(%esp)\n\tpush %ecx\n\tpush %edx\n\tpush %eax\n\t"
        "call hook_read_c\n\tadd $16, %esp\n\tret\n\t");
}

__attribute__((noinline, used))
static int hook_read_range_c(void *node, uint32_t index, void *buffer,
                             int32_t offset, uint32_t length,
                             const struct phoenix_resource_block *block)
{
    FILE *file;
    size_t received;

    if (!buffer || !node || !block || index != 0u || offset < 0 ||
        node != &block->node || block->node.records != &block->record)
        return -1;
    file = pt_real_fopen ? pt_real_fopen(block->path, "rb") : NULL;
    if (!file)
        return -1;
    if (fseeko(file, (off_t)offset, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
    received = fread(buffer, 1, length, file);
    fclose(file);
    return (int)received;
}

__attribute__((naked, noinline))
void hook_read_range_entry(void)
{
    __asm__ volatile(
        "push 12(%esp)\n\tpush 12(%esp)\n\tpush 12(%esp)\n\t"
        "push %ecx\n\tpush %edx\n\tpush %eax\n\t"
        "call hook_read_range_c\n\tadd $24, %esp\n\tret\n\t");
}
