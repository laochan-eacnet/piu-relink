#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void check(int condition,const char *name)
{ if(!condition){fprintf(stderr,"FAIL %s errno=%d\n",name,errno);exit(1);} }
int main(void)
{
    struct in_addr a;
    check(inet_aton("127.1",&a)==1 && a.s_addr==htonl(0x7f000001),"short IPv4");
    check(inet_aton("0x7f000001",&a)==1 && a.s_addr==htonl(0x7f000001),"hex IPv4");
    check(inet_aton("255.255.255.255",&a)==1 && a.s_addr==INADDR_BROADCAST,"broadcast IPv4");
    check(inet_aton("256.0.0.1",&a)==0 && inet_aton("1.2.3.4:5",&a)==0,"invalid IPv4");
    struct hostent h,*result=(void*)1; int herr=0; char tiny[1];
    check(gethostbyname_r("localhost",&h,tiny,sizeof(tiny),&result,&herr)==ERANGE && !result && herr==NETDB_INTERNAL,"resolver ERANGE");
    int s=socket(AF_INET,SOCK_DGRAM,0),c=socket(AF_INET,SOCK_DGRAM,0); check(s>=0&&c>=0,"UDP sockets");
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    check(bind(s,(void*)&addr,sizeof(addr))==0,"bind");socklen_t n=sizeof(addr);check(getsockname(s,(void*)&addr,&n)==0,"name");
    int one=1,zero=0;check(ioctl(s,FIONBIO,&one)==0 && (fcntl(s,F_GETFL)&O_NONBLOCK),"FIONBIO enable");
    char buf[4];check(recv(s,buf,sizeof(buf),0)==-1 && errno==EAGAIN,"EAGAIN");
    check(ioctl(s,FIONBIO,&zero)==0 && !(fcntl(s,F_GETFL)&O_NONBLOCK),"FIONBIO disable");
    check(recv(s,buf,sizeof(buf),MSG_DONTWAIT)==-1 && errno==EAGAIN,"MSG_DONTWAIT");
    check(sendto(c,"abcdef",6,0,(void*)&addr,sizeof(addr))==6,"send truncation");
    check(recv(s,buf,2,MSG_TRUNC)==6 && !memcmp(buf,"ab",2),"recv MSG_TRUNC");
    check(sendto(c,"uvwxyz",6,0,(void*)&addr,sizeof(addr))==6,"send message truncation");
    struct iovec vec={buf,2};struct msghdr msg={0};msg.msg_iov=&vec;msg.msg_iovlen=1;
    check(recvmsg(s,&msg,0)==2 && (msg.msg_flags&MSG_TRUNC) && !memcmp(buf,"uv",2),"recvmsg truncation");
    struct timeval timeout={0,30000};check(setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0,"recv timeout");
    check(recv(s,buf,sizeof(buf),0)==-1 && errno==EAGAIN,"SO_RCVTIMEO delivery");close(s);close(c);
    puts("inet/resolver/nonblock/truncation/timeout errors: ok");
    int listener=socket(AF_INET,SOCK_STREAM,0);check(listener>=0,"TCP socket");addr.sin_port=0;
    check(bind(listener,(void*)&addr,sizeof(addr))==0,"reserve port");n=sizeof(addr);check(getsockname(listener,(void*)&addr,&n)==0,"reserve name");close(listener);
    c=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);check(c>=0,"nonblocking TCP");
    int r=connect(c,(void*)&addr,sizeof(addr));check(r==-1 && (errno==EINPROGRESS||errno==ECONNREFUSED),"connect refused initiation");
    if(errno==EINPROGRESS){fd_set w,e;FD_ZERO(&w);FD_ZERO(&e);FD_SET(c,&w);FD_SET(c,&e);timeout.tv_sec=3;timeout.tv_usec=0;
        check(select(c+1,NULL,&w,&e,&timeout)>0,"connect refused readiness");int error=0;n=sizeof(error);
        check(getsockopt(c,SOL_SOCKET,SO_ERROR,&error,&n)==0 && error==ECONNREFUSED,"connect refused errno translation");}
    close(c);puts("nonblocking refused connect: ok");return 0;
}
