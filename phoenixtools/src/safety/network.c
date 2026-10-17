#define _GNU_SOURCE
#include "core/runtime.h"
#include "safety/safety.h"
#include "safety/private_http.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Accept only explicit private HTTP origins; never contact original servers. */
int pt_network_private_base(const char *base)
{
    struct pt_private_http parsed;
    return pt_private_http_base(base, &parsed);
}

static int cdn_request(const char *authority, const char *path)
{
    static const char host[] = "xx-cdn.piugame.com";
    const char *p;
    unsigned port = 0;
    if (!authority || !path || path - authority < (ptrdiff_t)(sizeof(host) - 1) ||
        strncasecmp(authority, host, sizeof(host) - 1) ||
        strncmp(path, "/2023/", 6)) return 0;
    p = authority + sizeof(host) - 1;
    if (p == path) return 1;
    if (*p++ != ':' || p == path) return 0;
    for (; p < path; ++p) {
        if (*p < '0' || *p > '9') return 0;
        port = port * 10u + (unsigned)(*p - '0');
        if (port > 65535) return 0;
    }
    return port != 0;
}

void *curl_easy_init(void)
{
    void *(*real)(void) = dlsym(RTLD_NEXT, "curl_easy_init");
    return real ? real() : NULL;
}

void curl_easy_cleanup(void *curl)
{
    void (*real)(void *) = dlsym(RTLD_NEXT, "curl_easy_cleanup");
    if (real) real(curl);
}

int curl_easy_getinfo(void *curl, int info, ...)
{
    int (*real)(void *, int, ...) = dlsym(RTLD_NEXT, "curl_easy_getinfo");
    va_list ap;
    void *value;
    va_start(ap, info); value = va_arg(ap, void *); va_end(ap);
    return real ? real(curl, info, value) : 2;
}

int curl_easy_perform(void *curl)
{
    int (*real)(void *) = dlsym(RTLD_NEXT, "curl_easy_perform");
    int result = real ? real(curl) : 7;
    long status = 0;
    curl_easy_getinfo(curl, 0x200002, &status);
    pt_log("network", "HTTP complete curl=%d status=%ld", result, status);
    return result;
}

const char *curl_easy_strerror(int code)
{
    const char *(*real)(int) = dlsym(RTLD_NEXT, "curl_easy_strerror");
    return real ? real(code) : "offline";
}

int curl_easy_setopt(void *curl, int option, ...)
{
    va_list ap;
    uintptr_t value;
    if (!pt_real_curl_easy_setopt)
        pt_real_curl_easy_setopt = dlsym(RTLD_NEXT, "curl_easy_setopt");
    if (!pt_real_curl_easy_setopt) return 2;
    va_start(ap, option);
    if (option >= CURLOPTTYPE_OFF_T && option < CURLOPTTYPE_BLOB) {
        long long wide = va_arg(ap, long long);
        va_end(ap);
        return pt_real_curl_easy_setopt(curl, option, wide);
    }
    value = va_arg(ap, uintptr_t);
    va_end(ap);
    if (option == CURLOPT_URL_VALUE) {
        const struct pt_config *config = pt_config_get();
        const char *original = (const char *)value;
        const char *base = config->api_base;
        const char *authority = original ? strstr(original, "://") : NULL;
        const char *path = authority ? strchr(authority + 3, '/') : NULL;
        char local[2048];
        int length;
        if (cdn_request(authority ? authority + 3 : NULL, path))
            base = config->cdn_base;
        if (!path) path = "/";
        length = pt_network_private_base(base) ?
            snprintf(local, sizeof(local), "%.*s%s",
                     (int)(strlen(base)-(base[strlen(base)-1] == '/')), base, path) : -1;
        if (length < 0 || (size_t)length >= sizeof(local))
            strcpy(local, "offline://blocked");
        pt_log("network", "URL %.400s -> %.400s", original ? original : "(null)", local);
        return pt_real_curl_easy_setopt(curl, option, local);
    }
    if (option == 10015 && value)
        pt_log("network", "POST body (first 256 bytes): %.256s", (const char *)value);
    return pt_real_curl_easy_setopt(curl, option, value);
}
