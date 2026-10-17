#pragma once
/* Minimal reader for phoenixtools.ini beside the EXE, shared by native bridges.
 * Format: "[section]" headers, "key = value" lines, '#'/';' comments, case-insensitive
 * section and key names, surrounding blanks trimmed. Reads on demand; no caching.
 * Values are UTF-8; relative diagnostic paths resolve against the EXE directory. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

#define LX_INI_PATH_CAPACITY (MAX_PATH + 1024)

/* Copies the value of [section] key into out and returns 1; returns 0 (out untouched)
 * when the file, section or key is missing. */
static inline int lx_ini_get(const char *section, const char *key, char *out, size_t capacity)
{
    wchar_t path[MAX_PATH + 32];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return 0;
    while (n && path[n - 1] != L'\\' && path[n - 1] != L'/')
        --n;
    wcscpy(path + n, L"phoenixtools.ini");
    FILE *f = _wfopen(path, L"r");
    if (!f)
        return 0;
    char line[1024], current[64] = "";
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *s = line, *e;
        while (*s == ' ' || *s == '\t')
            ++s;
        e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
        if (!*s || *s == '#' || *s == ';')
            continue;
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (close) {
                *close = 0;
                snprintf(current, sizeof current, "%s", s + 1);
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq || _stricmp(current, section))
            continue;
        char *k = s, *v = eq + 1, *ke = eq;
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t'))
            --ke;
        *ke = 0;
        while (*v == ' ' || *v == '\t')
            ++v;
        char *comment = v;
        while (*comment && !((*comment == '#' || *comment == ';') &&
               (comment == v || comment[-1] == ' ' || comment[-1] == '\t')))
            ++comment;
        if (*comment) {
            *comment = 0;
            for (e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t'); )
                *--e = 0;
        }
        if (!_stricmp(k, key) && capacity) {
            snprintf(out, capacity, "%s", v);
            found = 1;
        }
    }
    fclose(f);
    return found;
}

static inline int lx_ini_get_int(const char *section, const char *key, int fallback)
{
    char v[32];
    char *end;
    if (!lx_ini_get(section, key, v, sizeof v) || !*v)
        return fallback;
    long r = strtol(v, &end, 10);
    return *end ? fallback : (int)r;
}

/* Missing or invalid booleans use the caller's default. */
static inline int lx_ini_get_bool(const char *section, const char *key, int fallback)
{
    char value[16];
    if (!lx_ini_get(section, key, value, sizeof(value)))
        return fallback;
    if (!_stricmp(value, "true") || !_stricmp(value, "yes") ||
        !_stricmp(value, "on") || !strcmp(value, "1"))
        return 1;
    if (!_stricmp(value, "false") || !_stricmp(value, "no") ||
        !_stricmp(value, "off") || !strcmp(value, "0"))
        return 0;
    return fallback;
}

/* Returns a Windows path in out (capacity is in wchar_t units). Missing/empty
 * values, invalid UTF-8, or insufficient capacity return 0 with out untouched. */
static inline int lx_ini_get_path(const char *section, const char *key,
                                 wchar_t *out, size_t capacity)
{
    char value[1024];
    wchar_t path[1024], exe[MAX_PATH];
    if (!lx_ini_get(section, key, value, sizeof(value)) || !*value ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, path,
                             sizeof(path) / sizeof(path[0])))
        return 0;
    size_t length = wcslen(path), prefix = 0;
    if (path[0] != L'/' && path[0] != L'\\' && path[1] != L':') {
        DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        if (!n || n >= MAX_PATH)
            return 0;
        while (n && exe[n - 1] != L'\\' && exe[n - 1] != L'/')
            --n;
        prefix = n;
    }
    if (prefix + length >= capacity)
        return 0;
    if (prefix)
        wmemcpy(out, exe, prefix);
    wmemcpy(out + prefix, path, length + 1);
    return 1;
}
