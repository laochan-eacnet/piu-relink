#pragma once

#include <arpa/inet.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct pt_private_http {
    struct sockaddr_in address;
    const char *host, *path;
    size_t host_length;
};

/* Deliberately no DNS, shorthand IPv4, octal, userinfo or alternate loopbacks. */
static inline int pt_private_http_address(const char *host, size_t length,
                                          struct in_addr *address)
{
    const char *p = host, *end = host + length;
    uint32_t value = 0;
    unsigned i;
    if (length == 9 && !memcmp(host, "localhost", 9)) {
        address->s_addr = htonl(INADDR_LOOPBACK);
        return 1;
    }
    for (i = 0; i < 4; ++i) {
        const char *start = p;
        unsigned octet = 0;
        if (p == end || *p < '0' || *p > '9') return 0;
        while (p < end && *p >= '0' && *p <= '9') {
            octet = octet * 10u + (unsigned)(*p++ - '0');
            if (octet > 255) return 0;
        }
        if (p - start > 1 && *start == '0') return 0;
        value = (value << 8) | octet;
        if (i < 3 && (p == end || *p++ != '.')) return 0;
    }
    if (p != end || !(value == INADDR_LOOPBACK ||
        (value & 0xff000000u) == 0x0a000000u ||
        (value & 0xfff00000u) == 0xac100000u ||
        (value & 0xffff0000u) == 0xc0a80000u)) return 0;
    address->s_addr = htonl(value);
    return 1;
}

/* Curl-compatible errors: unsupported protocol 1, malformed URL 3, blocked 7. */
static inline int pt_private_http_parse(const char *url, struct pt_private_http *out)
{
    const char *authority, *end, *port, *p;
    unsigned number = 80;
    if (!url || strncmp(url, "http://", 7)) return 1;
    authority = url + 7;
    end = strchr(authority, '/');
    if (!end) end = authority + strlen(authority);
    port = memchr(authority, ':', (size_t)(end - authority));
    out->host = authority;
    out->host_length = (size_t)((port ? port : end) - authority);
    memset(&out->address, 0, sizeof(out->address));
    if (!pt_private_http_address(authority, out->host_length,
                                 &out->address.sin_addr)) return 7;
    if (port) {
        p = port + 1;
        number = 0;
        if (p == end) return 3;
        for (; p < end; ++p) {
            if (*p < '0' || *p > '9') return 3;
            number = number * 10u + (unsigned)(*p - '0');
            if (number > 65535) return 3;
        }
        if (!number) return 3;
    }
    out->address.sin_family = AF_INET;
    out->address.sin_port = htons((uint16_t)number);
    out->path = *end ? end : "/";
    for (p = out->path; *p; ++p)
        if ((unsigned char)*p <= 32 || (unsigned char)*p == 127 || *p == '#') return 3;
    return 0;
}

static inline int pt_private_http_base(const char *base, struct pt_private_http *out)
{
    return !pt_private_http_parse(base, out) && !strchr(out->path, '?');
}
