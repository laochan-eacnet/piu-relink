#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
extern ssize_t __recv_chk(int fd, void *buf, size_t length, size_t buffer_length, int flags);

_Static_assert(sizeof(struct sockaddr) == 16, "sockaddr");
_Static_assert(sizeof(struct sockaddr_in) == 16, "sockaddr_in");
_Static_assert(sizeof(struct sockaddr_in6) == 28, "sockaddr_in6");
_Static_assert(sizeof(struct timeval) == 8, "timeval");
_Static_assert(sizeof(struct pollfd) == 8, "pollfd");
_Static_assert(sizeof(struct iovec) == 8, "iovec");
_Static_assert(sizeof(struct msghdr) == 28, "msghdr");
_Static_assert(sizeof(struct hostent) == 20, "hostent");
_Static_assert(sizeof(struct addrinfo) == 32 && offsetof(struct addrinfo, ai_addr) == 20, "addrinfo");
_Static_assert(sizeof(struct ifaddrs) == 28 && offsetof(struct ifaddrs, ifa_addr) == 12, "ifaddrs");
_Static_assert(sizeof(struct ifreq) == 32 && sizeof(struct ifconf) == 8, "ifreq");
_Static_assert(sizeof(struct servent) == 16, "servent");
_Static_assert(sizeof(fd_set) == 128, "fd_set");

static void check(int ok, const char *what)
{
    if (!ok) { fprintf(stderr, "FAIL %s errno=%d\n", what, errno); exit(1); }
}
static struct sockaddr_in local_address(void)
{
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); return a;
}
static int readable(int fd)
{
    fd_set r; FD_ZERO(&r); FD_SET(fd, &r); struct timeval timeout = {2, 0};
    return select(fd + 1, &r, NULL, NULL, &timeout) == 1 && FD_ISSET(fd, &r);
}
static void tcp_test(void)
{
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); check(listener >= 0, "socket TCP");
    int one = 1; check(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0, "reuseaddr");
    struct sockaddr_in a = local_address(); check(bind(listener, (void *)&a, sizeof(a)) == 0, "bind TCP");
    socklen_t n = sizeof(a); check(getsockname(listener, (void *)&a, &n) == 0 && n == sizeof(a), "getsockname TCP");
    check(listen(listener, 8) == 0, "listen");
    int client = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); check(client >= 0, "client socket");
    int c = connect(client, (void *)&a, sizeof(a)); check(c == 0 || (c == -1 && errno == EINPROGRESS), "nonblocking connect");
    fd_set w; FD_ZERO(&w); FD_SET(client, &w); struct timeval timeout = {2,0};
    check(select(client + 1, NULL, &w, NULL, &timeout) == 1 && FD_ISSET(client, &w), "select connect");
    int error = -1; n = sizeof(error); check(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &n) == 0 && error == 0, "SO_ERROR");
    check(readable(listener), "listener readable");
    int server = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC); check(server >= 0, "accept4");
    check((fcntl(server, F_GETFL) & O_NONBLOCK) && (fcntl(server, F_GETFD) & FD_CLOEXEC), "accept4 flags");
    n = sizeof(a); check(getpeername(server, (void *)&a, &n) == 0 && a.sin_family == AF_INET, "getpeername");
    check(setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0, "TCP_NODELAY");
    struct timeval delay = {0, 125000}, got = {0}; n = sizeof(got);
    check(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &delay, sizeof(delay)) == 0, "set timeout");
    check(getsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &got, &n) == 0 && got.tv_sec == 0 && got.tv_usec >= 125000, "get timeout");
    check(write(client, "hello", 5) == 5, "socket write");
    struct pollfd p = {server, POLLIN, 0}; check(poll(&p, 1, 2000) == 1 && (p.revents & POLLIN), "poll TCP");
    int pending = 0; check(ioctl(server, FIONREAD, &pending) == 0 && pending == 5, "FIONREAD");
    char text[32] = {0}; check(read(server, text, sizeof(text)) == 5 && !strcmp(text, "hello"), "socket read");
    check(recv(server, text, sizeof(text), 0) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK), "nonblocking read");
    struct iovec v[2] = {{"ab",2},{"cd",2}}; struct msghdr msg = {0}; msg.msg_iov = v; msg.msg_iovlen = 2;
    check(sendmsg(server, &msg, MSG_NOSIGNAL) == 4, "sendmsg TCP"); check(readable(client), "client readable");
    char left[2], right[2]; struct iovec r[2] = {{left,2},{right,2}}; memset(&msg, 0, sizeof(msg)); msg.msg_iov = r; msg.msg_iovlen = 2;
    check(recvmsg(client, &msg, 0) == 4 && !memcmp(left,"ab",2) && !memcmp(right,"cd",2), "recvmsg TCP");
    check(send(server, "chk", 3, MSG_NOSIGNAL) == 3 && readable(client), "fortified recv setup");
    check(__recv_chk(client, text, 3, sizeof(text), 0) == 3 && !memcmp(text, "chk", 3), "__recv_chk ABI");
    check(__recv_chk(-1, text, 0, sizeof(text), 0) == -1 && errno == EBADF, "__recv_chk errno");
    check(shutdown(server, SHUT_WR) == 0 && readable(client) && recv(client,text,sizeof(text),0) == 0, "shutdown EOF");
    check(close(server) == 0 && close(client) == 0 && close(listener) == 0, "close TCP");
    puts("tcp connect/read/write/select/poll/message: ok");
}
static void udp_test(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0), c = socket(AF_INET, SOCK_DGRAM, 0); check(s >= 0 && c >= 0, "socket UDP");
    struct sockaddr_in a = local_address(); check(bind(s, (void *)&a, sizeof(a)) == 0, "bind UDP");
    socklen_t n = sizeof(a); check(getsockname(s, (void *)&a, &n) == 0, "name UDP");
    check(sendto(c, "datagram", 8, MSG_NOSIGNAL, (void *)&a, sizeof(a)) == 8, "sendto UDP");
    check(readable(s), "select UDP");
    char text[32] = {0}; struct sockaddr_in from; n = sizeof(from);
    check(recvfrom(s, text, sizeof(text), 0, (void *)&from, &n) == 8 && !strcmp(text,"datagram") && from.sin_family == AF_INET, "recvfrom UDP");
    struct iovec vec[2] = {{"12",2},{"345",3}}; struct msghdr msg = {0}; msg.msg_name = &a; msg.msg_namelen = sizeof(a); msg.msg_iov = vec; msg.msg_iovlen = 2;
    check(sendmsg(c,&msg,0) == 5, "sendmsg UDP"); struct pollfd p = {s,POLLIN,0}; check(poll(&p,1,2000)==1 && (p.revents&POLLIN), "poll UDP");
    char one[2], two[3]; struct iovec out[2]={{one,2},{two,3}}; memset(&msg,0,sizeof(msg)); msg.msg_name=&from; msg.msg_namelen=sizeof(from); msg.msg_iov=out; msg.msg_iovlen=2;
    check(recvmsg(s,&msg,0)==5 && !memcmp(one,"12",2) && !memcmp(two,"345",3) && msg.msg_namelen==sizeof(from), "recvmsg UDP");
    close(c); close(s); puts("udp send/recv/select/poll/message: ok");
}
static void ipv6_test(void)
{
    int s=socket(AF_INET6,SOCK_DGRAM,0),c=socket(AF_INET6,SOCK_DGRAM,0);
    check(s>=0 && c>=0,"IPv6 sockets");
    struct sockaddr_in6 a={0},from={0}; a.sin6_family=AF_INET6; a.sin6_addr.s6_addr[15]=1;
    check(bind(s,(void*)&a,sizeof(a))==0,"IPv6 bind");
    socklen_t n=sizeof(a); check(getsockname(s,(void*)&a,&n)==0 && n==28 && a.sin6_family==AF_INET6,"IPv6 getsockname");
    check(sendto(c,"ipv6",4,0,(void*)&a,sizeof(a))==4 && readable(s),"IPv6 send/select");
    char text[8]={0}; n=sizeof(from);
    check(recvfrom(s,text,sizeof(text),0,(void*)&from,&n)==4 && !strcmp(text,"ipv6") && n==28 && from.sin6_family==AF_INET6,"IPv6 recvfrom");
    close(c);close(s);
    struct addrinfo hints={0},*list=NULL; hints.ai_family=AF_INET6; hints.ai_socktype=SOCK_DGRAM; hints.ai_flags=AI_NUMERICHOST;
    check(getaddrinfo("::1","80",&hints,&list)==0 && list && list->ai_family==AF_INET6 && list->ai_addrlen==28 && list->ai_addr->sa_family==AF_INET6,"IPv6 getaddrinfo");
    freeaddrinfo(list);puts("IPv6 socket/address ABI: ok");
}
static void resolver_test(void)
{
    struct in_addr a; char text[64];
    check(inet_aton("127.0.0.1",&a)==1 && a.s_addr==htonl(0x7f000001), "inet_aton");
    check(!strcmp(inet_ntoa(a),"127.0.0.1"), "inet_ntoa");
    check(inet_pton(AF_INET,"127.0.0.1",&a)==1 && inet_ntop(AF_INET,&a,text,sizeof(text)) && !strcmp(text,"127.0.0.1"), "inet_pton/ntop");
    struct in6_addr a6; check(inet_pton(AF_INET6,"::1",&a6)==1 && inet_ntop(AF_INET6,&a6,text,sizeof(text)) && !strcmp(text,"::1"), "inet6");
    struct addrinfo hints={0},*list=NULL; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    check(getaddrinfo("localhost","80",&hints,&list)==0 && list && list->ai_family==AF_INET && list->ai_addrlen==16 && list->ai_addr->sa_family==AF_INET,"getaddrinfo");
    char service[16]; check(getnameinfo(list->ai_addr,list->ai_addrlen,text,sizeof(text),service,sizeof(service),NI_NUMERICHOST|NI_NUMERICSERV)==0 && !strcmp(service,"80"),"getnameinfo"); freeaddrinfo(list);
    struct hostent *host=gethostbyname("localhost"); check(host && host->h_addrtype==AF_INET && host->h_length==4 && host->h_addr_list[0],"gethostbyname");
    struct hostent ret,*result=NULL; char buffer[4096]; int err=0; check(gethostbyname_r("localhost",&ret,buffer,sizeof(buffer),&result,&err)==0 && result==&ret && ret.h_addr_list[0],"gethostbyname_r");
    struct ifaddrs *ifs=NULL; check(getifaddrs(&ifs)==0,"getifaddrs"); int loop=0;
    for(struct ifaddrs *p=ifs;p;p=p->ifa_next) if(p->ifa_addr && p->ifa_addr->sa_family==AF_INET && (p->ifa_flags&IFF_LOOPBACK)) { check(p->ifa_netmask!=NULL,"loopback mask"); check(if_nametoindex(p->ifa_name)>0,"if_nametoindex"); loop=1; }
    freeifaddrs(ifs); check(loop,"loopback listed");
    int control=socket(AF_INET,SOCK_DGRAM,0); check(control>=0,"interface ioctl socket");
    struct ifreq req={0}; strcpy(req.ifr_name,"lo");
    check(ioctl(control,SIOCGIFADDR,&req)==0 && req.ifr_addr.sa_family==AF_INET,"SIOCGIFADDR");
    check(ioctl(control,SIOCGIFFLAGS,&req)==0 && (req.ifr_flags&IFF_LOOPBACK),"SIOCGIFFLAGS");
    struct ifconf conf={0}; check(ioctl(control,SIOCGIFCONF,&conf)==0 && conf.ifc_len>=32,"SIOCGIFCONF size");
    close(control);
    puts("resolver/inet/interface loopback: ok");
}
static void mixed_test(void)
{
    int f[2]; check(pipe(f)==0,"pipe");
    struct pollfd p={f[0],POLLIN,0}; check(poll(&p,1,0)==0,"empty pipe poll");
    check(write(f[1],"x",1)==1,"pipe write"); check(poll(&p,1,0)==1 && (p.revents&POLLIN),"pipe ready poll");
    int s=socket(AF_INET,SOCK_DGRAM,0); check(s>=0,"mixed socket");
    fd_set r,w; FD_ZERO(&r); FD_ZERO(&w); FD_SET(f[0],&r); FD_SET(s,&w);
    struct timeval t={1,0}; int max=s>f[0]?s:f[0];
    check(select(max+1,&r,&w,NULL,&t)==2 && FD_ISSET(f[0],&r) && FD_ISSET(s,&w),"mixed pipe/socket select");
    char c; check(read(f[0],&c,1)==1 && c=='x',"pipe read");
    close(f[1]); p.revents=0; check(poll(&p,1,0)==1 && (p.revents&POLLHUP),"pipe hangup poll");
    close(f[0]); close(s);
    puts("mixed pipe/socket readiness: ok");
}
int main(void)
{
    tcp_test(); udp_test(); ipv6_test(); resolver_test(); mixed_test();
    struct pollfd p = {4095, POLLIN, 0}; check(poll(&p,1,0)==1 && (p.revents&POLLNVAL),"POLLNVAL");
    fd_set r; FD_ZERO(&r); FD_SET(0,&r); struct timeval t={0,0}; check(select(1,&r,NULL,NULL,&t)>=0,"mixed fd select");
    puts("network: ok"); return 0;
}
