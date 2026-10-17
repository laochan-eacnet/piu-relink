/* phoenixtools.ini beside the EXE: the single configuration file shared with phoenixtools (guest)
 * and the native bridges. lxrt owns [mounts], [immutable], [process], [serial] and the lxrt keys of
 * [debug]; every other section is ignored here. Parsed once from DllMain, before any other lxrt
 * code runs; entries are kept in file order and never change afterwards.
 *
 * Format: "[section]" headers, "key = value" lines, whole-line '#'/';' comments, surrounding
 * blanks trimmed. Section and key names compare case-insensitively ([mounts]/[serial] keys are
 * guest paths and compare exactly). A '#' or ';' preceded by a blank starts a trailing comment. */
#include "lxrt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *section, *key, *value; } entry;
static entry *entries;
static unsigned nentries;
static char ini_dir[MAX_PATH * 3];   /* UTF-8, '/' separators */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}

static void strip_comment(char *v)
{
    for (char *c = v; *c; c++)
        if ((*c == '#' || *c == ';') && c > v && (c[-1] == ' ' || c[-1] == '\t')) {
            *c = 0;
            trim(v);
            return;
        }
}

void lx_config_load(void)
{
    wchar_t w[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, w, MAX_PATH);
    w[n < MAX_PATH ? n : MAX_PATH - 1] = 0;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, ini_dir, sizeof ini_dir, NULL, NULL);
    for (char *c = ini_dir; *c; c++)
        if (*c == '\\')
            *c = '/';
    char *slash = strrchr(ini_dir, '/');
    if (slash)
        *slash = 0;

    wchar_t *dir_end = wcsrchr(w, L'\\');
    if (!dir_end || (size_t)(dir_end - w) + 18 >= MAX_PATH)
        return;
    wcscpy(dir_end + 1, L"phoenixtools.ini");
    FILE *f = _wfopen(w, L"r");
    if (!f)
        return;
    char line[2048], section[64] = "";
    unsigned cap = 0;
    while (fgets(line, sizeof line, f)) {
        char *l = trim(line);
        if (!*l || *l == '#' || *l == ';')
            continue;
        if (*l == '[') {
            char *e = strchr(l, ']');
            if (e) {
                *e = 0;
                snprintf(section, sizeof section, "%s", trim(l + 1));
            }
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *k = trim(l), *v = trim(eq + 1);
        strip_comment(v);
        if (nentries == cap) {
            cap = cap ? cap * 2 : 64;
            entries = realloc(entries, cap * sizeof *entries);
        }
        entries[nentries].section = _strdup(section);
        entries[nentries].key = _strdup(k);
        entries[nentries].value = _strdup(v);
        nentries++;
    }
    fclose(f);
}

int lx_config_next(const char *section, unsigned *cursor, const char **key, const char **value)
{
    for (unsigned i = *cursor; i < nentries; i++)
        if (!_stricmp(entries[i].section, section)) {
            *cursor = i + 1;
            *key = entries[i].key;
            *value = entries[i].value;
            return 1;
        }
    *cursor = nentries;
    return 0;
}

const char *lx_config_get(const char *section, const char *key)
{
    const char *found = NULL;
    for (unsigned i = 0; i < nentries; i++)
        if (!_stricmp(entries[i].section, section) && !_stricmp(entries[i].key, key))
            found = entries[i].value;   /* last assignment wins */
    return found;
}

int lx_config_bool(const char *section, const char *key)
{
    const char *v = lx_config_get(section, key);
    if (!v || !*v)
        return 0;
    static const char *const yes[] = { "1", "true", "yes", "on" }, *const no[] = { "0", "false", "no", "off" };
    for (unsigned i = 0; i < LX_COUNT(yes); i++) {
        if (!_stricmp(v, yes[i]))
            return 1;
        if (!_stricmp(v, no[i]))
            return 0;
    }
    lx_log("phoenixtools.ini: [%s] %s = '%s' is not a boolean; using false", section, key, v);
    return 0;
}

char *lx_config_path(const char *value)
{
    size_t n = strlen(value);
    int absolute = value[0] == '/' || value[0] == '\\' ||
                   (((value[0] | 0x20) >= 'a' && (value[0] | 0x20) <= 'z') && value[1] == ':');
    char *out = malloc(n + (absolute ? 1 : strlen(ini_dir) + 2));
    if (absolute)
        memcpy(out, value, n + 1);
    else
        sprintf(out, "%s/%s", ini_dir, value);
    return out;
}
