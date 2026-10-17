#define _GNU_SOURCE
#include "core/runtime.h"
#include "safety/safety.h"
#include "safety/private_http.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>

static int raw_fd = -1;

static int enabled(void)
{
    const struct pt_config *c = pt_config_get();
    return pt_network_private_base(c->api_base) || pt_network_private_base(c->cdn_base);
}

static int configured_endpoint(const struct sockaddr_in *address)
{
    const struct pt_config *config = pt_config_get();
    struct pt_private_http parsed;
    return (pt_private_http_base(config->api_base, &parsed) &&
            address->sin_addr.s_addr == parsed.address.sin_addr.s_addr &&
            address->sin_port == parsed.address.sin_port) ||
           (pt_private_http_base(config->cdn_base, &parsed) &&
            address->sin_addr.s_addr == parsed.address.sin_addr.s_addr &&
            address->sin_port == parsed.address.sin_port);
}

struct hostent *gethostbyname(const char *name)
{
    const struct pt_config *config = pt_config_get();
    struct pt_private_http parsed;
    static struct in_addr address;
    static char *addresses[] = {(char *)&address, NULL};
    static char *aliases[] = {NULL};
    static struct hostent host = {"private-server", aliases, AF_INET, 4, addresses};
    if (pt_private_http_base(config->api_base, &parsed) ||
        pt_private_http_base(config->cdn_base, &parsed)) {
        address = parsed.address.sin_addr;
        pt_log("network", "DNS isolated %s -> %s", name, inet_ntoa(address));
        return &host;
    }
    pt_log("network", "DNS blocked %s", name);
    h_errno = HOST_NOT_FOUND;
    return NULL;
}

int connect(int fd, const struct sockaddr *address, socklen_t size)
{
    int (*real)(int, const struct sockaddr *, socklen_t) = dlsym(RTLD_NEXT, "connect");
    struct sockaddr_in local;
    struct pt_private_http api;
    const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)address;
    if (!address || size < sizeof(struct sockaddr_in) || address->sa_family != AF_INET) {
        pt_log("network", "connect blocked fd=%d (unsupported address)", fd);
        errno = EACCES; return -1;
    }
    local = *ipv4;
    if (ntohs(local.sin_port) == 50400) {
        unsigned int port = pt_config_get()->game_port;
        if (!pt_private_http_base(pt_config_get()->api_base, &api)) {
            errno = EACCES; return -1;
        }
        local.sin_port = htons((uint16_t)port);
        local.sin_addr = api.address.sin_addr;
        raw_fd = fd;
        pt_log("network", "game TCP isolated -> %s:%u fd=%d",
               inet_ntoa(local.sin_addr), (unsigned)port, fd);
    } else if (!configured_endpoint(&local)) {
        pt_log("network", "connect blocked unconfigured endpoint fd=%d port=%u",
               fd, ntohs(local.sin_port));
        errno = EACCES; return -1;
    }
    if (!real) { errno = ENOSYS; return -1; }
    return real(fd, (const struct sockaddr *)&local, sizeof(local));
}

static void packet(const char *direction, const void *data, size_t length)
{
    static const char digits[] = "0123456789abcdef";
    const unsigned char *p = data;
    char hex[193];
    size_t i, count = length > 64 ? 64 : length;
    for (i = 0; i < count; ++i) {
        hex[i*3] = digits[p[i] >> 4];
        hex[i*3+1] = digits[p[i] & 15];
        hex[i*3+2] = ' ';
    }
    hex[count*3] = 0;
    pt_log("network", "TCP %s bytes=%zu head=%s", direction, length, hex);
}

ssize_t send(int fd, const void *data, size_t size, int flags)
{
    ssize_t (*real)(int, const void *, size_t, int) = dlsym(RTLD_NEXT, "send");
    if (fd == raw_fd && data) packet("send", data, size);
    if (!real) { errno = ENOSYS; return -1; }
    return real(fd, data, size, flags);
}

ssize_t recv(int fd, void *data, size_t size, int flags)
{
    ssize_t (*real)(int, void *, size_t, int) = dlsym(RTLD_NEXT, "recv");
    ssize_t result;
    if (!real) { errno = ENOSYS; return -1; }
    result = real(fd, data, size, flags);
    if (fd == raw_fd && result > 0) packet("recv", data, (size_t)result);
    return result;
}

ssize_t sendto(int fd, const void *data, size_t size, int flags,
               const struct sockaddr *address, socklen_t length)
{
    ssize_t (*real)(int, const void *, size_t, int, const struct sockaddr *, socklen_t) =
        dlsym(RTLD_NEXT, "sendto");
    const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)address;
    if (!enabled() || !address || length < sizeof(*ipv4) ||
        address->sa_family != AF_INET || ipv4->sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
        errno = EACCES; return -1;
    }
    if (!real) { errno = ENOSYS; return -1; }
    return real(fd, data, size, flags, address, length);
}
