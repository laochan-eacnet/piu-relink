#define _GNU_SOURCE

#include "config/config_internal.h"
#include "core/log.h"

#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef FILE *(*fopen_fn)(const char *, const char *);

static char *trim(char *text)
{
    char *end;

    while (*text && isspace((unsigned char)*text))
        ++text;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return text;
}

static FILE *open_config(const char *requested_path, char *path, size_t capacity)
{
    fopen_fn real_fopen = (fopen_fn)dlsym(RTLD_NEXT, "fopen");
    Dl_info info;
    char module[PHOENIXTOOLS_PATH_MAX];
    char *slash;
    FILE *file;
    int written;

    if (!real_fopen)
        return NULL;
    if (requested_path) {
        if (strlen(requested_path) >= capacity)
            return NULL;
        strcpy(path, requested_path);
        return real_fopen(path, "r");
    }
    if (!dladdr((void *)&pt_config_load, &info) || !info.dli_fname ||
        strlen(info.dli_fname) >= sizeof(module))
        return NULL;

    strcpy(module, info.dli_fname);
    slash = strrchr(module, '/');
    if (!slash)
        return NULL;
    *slash = '\0';

    written = snprintf(path, capacity, "%s/phoenixtools.ini", module);
    if (written >= 0 && (size_t)written < capacity) {
        file = real_fopen(path, "r");
        if (file)
            return file;
    }
    written = snprintf(path, capacity, "%s/../phoenixtools.ini", module);
    if (written < 0 || (size_t)written >= capacity)
        return NULL;
    return real_fopen(path, "r");
}

static int runtime_section(const char *section)
{
    return !strcmp(section, "mounts") || !strcmp(section, "immutable") ||
           !strcmp(section, "process") || !strcmp(section, "display") ||
           !strcmp(section, "audio") || !strcmp(section, "serial") ||
           !strcmp(section, "debug");
}

void pt_config_load(void)
{
    pt_config_load_path(NULL);
}

void pt_config_load_path(const char *requested_path)
{
    FILE *file;
    char path[PHOENIXTOOLS_PATH_MAX];
    char line[2048];
    char section[64] = "";
    unsigned int line_number = 0;

    pt_config_set_defaults();
    file = open_config(requested_path, path, sizeof(path));
    if (!file) {
        pt_log("config", "phoenixtools.ini not found; using safe defaults");
        return;
    }
    strcpy(pt_config_state.loaded_from, path);

    while (fgets(line, sizeof(line), file)) {
        char *text = trim(line);
        char *equals;
        size_t length;
        int result;

        ++line_number;
        if (!*text || *text == '#' || *text == ';')
            continue;
        length = strlen(text);
        if (text[0] == '[' && length >= 3u && text[length - 1u] == ']') {
            text[length - 1u] = '\0';
            pt_config_normalize_name(text + 1, section, sizeof(section));
            continue;
        }
        if (runtime_section(section))
            continue;
        equals = strchr(text, '=');
        if (!equals) {
            pt_log("config", "%s:%u: expected key=value", path, line_number);
            continue;
        }
        *equals = '\0';
        result = pt_config_apply(section, trim(text), trim(equals + 1),
                                 path, line_number);
        if (result > 0)
            pt_log("config", "%s:%u: ignored unknown [%s] key %s",
                   path, line_number, section, trim(text));
    }
    fclose(file);

    if (pt_config_state.custom_id_min > pt_config_state.custom_id_max) {
        pt_log("config", "%s: custom_id_min exceeds custom_id_max; using default range",
               path);
        pt_config_state.custom_id_min = PT_CUSTOM_ID_MIN_DEFAULT;
        pt_config_state.custom_id_max = PT_CUSTOM_ID_MAX_DEFAULT;
    }

    if (pt_config_verbose()) {
        const struct pt_input_bindings *input = &pt_config_state.input;
        pt_log("config", "loaded %s: region=%s virtual-lxio=%d",
               path, pt_region_name(pt_config_state.region_slot),
               pt_config_state.virtual_lxio);
        pt_log("config", "P1=%s,%s,%s,%s,%s P2=%s,%s,%s,%s,%s",
               input->upper_left, input->upper_right, input->center,
               input->lower_left, input->lower_right, input->p2_upper_left,
               input->p2_upper_right, input->p2_center,
               input->p2_lower_left, input->p2_lower_right);
    }
}
