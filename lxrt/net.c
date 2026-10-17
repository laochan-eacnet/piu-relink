/* Linux i386 sockets/resolver over Winsock. Native sockets remain nonblocking and guest
 * blocking calls wait explicitly; MSG_DONTWAIT never mutates another thread's mode.
 * Linux AF_INET6=10, options, message flags, fd bitmaps and errno are marshalled explicitly.
 * getifaddrs (glibc order: IPv4 entries, then IPv6) names the up adapter with the lowest-metric
 * IPv4 default gateway eth0 and the real loopback lo; /proc/net/route and /sys/class/net/<if>
 * describe the same adapters (lx_net_special).
 * No AF_UNIX/ancillary-fd transport: socketpair is ENOSYS, sendmsg controls EOPNOTSUPP.
 * poll checks native pipe readiness at 10-ms intervals; select treats files as ready. */
#define FD_SETSIZE 4096
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "lxrt.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>

#pragma pack(push, 4)
typedef struct { uint16_t family; char data[14]; } g_sockaddr;
typedef struct { uint16_t family, port; uint32_t addr; char zero[8]; } g_in;
typedef struct { uint16_t family, port; uint32_t flow; unsigned char addr[16]; uint32_t scope; } g_in6;
typedef struct { int32_t sec, usec; } g_timeval;
typedef struct { int fd; int16_t events, revents; } g_pollfd;
typedef struct { void *base; uint32_t len; } g_iovec;
typedef struct { void *name; uint32_t namelen; g_iovec *iov; uint32_t iovlen; void *control; uint32_t controllen; int flags; } g_msghdr;
typedef struct { char *name; char **aliases; int type, length; char **addresses; } g_hostent;
typedef struct g_addrinfo { int flags, family, socktype, protocol; uint32_t addrlen; g_sockaddr *addr; char *canonname; struct g_addrinfo *next; } g_addrinfo;
typedef struct g_ifaddrs { struct g_ifaddrs *next; char *name; uint32_t flags; g_sockaddr *addr, *netmask, *broadaddr; void *data; } g_ifaddrs;
typedef struct { char name[16]; unsigned char value[16]; } g_ifreq;
typedef struct { int len; g_ifreq *buf; } g_ifconf;
typedef struct { char *name, **aliases; int port; char *proto; } g_servent;
#pragma pack(pop)
static_assert(sizeof(g_sockaddr) == 16, "sockaddr");
static_assert(sizeof(g_in) == 16 && offsetof(g_in, addr) == 4, "sockaddr_in");
static_assert(sizeof(g_in6) == 28 && offsetof(g_in6, scope) == 24, "sockaddr_in6");
static_assert(sizeof(g_timeval) == 8, "timeval");
static_assert(sizeof(g_pollfd) == 8 && offsetof(g_pollfd, revents) == 6, "pollfd");
static_assert(sizeof(g_iovec) == 8, "iovec");
static_assert(sizeof(g_msghdr) == 28 && offsetof(g_msghdr, flags) == 24, "msghdr");
static_assert(sizeof(g_hostent) == 20 && offsetof(g_hostent, addresses) == 16, "hostent");
static_assert(sizeof(g_addrinfo) == 32 && offsetof(g_addrinfo, addr) == 20 && offsetof(g_addrinfo, next) == 28, "addrinfo");
static_assert(sizeof(g_ifaddrs) == 28 && offsetof(g_ifaddrs, addr) == 12 && offsetof(g_ifaddrs, data) == 24, "ifaddrs");
static_assert(sizeof(g_ifreq) == 32 && sizeof(g_ifconf) == 8, "interface ioctl");
static_assert(sizeof(g_servent) == 16, "servent");
static_assert(sizeof(struct sockaddr_in) == 16 && sizeof(struct sockaddr_in6) == 28, "native socket layouts");

static INIT_ONCE net_once = INIT_ONCE_STATIC_INIT;
static int net_start_error;
static BOOL CALLBACK net_start(PINIT_ONCE once, PVOID arg, PVOID *ctx)
{
    WSADATA w;
    net_start_error = WSAStartup(MAKEWORD(2, 2), &w);
    return TRUE;
}
static int net_init(void)
{
    InitOnceExecuteOnce(&net_once, net_start, NULL, NULL);
    return net_start_error ? lx_set_errno(LX_ENETUNREACH) : 0;
}
static int net_errno(int e)
{
    switch (e) {
    case 0: return 0;
    case WSAEINTR: return LX_EINTR;
    case WSAEBADF: return LX_EBADF;
    case WSAEACCES: return LX_EACCES;
    case WSAEFAULT: return LX_EFAULT;
    case WSAEINVAL: return LX_EINVAL;
    case WSAEMFILE: return LX_EMFILE;
    case WSAEWOULDBLOCK: return LX_EAGAIN;
    case WSAEINPROGRESS: return LX_EINPROGRESS;
    case WSAEALREADY: return LX_EALREADY;
    case WSAENOTSOCK: return LX_ENOTSOCK;
    case WSAEDESTADDRREQ: return 89;
    case WSAEMSGSIZE: return 90;
    case WSAEPROTOTYPE: return 91;
    case WSAENOPROTOOPT: return 92;
    case WSAEPROTONOSUPPORT: return 93;
    case WSAESOCKTNOSUPPORT: return 94;
    case WSAEOPNOTSUPP: return LX_EOPNOTSUPP;
    case WSAEPFNOSUPPORT: return 96;
    case WSAEAFNOSUPPORT: return LX_EAFNOSUPPORT;
    case WSAEADDRINUSE: return LX_EADDRINUSE;
    case WSAEADDRNOTAVAIL: return LX_EADDRNOTAVAIL;
    case WSAENETDOWN: return 100;
    case WSAENETUNREACH: return LX_ENETUNREACH;
    case WSAENETRESET: return 102;
    case WSAECONNABORTED: return LX_ECONNABORTED;
    case WSAECONNRESET: return LX_ECONNRESET;
    case WSAENOBUFS: return 105;
    case WSAEISCONN: return 106;
    case WSAENOTCONN: return LX_ENOTCONN;
    case WSAESHUTDOWN: return LX_EPIPE;
    case WSAETIMEDOUT: return LX_ETIMEDOUT;
    case WSAECONNREFUSED: return LX_ECONNREFUSED;
    case WSAEHOSTDOWN: return 112;
    case WSAEHOSTUNREACH: return LX_EHOSTUNREACH;
    case WSA_NOT_ENOUGH_MEMORY: return LX_ENOMEM;
    default: return LX_EIO;
    }
}
static int net_fail(void) { return lx_set_errno(net_errno(WSAGetLastError())); }
static int host_family(int family) { return family == 10 ? AF_INET6 : family == 2 ? AF_INET : family == 0 ? AF_UNSPEC : -1; }
static int guest_family(int family) { return family == AF_INET6 ? 10 : family; }
static int in_addr(const void *addr, uint32_t len, SOCKADDR_STORAGE *out)
{
    if (!addr) return lx_set_errno(LX_EFAULT);
    if (len < 2 || len > sizeof(*out)) return lx_set_errno(LX_EINVAL);
    memset(out, 0, sizeof(*out));
    memcpy(out, addr, len);
    int family = host_family(out->ss_family);
    if (family < 0) return lx_set_errno(LX_EAFNOSUPPORT);
    if ((family == AF_INET && len < 16) || (family == AF_INET6 && len < 28)) return lx_set_errno(LX_EINVAL);
    out->ss_family = (ADDRESS_FAMILY)family;
    return 0;
}
static void out_addr(void *dst, uint32_t *len, const void *addr, int size)
{
    if (!len) return;
    if (dst) {
        uint32_t n = *len < (uint32_t)size ? *len : (uint32_t)size;
        memcpy(dst, addr, n);
        if (n >= 2) *(uint16_t *)dst = (uint16_t)guest_family(((const SOCKADDR *)addr)->sa_family);
    }
    *len = (uint32_t)size;
}
static lx_fd *socket_fd(int fd)
{
    lx_fd *f = lx_fd_get(fd);
    if (f && f->kind != LX_FD_SOCKET) { lx_set_errno(LX_ENOTSOCK); return NULL; }
    return f;
}
/* The fd hooks receive SOCKET rather than the fd; keep an O(1) reverse index for them. */
static struct { uintptr_t socket; int fd; } socket_index[8192];
static SRWLOCK socket_index_lock = SRWLOCK_INIT;
static void index_socket(uintptr_t s, int fd)
{
    unsigned slot = (unsigned)(s >> 2) & 8191;
    AcquireSRWLockExclusive(&socket_index_lock);
    while (socket_index[slot].fd > 0 && socket_index[slot].socket != s) slot = (slot + 1) & 8191;
    socket_index[slot].socket = s; socket_index[slot].fd = fd + 1;
    ReleaseSRWLockExclusive(&socket_index_lock);
}
static lx_fd *indexed_socket(uintptr_t s, int remove)
{
    unsigned slot = (unsigned)(s >> 2) & 8191; int fd = -1;
    AcquireSRWLockExclusive(&socket_index_lock);
    for (unsigned n = 0; n < LX_COUNT(socket_index); n++, slot = (slot + 1) & 8191) {
        if (!socket_index[slot].fd) break;
        if (socket_index[slot].fd > 0 && socket_index[slot].socket == s) {
            fd = socket_index[slot].fd - 1;
            if (remove) socket_index[slot].fd = -1;
            break;
        }
    }
    ReleaseSRWLockExclusive(&socket_index_lock);
    return fd < 0 || remove ? NULL : lx_fd_get(fd);
}
int lx_socket_close(uintptr_t s)
{
    indexed_socket(s, 1);
    return closesocket((SOCKET)s) == SOCKET_ERROR ? net_fail() : 0;
}
int lx_socket_set_nonblock(uintptr_t s, int on)
{
    u_long one = 1;
    return ioctlsocket((SOCKET)s, FIONBIO, &one) == SOCKET_ERROR ? net_fail() : 0;
}
static int install_socket(SOCKET s, int flags)
{
    if (s == INVALID_SOCKET) return net_fail();
    if (lx_socket_set_nonblock(s, 1) < 0) { closesocket(s); return -1; }
    int fd = lx_fd_install_socket(s);
    if (fd < 0) closesocket(s);
    else {
        lx_fd *f = lx_fd_get(fd);
        f->flags = LX_O_RDWR | ((flags & 0x800) ? LX_O_NONBLOCK : 0);
        f->cloexec = !!(flags & 0x80000);
        SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, f->cloexec ? 0 : HANDLE_FLAG_INHERIT);
        index_socket(s, fd);
    }
    return fd;
}
static int g_socket(int family, int type, int protocol)
{
    if (net_init() < 0) return -1;
    int af = host_family(family), base = type & 15;
    if (af < 0 || !af) return lx_set_errno(LX_EAFNOSUPPORT);
    if (type & ~(15 | 0x800 | 0x80000)) return lx_set_errno(LX_EINVAL);
    if (base != SOCK_STREAM && base != SOCK_DGRAM && base != SOCK_RAW) return lx_set_errno(94);
    return install_socket(socket(af, base, protocol), type);
}
/* Wait timeout is SO_*TIMEO in milliseconds; zero means no timeout. */
static int socket_wait(SOCKET s, int writing, int timed)
{
    DWORD timeout = 0;
    int n = sizeof(timeout);
    if (timed) getsockopt(s, SOL_SOCKET, writing ? SO_SNDTIMEO : SO_RCVTIMEO, (char *)&timeout, &n);
    struct timeval tv = { (long)(timeout / 1000), (long)(timeout % 1000) * 1000 };
    fd_set set, err;
    FD_ZERO(&set); FD_ZERO(&err); FD_SET(s, &set); FD_SET(s, &err);
    int r = select(0, writing ? NULL : &set, writing ? &set : NULL, &err, timeout ? &tv : NULL);
    if (r == SOCKET_ERROR) return net_fail();
    if (!r) return lx_set_errno(LX_EAGAIN);
    return 0;
}
static int g_connect(int fd, const void *addr, uint32_t len)
{
    lx_fd *f = socket_fd(fd); SOCKADDR_STORAGE a;
    if (!f || in_addr(addr, len, &a) < 0) return -1;
    if (connect(f->sock, (SOCKADDR *)&a, (int)len) == 0) return 0;
    int e = WSAGetLastError();
    if (e != WSAEWOULDBLOCK && e != WSAEINPROGRESS) return lx_set_errno(net_errno(e));
    if (f->flags & LX_O_NONBLOCK) return lx_set_errno(LX_EINPROGRESS);
    if (socket_wait(f->sock, 1, 1) < 0) return -1;
    int error = 0, n = sizeof(error);
    if (getsockopt(f->sock, SOL_SOCKET, SO_ERROR, (char *)&error, &n) == SOCKET_ERROR) return net_fail();
    return error ? lx_set_errno(net_errno(error)) : 0;
}
static int g_bind(int fd, const void *addr, uint32_t len)
{
    lx_fd *f = socket_fd(fd); SOCKADDR_STORAGE a;
    if (!f || in_addr(addr, len, &a) < 0) return -1;
    return bind(f->sock, (SOCKADDR *)&a, (int)len) == SOCKET_ERROR ? net_fail() : 0;
}
static int g_listen(int fd, int backlog)
{
    lx_fd *f = socket_fd(fd); if (!f) return -1;
    return listen(f->sock, backlog) == SOCKET_ERROR ? net_fail() : 0;
}
static int g_accept4(int fd, void *addr, uint32_t *len, int flags)
{
    lx_fd *f = socket_fd(fd); if (!f) return -1;
    if (flags & ~(0x800 | 0x80000)) return lx_set_errno(LX_EINVAL);
    if (addr && !len) return lx_set_errno(LX_EFAULT);
    for (;;) {
        SOCKADDR_STORAGE a; int n = sizeof(a);
        SOCKET s = accept(f->sock, addr ? (SOCKADDR *)&a : NULL, addr ? &n : NULL);
        if (s != INVALID_SOCKET) {
            int result = install_socket(s, flags);
            if (result >= 0 && addr) out_addr(addr, len, &a, n);
            return result;
        }
        if (WSAGetLastError() != WSAEWOULDBLOCK || (f->flags & LX_O_NONBLOCK)) return net_fail();
        if (socket_wait(f->sock, 0, 1) < 0) return -1;
    }
}
static int g_accept(int fd, void *addr, uint32_t *len) { return g_accept4(fd, addr, len, 0); }
static int g_shutdown(int fd, int how)
{
    lx_fd *f = socket_fd(fd); if (!f) return -1;
    return shutdown(f->sock, how) == SOCKET_ERROR ? net_fail() : 0;
}
static int msg_flags(int flags, int recv_side)
{
    const int known = 1 | 2 | 4 | 0x20 | 0x40 | 0x80 | 0x100 | 0x4000 | 0x8000 | 0x40000000;
    if (flags & ~known) return lx_set_errno(LX_EOPNOTSUPP);
    if (!recv_side && (flags & (2 | 0x100))) return lx_set_errno(LX_EOPNOTSUPP);
    return (flags & 1 ? MSG_OOB : 0) | (flags & 2 ? MSG_PEEK : 0) | (flags & 4 ? MSG_DONTROUTE : 0);
}
static long socket_io(lx_fd *f, void *buf, unsigned long size, int flags, int writing, const void *to, uint32_t tolen, void *from, uint32_t *fromlen)
{
    int native_flags = msg_flags(flags, !writing);
    if (native_flags < 0) return -1;
    if (size > INT_MAX) size = INT_MAX;
    SOCKADDR_STORAGE a;
    if (to && in_addr(to, tolen, &a) < 0) return -1;
    if (from && !fromlen) return lx_set_errno(LX_EFAULT);
    unsigned long total = 0;
    int type = 0, typelen = sizeof(type);
    if (!writing && (flags & 0x100)) getsockopt(f->sock, SOL_SOCKET, SO_TYPE, (char *)&type, &typelen);
    for (;;) {
        int n = sizeof(a);
        u_long datagram_size = 0;
        if (!writing && (flags & 0x20)) ioctlsocket(f->sock, FIONREAD, &datagram_size);
        int r = writing ? (to ? sendto(f->sock, (char *)buf + total, (int)(size - total), native_flags, (SOCKADDR *)&a, tolen)
                             : send(f->sock, (char *)buf + total, (int)(size - total), native_flags))
                        : recvfrom(f->sock, (char *)buf + total, (int)(size - total), native_flags, from ? (SOCKADDR *)&a : NULL, from ? &n : NULL);
        if (r == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (!writing && e == WSAEMSGSIZE) {
                if (from) out_addr(from, fromlen, &a, n);
                return (flags & 0x20) && datagram_size > size ? datagram_size : (long)size;
            }
            if (e != WSAEWOULDBLOCK || ((f->flags & LX_O_NONBLOCK) || (flags & 0x40)))
                return total ? (long)total : lx_set_errno(net_errno(e));
            if (socket_wait(f->sock, writing, 1) < 0) return total ? (long)total : -1;
            continue;
        }
        if (from) out_addr(from, fromlen, &a, n);
        total += r;
        if (writing || !r || total == size || !(flags & 0x100) || type != SOCK_STREAM || (flags & 2)) return (long)total;
    }
}
long lx_socket_read(uintptr_t s, void *buf, unsigned long n)
{
    lx_fd *f = indexed_socket(s, 0);
    return f ? socket_io(f, buf, n, 0, 0, NULL, 0, NULL, NULL) : lx_set_errno(LX_EBADF);
}
long lx_socket_write(uintptr_t s, const void *buf, unsigned long n)
{
    lx_fd *f = indexed_socket(s, 0);
    return f ? socket_io(f, (void *)buf, n, 0, 1, NULL, 0, NULL, NULL) : lx_set_errno(LX_EBADF);
}
static long g_send(int fd, const void *buf, unsigned long n, int flags)
{ lx_fd *f = socket_fd(fd); return f ? socket_io(f, (void *)buf, n, flags, 1, NULL, 0, NULL, NULL) : -1; }
static long g_recv(int fd, void *buf, unsigned long n, int flags)
{ lx_fd *f = socket_fd(fd); return f ? socket_io(f, buf, n, flags, 0, NULL, 0, NULL, NULL) : -1; }
static long g_recv_chk(int fd, void *buf, unsigned long n, unsigned long buflen, int flags)
{
    if (n > buflen) lx_chk_fail();
    return g_recv(fd, buf, n, flags);
}
static long g_sendto(int fd, const void *buf, unsigned long n, int flags, const void *addr, uint32_t len)
{ lx_fd *f = socket_fd(fd); return f ? socket_io(f, (void *)buf, n, flags, 1, addr, len, NULL, NULL) : -1; }
static long g_recvfrom(int fd, void *buf, unsigned long n, int flags, void *addr, uint32_t *len)
{ lx_fd *f = socket_fd(fd); return f ? socket_io(f, buf, n, flags, 0, NULL, 0, addr, len) : -1; }
static long socket_message(int fd, g_msghdr *msg, int flags, int writing)
{
    lx_fd *f = socket_fd(fd); if (!f) return -1;
    if (!msg || (msg->iovlen && !msg->iov)) return lx_set_errno(LX_EFAULT);
    if (msg->iovlen > 1024) return lx_set_errno(90);
    if (writing && msg->controllen) return lx_set_errno(LX_EOPNOTSUPP);
    int native_flags = msg_flags(flags, !writing); if (native_flags < 0) return -1;
    WSABUF local[16], *vec = msg->iovlen > 16 ? malloc(msg->iovlen * sizeof(*vec)) : local;
    if (!vec) return lx_set_errno(LX_ENOMEM);
    uint64_t total = 0;
    for (unsigned i = 0; i < msg->iovlen; i++) { vec[i].buf = msg->iov[i].base; vec[i].len = msg->iov[i].len; total += vec[i].len; }
    if (total > INT_MAX) { if (vec != local) free(vec); return lx_set_errno(LX_EINVAL); }
    SOCKADDR_STORAGE a;
    if (writing && msg->name && in_addr(msg->name, msg->namelen, &a) < 0) { if (vec != local) free(vec); return -1; }
    long result = 0;
    unsigned first = 0;
    int type = 0, type_len = sizeof(type);
    if (!writing && (flags & 0x100)) getsockopt(f->sock, SOL_SOCKET, SO_TYPE, (char *)&type, &type_len);
    int waitall = !writing && (flags & 0x100) && !(flags & 2) && type == SOCK_STREAM;
    for (;;) {
        DWORD done = 0, nf = native_flags; int n = sizeof(a);
        u_long pending = 0;
        if (!writing && (flags & 0x20)) ioctlsocket(f->sock, FIONREAD, &pending);
        WSABUF empty = {0};
        WSABUF *current = msg->iovlen ? vec + first : &empty;
        DWORD remaining = msg->iovlen ? msg->iovlen - first : 1;
        int r = writing ? WSASendTo(f->sock, current, remaining, &done, nf, msg->name ? (SOCKADDR *)&a : NULL, msg->name ? msg->namelen : 0, NULL, NULL)
                        : WSARecvFrom(f->sock, current, remaining, &done, &nf, msg->name ? (SOCKADDR *)&a : NULL, msg->name ? &n : NULL, NULL, NULL);
        int e = r == SOCKET_ERROR ? WSAGetLastError() : 0;
        if (!e || (!writing && e == WSAEMSGSIZE)) {
            result += (long)done;
            if (!writing) {
                if (msg->name) out_addr(msg->name, &msg->namelen, &a, n);
                msg->controllen = 0;
                msg->flags = (nf & MSG_OOB ? 1 : 0) | (e == WSAEMSGSIZE ? 0x20 : 0);
                if (e == WSAEMSGSIZE && (flags & 0x20) && pending > done) result = pending;
            }
            if (!waitall || !done || (uint64_t)result >= total) break;
            while (first < msg->iovlen && done >= vec[first].len) { done -= vec[first].len; first++; }
            if (first < msg->iovlen) { vec[first].buf += done; vec[first].len -= done; }
            continue;
        }
        if (e != WSAEWOULDBLOCK || (f->flags & LX_O_NONBLOCK) || (flags & 0x40)) { if (!result) result = lx_set_errno(net_errno(e)); break; }
        if (socket_wait(f->sock, writing, 1) < 0) { if (!result) result = -1; break; }
    }
    if (vec != local) free(vec);
    return result;
}
static long g_sendmsg(int fd, g_msghdr *msg, int flags) { return socket_message(fd, msg, flags, 1); }
static long g_recvmsg(int fd, g_msghdr *msg, int flags) { return socket_message(fd, msg, flags, 0); }
static int sock_option(int *level, int option)
{
    if (*level == 1) {
        *level = SOL_SOCKET;
        switch (option) {
        case 1: return SO_DEBUG; case 2: return SO_REUSEADDR; case 3: return SO_TYPE;
        case 4: return SO_ERROR; case 5: return SO_DONTROUTE; case 6: return SO_BROADCAST;
        case 7: return SO_SNDBUF; case 8: return SO_RCVBUF; case 9: return SO_KEEPALIVE;
        case 10: return SO_OOBINLINE; case 13: return SO_LINGER; case 15: return SO_REUSEADDR;
        case 20: return SO_RCVTIMEO; case 21: return SO_SNDTIMEO; case 30: return SO_ACCEPTCONN;
        default: return -1;
        }
    }
    if (*level == IPPROTO_TCP) return option == 1 ? TCP_NODELAY : -1;
    if (*level == IPPROTO_IP) {
        switch (option) {
        case 1: return IP_TOS; case 2: return IP_TTL; case 32: return IP_MULTICAST_IF;
        case 33: return IP_MULTICAST_TTL; case 34: return IP_MULTICAST_LOOP;
        case 35: return IP_ADD_MEMBERSHIP; case 36: return IP_DROP_MEMBERSHIP;
        default: return -1;
        }
    }
    if (*level == 41) { *level = IPPROTO_IPV6; return option == 26 ? IPV6_V6ONLY : -1; }
    return -1;
}
static int g_setsockopt(int fd, int level, int option, const void *value, uint32_t len)
{
    lx_fd *f = socket_fd(fd); if (!f) return -1;
    int native = sock_option(&level, option); if (native < 0) return lx_set_errno(92);
    if (!value) return lx_set_errno(LX_EFAULT);
    DWORD timeout; struct linger linger;
    if (level == SOL_SOCKET && (native == SO_RCVTIMEO || native == SO_SNDTIMEO)) {
        if (len < sizeof(g_timeval)) return lx_set_errno(LX_EINVAL);
        const g_timeval *v = value;
        if (v->sec < 0 || v->usec < 0 || v->usec >= 1000000) return lx_set_errno(LX_EDOM);
        uint64_t ms = (uint64_t)v->sec * 1000 + (v->usec + 999) / 1000;
        timeout = ms > MAXDWORD ? MAXDWORD : (DWORD)ms; value = &timeout; len = sizeof(timeout);
    } else if (level == SOL_SOCKET && native == SO_LINGER) {
        if (len < 8) return lx_set_errno(LX_EINVAL);
        linger.l_onoff = (u_short)((const int *)value)[0]; linger.l_linger = (u_short)((const int *)value)[1];
        value = &linger; len = sizeof(linger);
    }
    return setsockopt(f->sock, level, native, value, len) == SOCKET_ERROR ? net_fail() : 0;
}
static int g_getsockopt(int fd, int level, int option, void *value, uint32_t *len)
{
    lx_fd *f = socket_fd(fd); if (!f) return -1;
    if (!len || (!value && *len)) return lx_set_errno(LX_EFAULT);
    int native = sock_option(&level, option); if (native < 0) return lx_set_errno(92);
    unsigned char storage[16]; void *target = value; int n = (int)*len;
    if (level == SOL_SOCKET && (native == SO_ERROR || native == SO_RCVTIMEO || native == SO_SNDTIMEO || native == SO_LINGER)) { target = storage; n = sizeof(storage); }
    if (getsockopt(f->sock, level, native, target, &n) == SOCKET_ERROR) return net_fail();
    if (target == storage) {
        int out[2]; int size = 4;
        if (native == SO_ERROR) out[0] = net_errno(*(int *)storage);
        else if (native == SO_LINGER) { out[0] = ((struct linger *)storage)->l_onoff; out[1] = ((struct linger *)storage)->l_linger; size = 8; }
        else { out[0] = *(DWORD *)storage / 1000; out[1] = (*(DWORD *)storage % 1000) * 1000; size = 8; }
        if (*len) memcpy(value, out, *len < (uint32_t)size ? *len : (uint32_t)size);
        *len = *len < (uint32_t)size ? *len : (uint32_t)size;
    } else *len = n;
    return 0;
}
static int socket_name(int fd, void *addr, uint32_t *len, int peer)
{
    lx_fd *f = socket_fd(fd); if (!f) return -1;
    if (!addr || !len) return lx_set_errno(LX_EFAULT);
    SOCKADDR_STORAGE a; int n = sizeof(a);
    int r = peer ? getpeername(f->sock, (SOCKADDR *)&a, &n) : getsockname(f->sock, (SOCKADDR *)&a, &n);
    if (r == SOCKET_ERROR) return net_fail();
    out_addr(addr, len, &a, n); return 0;
}
static int g_getsockname(int fd, void *addr, uint32_t *len) { return socket_name(fd, addr, len, 0); }
static int g_getpeername(int fd, void *addr, uint32_t *len) { return socket_name(fd, addr, len, 1); }
static int g_socketpair(int family, int type, int protocol, int *fds) { return lx_set_errno(LX_ENOSYS); }

static int bit_test(const uint32_t *set, int fd) { return set && (set[fd / 32] & (1u << (fd % 32))); }
static void bit_set(uint32_t *set, int fd) { if (set) set[fd / 32] |= 1u << (fd % 32); }
static int g_select(int count, uint32_t *readset, uint32_t *writeset, uint32_t *exceptset, g_timeval *timeout)
{
    if (count < 0 || count > 1024) return lx_set_errno(LX_EINVAL);
    if (timeout && (timeout->sec < 0 || timeout->usec < 0 || timeout->usec >= 1000000)) return lx_set_errno(LX_EINVAL);
    uint32_t in[3][32], out[3][32] = {{0}};
    uint32_t *sets[3] = {readset, writeset, exceptset};
    fd_set native[3]; for (int j = 0; j < 3; j++) { FD_ZERO(&native[j]); if (sets[j]) memcpy(in[j], sets[j], 128); else memset(in[j], 0, 128); }
    int immediate = 0, sockets = 0;
    for (int fd = 0; fd < count; fd++) {
        int used = bit_test(in[0], fd) || bit_test(in[1], fd) || bit_test(in[2], fd);
        if (!used) continue;
        lx_fd *f = lx_fd_get(fd); if (!f) return -1;
        if (f->kind == LX_FD_SOCKET) {
            sockets++;
            for (int j = 0; j < 3; j++) if (bit_test(in[j], fd)) FD_SET(f->sock, &native[j]);
        } else {
            for (int j = 0; j < 2; j++) if (bit_test(in[j], fd)) { bit_set(out[j], fd); immediate++; }
        }
    }
    ULONGLONG start = GetTickCount64();
    uint64_t ms = timeout ? (uint64_t)timeout->sec * 1000 + (timeout->usec + 999) / 1000 : UINT64_MAX;
    struct timeval tv = {0,0}, *ptv = NULL;
    if (immediate) ptv = &tv;
    else if (timeout) { tv.tv_sec = timeout->sec; tv.tv_usec = timeout->usec; ptv = &tv; }
    int result = 0;
    if (sockets) {
        if (net_init() < 0) return -1;
        result = select(0, &native[0], &native[1], &native[2], ptv);
        if (result == SOCKET_ERROR) return net_fail();
    } else if (!immediate) {
        if (ms == UINT64_MAX) Sleep(INFINITE);
        else while (ms > 0) { DWORD part = ms > MAXDWORD - 1 ? MAXDWORD - 1 : (DWORD)ms; Sleep(part); ms -= part; }
    }
    result = immediate;
    for (int fd = 0; fd < count; fd++) {
        if (!bit_test(in[0], fd) && !bit_test(in[1], fd) && !bit_test(in[2], fd)) continue;
        lx_fd *f = lx_fd_get(fd);
        if (!f || f->kind != LX_FD_SOCKET) continue;
        for (int j = 0; j < 3; j++) if (bit_test(in[j], fd) && FD_ISSET(f->sock, &native[j])) { bit_set(out[j], fd); result++; }
    }
    for (int j = 0; j < 3; j++) if (sets[j]) memcpy(sets[j], out[j], 128);
    if (timeout) {
        uint64_t original = (uint64_t)timeout->sec * 1000000 + timeout->usec;
        uint64_t spent = (GetTickCount64() - start) * 1000;
        uint64_t remain = spent < original ? original - spent : 0;
        timeout->sec = (int32_t)(remain / 1000000); timeout->usec = remain % 1000000;
    }
    return result;
}
static int file_poll(lx_fd *f, short events)
{
    int ready = events & 4;
    if (!(events & 1)) return ready;
    if (f->kind == LX_FD_PIPE) {
        DWORD bytes;
        if (PeekNamedPipe(f->h, NULL, 0, NULL, &bytes, NULL)) return ready | (bytes ? 1 : 0);
        return ready | (GetLastError() == ERROR_BROKEN_PIPE ? 0x10 : 8);
    }
    return ready | 1;
}
static int g_poll(g_pollfd *fds, unsigned long count, int timeout)
{
    if (count > 4096) return lx_set_errno(LX_EINVAL);
    if (count && !fds) return lx_set_errno(LX_EFAULT);
    WSAPOLLFD local_native[32], *native = local_native;
    unsigned local_mapping[32], *mapping = local_mapping;
    if (count > LX_COUNT(local_native)) {
        native = malloc(count * (sizeof(*native) + sizeof(*mapping)));
        if (!native) return lx_set_errno(LX_ENOMEM);
        mapping = (unsigned *)(native + count);
    }
    ULONGLONG start = GetTickCount64();
    int answer = 0;
    for (;;) {
        unsigned sockets = 0; int pipes = 0;
        fd_set priority; FD_ZERO(&priority);
        answer = 0;
        for (unsigned i = 0; i < count; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;
            int saved = lx_errno; lx_fd *f = lx_fd_get(fds[i].fd); lx_errno = saved;
            if (!f) fds[i].revents = 0x20;
            else if (f->kind == LX_FD_SOCKET) {
                native[sockets].fd = f->sock;
                native[sockets].events = (fds[i].events & 1 ? POLLRDNORM : 0) | (fds[i].events & 4 ? POLLWRNORM : 0);
                if (fds[i].events & 2) FD_SET(f->sock, &priority);
                native[sockets].revents = 0; mapping[sockets++] = i;
            } else { fds[i].revents = (short)file_poll(f, fds[i].events); pipes |= f->kind == LX_FD_PIPE; }
            if (fds[i].revents) answer++;
        }
        uint64_t elapsed = GetTickCount64() - start;
        int left = timeout < 0 ? -1 : elapsed >= (uint64_t)timeout ? 0 : timeout - (int)elapsed;
        int slice = answer ? 0 : (pipes || priority.fd_count) && (left < 0 || left > 10) ? 10 : left;
        int n = 0;
        if (sockets) { if (net_init() < 0) { answer = -1; break; } n = WSAPoll(native, sockets, slice); }
        else if (!answer && slice) Sleep(slice < 0 ? INFINITE : slice);
        if (n == SOCKET_ERROR) { answer = net_fail(); break; }
        /* The Microsoft WSAPoll provider rejects POLLPRI; its select except set supports OOB. */
        if (priority.fd_count) {
            struct timeval zero = {0,0};
            if (select(0, NULL, NULL, &priority, &zero) == SOCKET_ERROR) { answer = net_fail(); break; }
        }
        for (unsigned j = 0; j < sockets; j++) {
            short r = native[j].revents;
            short g = (r & (POLLRDNORM | POLLRDBAND) ? 1 : 0) | (FD_ISSET(native[j].fd, &priority) ? 2 : 0) | (r & POLLWRNORM ? 4 : 0) | (r & POLLERR ? 8 : 0) | (r & POLLHUP ? 0x10 : 0) | (r & POLLNVAL ? 0x20 : 0);
            fds[mapping[j]].revents = g;
            if (g) answer++;
        }
        if (answer || timeout == 0 || (timeout > 0 && GetTickCount64() - start >= (uint64_t)timeout)) break;
    }
    if (native != local_native) free(native);
    return answer;
}
static long g_fdelt_chk(long fd)
{
    if (fd < 0 || fd >= 1024) lx_fatal("__fdelt_chk: fd %ld outside fd_set", fd);
    return fd / 32;
}

static __declspec(thread) int host_error;
static __declspec(thread) struct {
    g_hostent host; char *aliases[65], *addresses[65]; char buffer[16384];
} host_store;
static int *g_h_errno_location(void) { return &host_error; }
static int resolver_error(int error)
{
    switch (error) { case WSAHOST_NOT_FOUND: return 1; case WSATRY_AGAIN: return 2; case WSANO_RECOVERY: return 3; case WSANO_DATA: return 4; default: return 3; }
}
static char *host_begin(const char *name, int family)
{
    size_t n = strlen(name) + 1;
    if (n + 64 * 16 > sizeof(host_store.buffer)) { host_error = 3; return NULL; }
    memcpy(host_store.buffer, name, n);
    host_store.host.name = host_store.buffer;
    host_store.host.aliases = host_store.aliases; host_store.aliases[0] = NULL;
    host_store.host.addresses = host_store.addresses;
    host_store.host.type = family; host_store.host.length = family == 2 ? 4 : 16;
    host_error = 0;
    return host_store.buffer + n;
}
static g_hostent *g_gethostbyname(const char *name)
{
    if (net_init() < 0) { host_error = 3; return NULL; }
    if (!name) { host_error = 1; return NULL; }
    ADDRINFOA hints = {0}, *list = NULL;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_CANONNAME;
    int error = getaddrinfo(name, NULL, &hints, &list);
    if (error) { host_error = resolver_error(error); return NULL; }
    char *storage = host_begin(list && list->ai_canonname ? list->ai_canonname : name, 2);
    if (!storage) { freeaddrinfo(list); return NULL; }
    unsigned count = 0;
    for (ADDRINFOA *a = list; a && count < 64; a = a->ai_next) {
        if (a->ai_family != AF_INET) continue;
        const void *address = &((SOCKADDR_IN *)a->ai_addr)->sin_addr;
        unsigned i;
        for (i = 0; i < count; i++) if (!memcmp(host_store.addresses[i], address, 4)) break;
        if (i < count) continue;
        host_store.addresses[count++] = storage; memcpy(storage, address, 4); storage += 4;
    }
    host_store.addresses[count] = NULL; freeaddrinfo(list);
    if (!count) { host_error = 4; return NULL; }
    return &host_store.host;
}
static g_hostent *g_gethostbyaddr(const void *addr, uint32_t len, int family)
{
    if (net_init() < 0) { host_error = 3; return NULL; }
    int af = host_family(family);
    if ((af != AF_INET && af != AF_INET6) || len != (af == AF_INET ? 4u : 16u)) { host_error = 3; lx_set_errno(LX_EINVAL); return NULL; }
    SOCKADDR_STORAGE socket_addr = {0}; socket_addr.ss_family = (ADDRESS_FAMILY)af;
    if (af == AF_INET) memcpy(&((SOCKADDR_IN *)&socket_addr)->sin_addr, addr, 4);
    else memcpy(&((SOCKADDR_IN6 *)&socket_addr)->sin6_addr, addr, 16);
    char name[1025];
    int error = getnameinfo((SOCKADDR *)&socket_addr, af == AF_INET ? 16 : 28, name, sizeof(name), NULL, 0, NI_NAMEREQD);
    if (error) { host_error = resolver_error(error); return NULL; }
    char *storage = host_begin(name, family); if (!storage) return NULL;
    memcpy(storage, addr, len); host_store.addresses[0] = storage; host_store.addresses[1] = NULL;
    return &host_store.host;
}
static int g_gethostbyname_r(const char *name, g_hostent *ret, char *buf, unsigned len, g_hostent **result, int *err)
{
    if (!ret || !buf || !result || !err) return LX_EINVAL;
    *result = NULL;
    g_hostent *h = g_gethostbyname(name); *err = host_error;
    if (!h) return 0;
    unsigned aliases = 0, addresses = 0;
    while (h->aliases[aliases]) aliases++;
    while (h->addresses[addresses]) addresses++;
    uintptr_t aligned = ((uintptr_t)buf + 3) & ~(uintptr_t)3;
    size_t pointer_bytes = (aliases + addresses + 2) * sizeof(char *);
    size_t required = aligned - (uintptr_t)buf + pointer_bytes + strlen(h->name) + 1 + addresses * h->length;
    for (unsigned i = 0; i < aliases; i++) required += strlen(h->aliases[i]) + 1;
    if (required > len) { *err = -1; lx_set_errno(LX_ERANGE); return LX_ERANGE; }
    *ret = *h; ret->aliases = (char **)aligned; ret->addresses = ret->aliases + aliases + 1;
    char *p = (char *)(ret->addresses + addresses + 1);
    ret->name = p; size_t n = strlen(h->name) + 1; memcpy(p, h->name, n); p += n;
    for (unsigned i = 0; i < aliases; i++) { ret->aliases[i] = p; n = strlen(h->aliases[i]) + 1; memcpy(p, h->aliases[i], n); p += n; }
    ret->aliases[aliases] = NULL;
    for (unsigned i = 0; i < addresses; i++) { ret->addresses[i] = p; memcpy(p, h->addresses[i], h->length); p += h->length; }
    ret->addresses[addresses] = NULL; *err = 0; *result = ret; return 0;
}
static int gai_error(int e)
{
    switch (e) {
    case 0: return 0; case EAI_AGAIN: return -3; case EAI_BADFLAGS: return -1;
    case EAI_FAIL: return -4; case EAI_FAMILY: return -6; case EAI_MEMORY: return -10;
    case EAI_NONAME: return -2; case EAI_SERVICE: return -8; case EAI_SOCKTYPE: return -7;
    default: return -4;
    }
}
static int ai_flags_in(int g)
{
    return (g & 1 ? AI_PASSIVE : 0) | (g & 2 ? AI_CANONNAME : 0) | (g & 4 ? AI_NUMERICHOST : 0) |
           (g & 8 ? AI_V4MAPPED : 0) | (g & 16 ? AI_ALL : 0) | (g & 32 ? AI_ADDRCONFIG : 0) | (g & 0x400 ? AI_NUMERICSERV : 0);
}
static void g_freeaddrinfo(g_addrinfo *list)
{ while (list) { g_addrinfo *next = list->next; lx_free(list); list = next; } }
static int g_getaddrinfo(const char *name, const char *service, const g_addrinfo *hints, g_addrinfo **result)
{
    if (!result) return -11;
    *result = NULL;
    if (net_init() < 0) return -11;
    ADDRINFOA h = {0}, *native = NULL;
    if (hints) {
        if (hints->flags & ~(1 | 2 | 4 | 8 | 16 | 32 | 0x400)) return -1;
        h.ai_flags = ai_flags_in(hints->flags); h.ai_family = host_family(hints->family);
        if (h.ai_family < 0) return -6;
        h.ai_socktype = hints->socktype; h.ai_protocol = hints->protocol;
    }
    int e = getaddrinfo(name, service, hints ? &h : NULL, &native);
    if (e) return gai_error(e);
    g_addrinfo **tail = result;
    for (ADDRINFOA *a = native; a; a = a->ai_next) {
        size_t cn = a->ai_canonname ? strlen(a->ai_canonname) + 1 : 0;
        g_addrinfo *g = lx_calloc(1, sizeof(*g) + a->ai_addrlen + cn);
        if (!g) { freeaddrinfo(native); g_freeaddrinfo(*result); *result = NULL; return -10; }
        g->flags = hints ? hints->flags : 0; g->family = guest_family(a->ai_family);
        g->socktype = a->ai_socktype; g->protocol = a->ai_protocol; g->addrlen = a->ai_addrlen;
        g->addr = (g_sockaddr *)(g + 1); uint32_t n = g->addrlen; out_addr(g->addr, &n, a->ai_addr, a->ai_addrlen);
        if (cn) { g->canonname = (char *)g->addr + g->addrlen; memcpy(g->canonname, a->ai_canonname, cn); }
        *tail = g; tail = &g->next;
    }
    freeaddrinfo(native); return 0;
}
static const char *g_gai_strerror(int e)
{
    switch (e) { case 0: return "Unknown error"; case -1: return "Bad value for ai_flags";
    case -2: return "Name or service not known"; case -3: return "Temporary failure in name resolution";
    case -4: return "Non-recoverable failure in name resolution"; case -6: return "ai_family not supported";
    case -7: return "ai_socktype not supported"; case -8: return "Servname not supported for ai_socktype";
    case -10: return "Memory allocation failure"; case -11: return "System error"; case -12: return "Argument buffer too small";
    default: return "Unknown error"; }
}
static int g_getnameinfo(const void *addr, uint32_t len, char *host, uint32_t hostlen, char *serv, uint32_t servlen, int flags)
{
    if (net_init() < 0) return -11;
    if (flags & ~31) return -1;
    SOCKADDR_STORAGE a; if (in_addr(addr, len, &a) < 0) return -6;
    int nf = (flags & 1 ? NI_NUMERICHOST : 0) | (flags & 2 ? NI_NUMERICSERV : 0) |
             (flags & 4 ? NI_NOFQDN : 0) | (flags & 8 ? NI_NAMEREQD : 0) | (flags & 16 ? NI_DGRAM : 0);
    int r = getnameinfo((SOCKADDR *)&a, len, host, hostlen, serv, servlen, nf);
    return r == WSAEFAULT ? -12 : gai_error(r);
}
static int g_inet_aton(const char *s, void *out)
{
    if (!s) return 0;
    uint32_t parts[4], value; unsigned count = 0;
    const unsigned char *p = (const unsigned char *)s;
    for (;;) {
        if (*p < '0' || *p > '9') return 0;
        unsigned base = 10, digits = 0; value = 0;
        if (*p == '0') { base = 8; p++; digits = 1; if (*p == 'x' || *p == 'X') { base = 16; p++; digits = 0; } }
        for (;;) {
            unsigned digit = *p >= '0' && *p <= '9' ? *p - '0' :
                             *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 :
                             *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : 255;
            if (digit >= base) break;
            if (value > (UINT32_MAX - digit) / base) return 0;
            value = value * base + digit; digits++; p++;
        }
        if (!digits) return 0;
        if (*p != '.') break;
        if (count == 3 || value > 255) return 0;
        parts[count++] = value; p++;
    }
    if (*p && *p != ' ' && (*p < 9 || *p > 13)) return 0;
    if ((count == 1 && value > 0xffffff) || (count == 2 && value > 0xffff) || (count == 3 && value > 255)) return 0;
    for (unsigned i = 0; i < count; i++) value |= parts[i] << (24 - i * 8);
    value = _byteswap_ulong(value); memcpy(out, &value, 4); return 1;
}
static unsigned long g_inet_addr(const char *s)
{
    uint32_t addr; return g_inet_aton(s, &addr) ? addr : UINT32_MAX;
}
static char *g_inet_ntoa(uint32_t addr)
{
    static __declspec(thread) char text[16];
    IN_ADDR a; a.s_addr = addr;
    if (net_init() < 0) return NULL;
    if (!InetNtopA(AF_INET, &a, text, sizeof(text))) { net_fail(); return NULL; }
    return text;
}
static const char *g_inet_ntop(int family, const void *src, char *dst, uint32_t len)
{
    if (net_init() < 0) return NULL;
    int af = host_family(family);
    if (af != AF_INET && af != AF_INET6) { lx_set_errno(LX_EAFNOSUPPORT); return NULL; }
    const char *r = InetNtopA(af, (void *)src, dst, len);
    if (!r) lx_set_errno(WSAGetLastError() == WSAEFAULT ? LX_ENOSPC : net_errno(WSAGetLastError()));
    return r;
}
static int g_inet_pton(int family, const char *src, void *dst)
{
    if (net_init() < 0) return -1;
    int af = host_family(family);
    if (af != AF_INET && af != AF_INET6) return lx_set_errno(LX_EAFNOSUPPORT);
    int r = InetPtonA(af, src, dst); return r < 0 ? net_fail() : r;
}
static uint16_t g_htons(uint16_t n) { return (uint16_t)((n >> 8) | (n << 8)); }
static uint32_t g_htonl(uint32_t n) { return _byteswap_ulong(n); }
static int g_gethostname(char *name, unsigned len)
{
    if (net_init() < 0) return -1;
    char tmp[256]; if (gethostname(tmp, sizeof(tmp)) == SOCKET_ERROR) return net_fail();
    size_t need = strlen(tmp) + 1; if (need > len) { if (len) memcpy(name, tmp, len); return lx_set_errno(LX_ENAMETOOLONG); }
    memcpy(name, tmp, need); return 0;
}
static g_servent *g_getservbyname(const char *name, const char *proto)
{
    static __declspec(thread) g_servent result;
    static __declspec(thread) char text[256];
    static __declspec(thread) char *aliases[1];
    if (net_init() < 0) return NULL;
    struct servent *s = getservbyname(name, proto); if (!s) return NULL;
    size_t n = strlen(s->s_name) + 1, p = strlen(s->s_proto) + 1;
    if (n + p > sizeof(text)) return NULL;
    memcpy(text, s->s_name, n); memcpy(text + n, s->s_proto, p);
    aliases[0] = NULL; result.name = text; result.aliases = aliases; result.port = s->s_port; result.proto = text + n;
    return &result;
}

typedef struct if_node {
    g_ifaddrs g;
    SOCKADDR_STORAGE addr, mask, broad;
    char name[32];
    unsigned index, mtu, maclen;
    unsigned char mac[8];
} if_node;
static_assert(offsetof(if_node, g) == 0, "ifaddrs allocation prefix");
static void g_freeifaddrs(g_ifaddrs *p) { while (p) { g_ifaddrs *next = p->next; lx_free(p); p = next; } }
static int add_interface(g_ifaddrs ***tail, const char *name, unsigned flags, const SOCKADDR *addr, unsigned len, unsigned prefix, unsigned index, unsigned mtu, const unsigned char *mac, unsigned maclen)
{
    if_node *p = lx_calloc(1, sizeof(*p)); if (!p) return -1;
    strncpy(p->name, name, sizeof(p->name) - 1); p->g.name = p->name; p->g.flags = flags;
    p->index = index; p->mtu = mtu; p->maclen = maclen > 8 ? 8 : maclen; if (mac) memcpy(p->mac, mac, p->maclen);
    uint32_t n = len; out_addr(&p->addr, &n, addr, len);
    p->g.addr = (g_sockaddr *)&p->addr; p->g.netmask = (g_sockaddr *)&p->mask;
    p->mask.ss_family = (ADDRESS_FAMILY)guest_family(addr->sa_family);
    unsigned bytes = addr->sa_family == AF_INET ? 4 : 16;
    unsigned char *mask = addr->sa_family == AF_INET ? (unsigned char *)&((g_in *)&p->mask)->addr : ((g_in6 *)&p->mask)->addr;
    if (prefix > bytes * 8) prefix = bytes * 8;
    for (unsigned i = 0; i < bytes; i++) { unsigned bits = prefix >= 8 ? 8 : prefix; mask[i] = bits ? (unsigned char)(0xffu << (8 - bits)) : 0; prefix -= bits; }
    if (addr->sa_family == AF_INET && !(flags & 8)) {
        p->broad = p->addr; ((g_in *)&p->broad)->addr |= ~((g_in *)&p->mask)->addr;
        p->g.broadaddr = (g_sockaddr *)&p->broad;
    }
    **tail = &p->g; *tail = &p->g.next; return 0;
}
/* Adapter snapshot shared by getifaddrs and the synthesized /proc/net/route and
 * /sys/class/net files, so every view names the same adapter eth0. */
static IP_ADAPTER_ADDRESSES *query_adapters(ULONG *status)
{
    ULONG size = 16384; IP_ADAPTER_ADDRESSES *buf = NULL;
    for (int attempt = 0; attempt < 4; attempt++) {
        free(buf); buf = malloc(size); if (!buf) { *status = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
        *status = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS, NULL, buf, &size);
        if (*status != ERROR_BUFFER_OVERFLOW) break;
    }
    if (*status != NO_ERROR) { free(buf); return NULL; }
    return buf;
}
static const SOCKADDR_IN *adapter_ipv4(const IP_ADAPTER_ADDRESSES *a)
{
    for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next)
        if (u->Address.lpSockaddr->sa_family == AF_INET) return (const SOCKADDR_IN *)u->Address.lpSockaddr;
    return NULL;
}
static const SOCKADDR_IN *adapter_gateway4(const IP_ADAPTER_ADDRESSES *a)
{
    for (IP_ADAPTER_GATEWAY_ADDRESS_LH *g = a->FirstGatewayAddress; g; g = g->Next)
        if (g->Address.lpSockaddr->sa_family == AF_INET) return (const SOCKADDR_IN *)g->Address.lpSockaddr;
    return NULL;
}
/* eth0 = the up adapter carrying an IPv4 default gateway with the lowest interface metric (the
 * one Windows routes through; VPN/TUN point-to-point adapters have no gateway); otherwise the
 * first up adapter with IPv4, otherwise any adapter with IPv4. */
static IP_ADAPTER_ADDRESSES *primary_adapter(IP_ADAPTER_ADDRESSES *buf)
{
    IP_ADAPTER_ADDRESSES *best = NULL; int best_rank = 0;
    for (IP_ADAPTER_ADDRESSES *a = buf; a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || !adapter_ipv4(a)) continue;
        int up = a->OperStatus == IfOperStatusUp;
        int rank = up && adapter_gateway4(a) ? 3 : up ? 2 : 1;
        if (rank > best_rank || (rank == best_rank && rank == 3 && a->Ipv4Metric < best->Ipv4Metric)) { best = a; best_rank = rank; }
    }
    return best;
}
static void adapter_name(const IP_ADAPTER_ADDRESSES *buf, const IP_ADAPTER_ADDRESSES *primary, const IP_ADAPTER_ADDRESSES *a, char name[32])
{
    if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) { strcpy(name, "lo"); return; }
    if (a == primary) { strcpy(name, "eth0"); return; }
    unsigned eth = 0;
    for (const IP_ADAPTER_ADDRESSES *p = buf; p && p != a; p = p->Next)
        if (p != primary && p->IfType != IF_TYPE_SOFTWARE_LOOPBACK) eth++;
    snprintf(name, 32, "eth%u", eth + 1);
}
static int g_getifaddrs(g_ifaddrs **result)
{
    if (!result) return lx_set_errno(LX_EFAULT);
    *result = NULL;
    if (net_init() < 0) return -1;
    ULONG status; IP_ADAPTER_ADDRESSES *buf = query_adapters(&status);
    if (!buf && status != ERROR_NO_DATA) return lx_set_errno(lx_errno_from_win32(status));
    g_ifaddrs **tail = result; int loop4 = 0;
    IP_ADAPTER_ADDRESSES *primary = buf ? primary_adapter(buf) : NULL;
    /* glibc order: every IPv4 entry, then every IPv6 entry */
    for (int pass = 0; buf && pass < 2; pass++) for (IP_ADAPTER_ADDRESSES *a = buf; a; a = a->Next) {
        int loop = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        char name[32]; adapter_name(buf, primary, a, name);
        unsigned flags = (a->OperStatus == IfOperStatusUp ? 1 | 0x40 : 0) | (loop ? 8 : 2) | (!(a->Flags & IP_ADAPTER_NO_MULTICAST) ? 0x1000 : 0);
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
            int af = u->Address.lpSockaddr->sa_family; if (af != (pass ? AF_INET6 : AF_INET)) continue;
            loop4 |= loop && af == AF_INET;
            if (add_interface(&tail, name, flags, u->Address.lpSockaddr, u->Address.iSockaddrLength, u->OnLinkPrefixLength, af == AF_INET6 ? a->Ipv6IfIndex : a->IfIndex, a->Mtu, a->PhysicalAddress, a->PhysicalAddressLength) < 0) {
                free(buf); g_freeifaddrs(*result); *result = NULL; return -1;
            }
        }
    }
    free(buf);
    if (!loop4) {
        SOCKADDR_IN lo = {0}; lo.sin_family = AF_INET; lo.sin_addr.s_addr = g_htonl(0x7f000001);
        if (add_interface(&tail, "lo", 1 | 8 | 0x40, (SOCKADDR *)&lo, sizeof(lo), 8, 1, 65536, NULL, 0) < 0) { g_freeifaddrs(*result); *result = NULL; return -1; }
    }
    return 0;
}

/* /proc/net/route (IPv4 routes of every named adapter: on-link subnet plus default via its
 * gateway, kernel hex format) and /sys/class/net/<if>/{operstate,carrier,address,mtu}.
 * Returns malloc'd text, NULL with errno set (ENOENT for unknown interfaces/attributes). */
char *lx_net_special(const char *path, size_t *len)
{
    if (net_init() < 0) return NULL;
    ULONG status; IP_ADAPTER_ADDRESSES *buf = query_adapters(&status);
    if (!buf && status != ERROR_NO_DATA) { lx_set_errno(lx_errno_from_win32(status)); return NULL; }
    IP_ADAPTER_ADDRESSES *primary = buf ? primary_adapter(buf) : NULL;
    size_t cap = 4096, n = 0; char *out = malloc(cap);
    if (!out) { free(buf); lx_set_errno(LX_ENOMEM); return NULL; }
    out[0] = 0;
    int found = 0;
    if (!strcmp(path, "/proc/net/route")) {
        found = 1;
        n += snprintf(out + n, cap - n, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
        for (IP_ADAPTER_ADDRESSES *a = buf; a && n + 256 < cap; a = a->Next) {
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->OperStatus != IfOperStatusUp) continue;
            char name[32]; adapter_name(buf, primary, a, name);
            const SOCKADDR_IN *gw = adapter_gateway4(a);
            if (gw) n += snprintf(out + n, cap - n, "%s\t00000000\t%08lX\t0003\t0\t0\t%lu\t00000000\t0\t0\t0\n", name,
                                  (unsigned long)gw->sin_addr.s_addr, (unsigned long)a->Ipv4Metric);
            for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
                unsigned prefix = u->OnLinkPrefixLength > 32 ? 32 : u->OnLinkPrefixLength;
                uint32_t mask = prefix ? g_htonl(0xffffffffu << (32 - prefix)) : 0;
                uint32_t net = ((const SOCKADDR_IN *)u->Address.lpSockaddr)->sin_addr.s_addr & mask;
                n += snprintf(out + n, cap - n, "%s\t%08lX\t00000000\t0001\t0\t0\t%lu\t%08lX\t0\t0\t0\n", name,
                              (unsigned long)net, (unsigned long)a->Ipv4Metric, (unsigned long)mask);
            }
        }
    } else if (!strncmp(path, "/sys/class/net/", 15)) {
        const char *ifn = path + 15, *slash = strchr(ifn, '/');
        size_t ifl = slash ? (size_t)(slash - ifn) : strlen(ifn);
        const char *attr = slash ? slash + 1 : "";
        for (IP_ADAPTER_ADDRESSES *a = buf; a && !found; a = a->Next) {
            char name[32]; adapter_name(buf, primary, a, name);
            if (strlen(name) != ifl || strncmp(name, ifn, ifl)) continue;
            int up = a->OperStatus == IfOperStatusUp;
            if (!strcmp(attr, "operstate")) { found = 1; n = snprintf(out, cap, "%s\n", up ? "up" : a->OperStatus == IfOperStatusDown ? "down" : "unknown"); }
            else if (!strcmp(attr, "carrier")) { found = 1; n = snprintf(out, cap, "%d\n", up); }
            else if (!strcmp(attr, "mtu")) { found = 1; n = snprintf(out, cap, "%lu\n", (unsigned long)a->Mtu); }
            else if (!strcmp(attr, "address")) {
                found = 1;
                unsigned char mac[6] = {0};
                memcpy(mac, a->PhysicalAddress, a->PhysicalAddressLength < 6 ? a->PhysicalAddressLength : 6);
                n = snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            }
            else break;
        }
    }
    free(buf);
    if (!found) { free(out); lx_set_errno(LX_ENOENT); return NULL; }
    *len = n;
    return out;
}
static unsigned g_if_nametoindex(const char *name)
{
    g_ifaddrs *list; if (g_getifaddrs(&list) < 0) return 0;
    unsigned index = 0;
    for (g_ifaddrs *p = list; p; p = p->next) if (!strcmp(p->name, name)) { index = ((if_node *)p)->index; break; }
    g_freeifaddrs(list); if (!index) lx_set_errno(LX_ENODEV); return index;
}
/* -2 means not a networking request: misc.c owns the generic ioctl entry. */
int lx_net_ioctl(int fd, unsigned long request, void *arg)
{
    if (request == 0x541b || request == 0x5421) {
        lx_fd *f = lx_fd_get(fd); if (!f) return -1;
        if (f->kind != LX_FD_SOCKET) return -2;
        if (!arg) return lx_set_errno(LX_EFAULT);
        if (request == 0x5421) { if (*(int *)arg) f->flags |= LX_O_NONBLOCK; else f->flags &= ~LX_O_NONBLOCK; return 0; }
        u_long bytes; if (ioctlsocket(f->sock, FIONREAD, &bytes) == SOCKET_ERROR) return net_fail();
        *(int *)arg = (int)bytes; return 0;
    }
    if (request != 0x8910 && request != 0x8912 && request != 0x8913 && request != 0x8915 && request != 0x8919 && request != 0x891b && request != 0x8921 && request != 0x8927 && request != 0x8933) return -2;
    if (!socket_fd(fd)) return -1;
    if (!arg) return lx_set_errno(LX_EFAULT);
    g_ifaddrs *list; if (g_getifaddrs(&list) < 0) return -1;
    int result = 0;
    if (request == 0x8912) {
        g_ifconf *conf = arg; int space = conf->len, needed = 0;
        if (space < 0) { g_freeifaddrs(list); return lx_set_errno(LX_EINVAL); }
        for (g_ifaddrs *p = list; p; p = p->next) if (p->addr->family == 2) {
            if (conf->buf && needed + 32 <= space) { g_ifreq *r = (g_ifreq *)((char *)conf->buf + needed); memset(r, 0, 32); strncpy(r->name, p->name, 15); memcpy(r->value, p->addr, 16); }
            if (!conf->buf || needed + 32 <= space) needed += 32;
        }
        conf->len = needed;
    } else {
        g_ifreq *r = arg; if_node *found = NULL;
        for (g_ifaddrs *p = list; p; p = p->next) {
            if ((request == 0x8910 ? ((if_node *)p)->index == *(unsigned *)r->value : !strncmp(p->name, r->name, 16)) && (p->addr->family == 2 || request == 0x8910 || request == 0x8913 || request == 0x8933)) { found = (if_node *)p; break; }
        }
        if (!found) result = lx_set_errno(LX_ENODEV);
        else switch (request) {
        case 0x8910: memset(r->name, 0, 16); strncpy(r->name, found->name, 15); break;
        case 0x8913: *(uint16_t *)r->value = (uint16_t)found->g.flags; break;
        case 0x8915: memcpy(r->value, found->g.addr, 16); break;
        case 0x8919: if (found->g.broadaddr) memcpy(r->value, found->g.broadaddr, 16); else result = lx_set_errno(LX_EADDRNOTAVAIL); break;
        case 0x891b: memcpy(r->value, found->g.netmask, 16); break;
        case 0x8921: *(unsigned *)r->value = found->mtu; break;
        case 0x8933: *(unsigned *)r->value = found->index; break;
        case 0x8927: memset(r->value, 0, 16); *(uint16_t *)r->value = found->g.flags & 8 ? 772 : 1; memcpy(r->value + 2, found->mac, found->maclen > 6 ? 6 : found->maclen); break;
        }
    }
    g_freeifaddrs(list); return result;
}

LX_SYMTAB(net,
    LX_SYM("socket", g_socket), LX_SYM("connect", g_connect), LX_SYM("bind", g_bind), LX_SYM("listen", g_listen),
    LX_SYM("accept", g_accept), LX_SYM("accept4", g_accept4), LX_SYM("shutdown", g_shutdown), LX_SYM("socketpair", g_socketpair),
    LX_SYM("send", g_send), LX_SYM("sendto", g_sendto), LX_SYM("sendmsg", g_sendmsg), LX_SYM("recv", g_recv), LX_SYM("recvfrom", g_recvfrom), LX_SYM("recvmsg", g_recvmsg),
    LX_SYMV("__recv_chk", "GLIBC_2.4", g_recv_chk),
    LX_SYM("getsockopt", g_getsockopt), LX_SYM("setsockopt", g_setsockopt), LX_SYM("getsockname", g_getsockname), LX_SYM("getpeername", g_getpeername),
    LX_SYM("select", g_select), LX_SYM("poll", g_poll), LX_SYM("__fdelt_chk", g_fdelt_chk),
    LX_SYM("gethostbyname", g_gethostbyname), LX_SYM("gethostbyname_r", g_gethostbyname_r), LX_SYM("gethostbyaddr", g_gethostbyaddr), LX_SYM("__h_errno_location", g_h_errno_location),
    LX_SYM("getaddrinfo", g_getaddrinfo), LX_SYM("freeaddrinfo", g_freeaddrinfo), LX_SYM("gai_strerror", g_gai_strerror), LX_SYM("getnameinfo", g_getnameinfo),
    LX_SYM("inet_addr", g_inet_addr), LX_SYM("inet_aton", g_inet_aton), LX_SYM("inet_ntoa", g_inet_ntoa), LX_SYM("inet_ntop", g_inet_ntop), LX_SYM("inet_pton", g_inet_pton),
    LX_SYM("htons", g_htons), LX_SYM("ntohs", g_htons), LX_SYM("htonl", g_htonl), LX_SYM("ntohl", g_htonl),
    LX_SYM("getifaddrs", g_getifaddrs), LX_SYM("freeifaddrs", g_freeifaddrs), LX_SYM("if_nametoindex", g_if_nametoindex), LX_SYM("gethostname", g_gethostname), LX_SYM("getservbyname", g_getservbyname)
);
