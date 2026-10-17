#define _GNU_SOURCE

#include "resources/basename_index.h"

#include "core/limits.h"
#include "core/log.h"

#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* One walk per root on first use, then hash lookups. A per-miss walk of the whole data tree
 * made every unresolved bare-name open (e.g. resources of songs absent from the local data)
 * cost tens of thousands of opendir/lstat calls and froze song selection. The roots are not
 * modified while the game runs; content added later is seen after restart. Semantics match
 * the former walk: regular files classified by lstat (symlinks are not followed), at most 32
 * directory levels, ASCII case-insensitive names, and a basename present more than once below
 * a root resolves nowhere. */
struct basename_entry {
    char *path;
    const char *name;
    uint32_t hash;
    int ambiguous;
};

struct basename_index {
    pthread_mutex_t mutex;
    char root[PHOENIXTOOLS_PATH_MAX];
    struct basename_entry *slots;
    size_t capacity;
    size_t count;
    int built;
};

static struct basename_index indexes[2] = {
    {PTHREAD_MUTEX_INITIALIZER, "", NULL, 0, 0, 0},
    {PTHREAD_MUTEX_INITIALIZER, "", NULL, 0, 0, 0},
};

static uint32_t basename_hash(const char *name)
{
    uint32_t hash = 2166136261u;

    for (; *name; ++name) {
        unsigned char character = (unsigned char)*name;
        if (character >= 'A' && character <= 'Z')
            character = (unsigned char)(character - 'A' + 'a');
        hash = (hash ^ character) * 16777619u;
    }
    return hash;
}

static struct basename_entry *index_slot(struct basename_entry *slots, size_t capacity,
                                         uint32_t hash, const char *name)
{
    size_t mask = capacity - 1u;
    size_t position = hash & mask;

    while (slots[position].path && (slots[position].hash != hash ||
                                    strcasecmp(slots[position].name, name)))
        position = (position + 1u) & mask;
    return &slots[position];
}

static int index_grow(struct basename_index *index)
{
    size_t capacity = index->capacity ? index->capacity * 2u : 4096u;
    struct basename_entry *slots = calloc(capacity, sizeof(*slots));
    size_t position;

    if (!slots)
        return -1;
    for (position = 0; position < index->capacity; ++position) {
        struct basename_entry *entry = &index->slots[position];
        if (entry->path)
            *index_slot(slots, capacity, entry->hash, entry->name) = *entry;
    }
    free(index->slots);
    index->slots = slots;
    index->capacity = capacity;
    return 0;
}

static int index_add(struct basename_index *index, const char *path, size_t name_offset)
{
    struct basename_entry *entry;
    const char *name = path + name_offset;
    uint32_t hash = basename_hash(name);

    if ((index->count + 1u) * 2u > index->capacity && index_grow(index) != 0)
        return -1;
    entry = index_slot(index->slots, index->capacity, hash, name);
    if (entry->path) {
        entry->ambiguous = 1;
        return 0;
    }
    if (!(entry->path = strdup(path)))
        return -1;
    entry->name = entry->path + name_offset;
    entry->hash = hash;
    entry->ambiguous = 0;
    ++index->count;
    return 0;
}

static void index_free(struct basename_index *index)
{
    size_t position;

    for (position = 0; position < index->capacity; ++position)
        free(index->slots[position].path);
    free(index->slots);
    index->slots = NULL;
    index->capacity = 0;
    index->count = 0;
    index->built = 0;
}

static int index_directory(struct basename_index *index, const char *directory,
                           unsigned int depth)
{
    DIR *stream;
    struct dirent64 *entry;
    int result = 0;

    if (depth > 32u || !(stream = opendir(directory)))
        return 0;
    while (!result && (entry = readdir64(stream)) != NULL) {
        char path[PHOENIXTOOLS_PATH_MAX];
        struct stat status;
        int written;

        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        written = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(path) ||
            lstat(path, &status) != 0)
            continue;
        if (S_ISDIR(status.st_mode))
            result = index_directory(index, path, depth + 1u);
        else if (S_ISREG(status.st_mode))
            result = index_add(index, path, (size_t)written - strlen(entry->d_name));
    }
    closedir(stream);
    return result;
}

int pt_basename_index_lookup(unsigned int slot, const char *root, const char *name,
                             char *resolved, size_t capacity)
{
    struct basename_index *index;
    const struct basename_entry *entry;
    int result = 0;

    if (slot >= sizeof(indexes) / sizeof(indexes[0]) || strlen(root) >= sizeof(index->root))
        return -2;
    index = &indexes[slot];
    pthread_mutex_lock(&index->mutex);
    if (index->built && strcmp(index->root, root))
        index_free(index);
    if (!index->built) {
        if (index_grow(index) != 0 || index_directory(index, root, 0u) != 0) {
            index_free(index);
            pthread_mutex_unlock(&index->mutex);
            pt_log("resource", "basename index for %s unavailable: out of memory", root);
            return -2;
        }
        strcpy(index->root, root);
        index->built = 1;
    }
    entry = index_slot(index->slots, index->capacity, basename_hash(name), name);
    if (entry->path) {
        if (entry->ambiguous)
            result = -1;
        else if (strlen(entry->path) + 1u <= capacity) {
            strcpy(resolved, entry->path);
            result = 1;
        } else
            result = -2;
    }
    pthread_mutex_unlock(&index->mutex);
    return result;
}
