#define _GNU_SOURCE

#include "core/runtime.h"
#include "resources/song_paths.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>

static const char *sandbox_path(const char *path, char *buffer, size_t capacity)
{
    const struct pt_config *config = pt_config_get();
    const char *suffix;
    const char *subdirectory;
    int written;

    if (!path)
        return path;
    if (!strcmp(path, "/SETTINGS") || !strncmp(path, "/SETTINGS/", 10)) {
        suffix = path + 9;
        subdirectory = "settings";
    } else if (!strcmp(path, "/var/ucs") || !strncmp(path, "/var/ucs/", 9)) {
        suffix = path + 8;
        subdirectory = "var/ucs";
    } else {
        return path;
    }
    if (!*config->runtime_dir) {
        errno = EACCES;
        return NULL;
    }
    written = snprintf(buffer, capacity, "%s/%s%s", config->runtime_dir,
                       subdirectory, suffix);
    if (written < 0 || (size_t)written >= capacity) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return buffer;
}

static int raw_system_disk(const char *path)
{
    return path && !strcmp(path, "/dev/sda");
}

static void create_directory_tree(const char *path)
{
    char current[PHOENIXTOOLS_PATH_MAX];
    char *cursor;

    if (!pt_real_mkdir || !path || !*path || strlen(path) >= sizeof(current))
        return;
    strcpy(current, path);
    for (cursor = current + (current[0] == '/'); *cursor; ++cursor) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (*current)
            (void)pt_real_mkdir(current, 0755);
        *cursor = '/';
    }
    (void)pt_real_mkdir(current, 0755);
}

void pt_sandbox_initialize(void)
{
    const char *runtime = pt_config_get()->runtime_dir;
    char path[PHOENIXTOOLS_PATH_MAX];
    int written;

    if (!*runtime)
        return;
    create_directory_tree(runtime);
    written = snprintf(path, sizeof(path), "%s/settings", runtime);
    if (written >= 0 && (size_t)written < sizeof(path))
        create_directory_tree(path);
    written = snprintf(path, sizeof(path), "%s/var/ucs", runtime);
    if (written >= 0 && (size_t)written < sizeof(path))
        create_directory_tree(path);
}

FILE *fopen(const char *path, const char *mode)
{
    char redirected[PHOENIXTOOLS_PATH_MAX];
    const char *actual;

    if (!pt_real_fopen)
        pt_real_fopen = dlsym(RTLD_NEXT, "fopen");
    if (!pt_real_fopen) {
        errno = ENOSYS;
        return NULL;
    }
    actual = sandbox_path(path, redirected, sizeof(redirected));
    if (!actual)
        return NULL;
    if (actual != path)
        return pt_real_fopen(actual, mode);
    if (mode && mode[0] == 'r' && !strchr(mode, '+') &&
        pt_resource_resolve_read_path(path, redirected, sizeof(redirected)) == 0)
        return pt_real_fopen(redirected, mode);
    if (!raw_system_disk(path))
        return pt_real_fopen(path, mode);
    if (!*pt_config_get()->virtual_disk) {
        errno = EACCES;
        pt_log("disk", "blocked fopen of /dev/sda");
        return NULL;
    }
    return pt_real_fopen(pt_config_get()->virtual_disk, mode);
}

int open(const char *path, int flags, ...)
{
    char redirected[PHOENIXTOOLS_PATH_MAX];
    const char *actual;
    mode_t mode = 0;
    va_list arguments;

    if (!pt_real_open)
        pt_real_open = dlsym(RTLD_NEXT, "open");
    if (!pt_real_open) {
        errno = ENOSYS;
        return -1;
    }
    actual = sandbox_path(path, redirected, sizeof(redirected));
    if (!actual)
        return -1;
    if (raw_system_disk(path)) {
        /* Read-only opens (the HDD model/serial probe of the system-information screens)
         * get the virtual disk image, created empty if missing, so the following
         * HDIO_GET_IDENTITY ioctl is answered below. Writable raw opens stay blocked. */
        if ((flags & O_ACCMODE) != O_RDONLY || !*pt_config_get()->virtual_disk) {
            errno = EACCES;
            pt_log("disk", "blocked raw open of /dev/sda");
            return -1;
        }
        return pt_real_open(pt_config_get()->virtual_disk,
                            (flags & ~(O_TRUNC | O_EXCL)) | O_CREAT, 0600);
    }
    if (actual == path && (flags & O_ACCMODE) == O_RDONLY &&
        !(flags & (O_CREAT | O_TRUNC)) &&
        pt_resource_resolve_read_path(path, redirected, sizeof(redirected)) == 0)
        actual = redirected;
    if (flags & O_CREAT) {
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
        return pt_real_open(actual, flags, mode);
    }
    return pt_real_open(actual, flags);
}

int mkdir(const char *path, mode_t mode)
{
    char redirected[PHOENIXTOOLS_PATH_MAX];
    const char *actual;

    if (!pt_real_mkdir)
        pt_real_mkdir = dlsym(RTLD_NEXT, "mkdir");
    if (!pt_real_mkdir) {
        errno = ENOSYS;
        return -1;
    }
    actual = sandbox_path(path, redirected, sizeof(redirected));
    if (!actual)
        return -1;
    if (pt_real_mkdir(actual, mode) == 0)
        return 0;
    return actual != path && errno == EEXIST ? 0 : -1;
}
