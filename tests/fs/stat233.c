/* Build against WSL's /usr/i686-linux-gnu to exercise stat@GLIBC_2.33
 * directly rather than glibc 2.31's __xstat inline wrappers. */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d errno=%d\n",__LINE__,errno);exit(1);}}while(0)
int main(void)
{
    const char *p="/tmp/lxrt-fs-stat233";int fd=open(p,O_CREAT|O_TRUNC|O_RDWR,0644);CHECK(fd>=0);CHECK(write(fd,"modern",6)==6);
    struct stat a,b,c;struct stat64 x,y,z;
    CHECK(stat(p,&a)==0);CHECK(fstat(fd,&b)==0);CHECK(fstatat(AT_FDCWD,p,&c,0)==0);
    CHECK(stat64(p,&x)==0);CHECK(fstat64(fd,&y)==0);CHECK(fstatat64(AT_FDCWD,p,&z,0)==0);
    CHECK(a.st_size==6&&b.st_size==6&&c.st_size==6);CHECK(x.st_size==6&&y.st_size==6&&z.st_size==6);
    CHECK(lstat(p,&a)==0);CHECK(lstat64(p,&x)==0);
    printf("modern stat reg=%d size=%ld ino=%d stat64=%lld\n",S_ISREG(a.st_mode),(long)a.st_size,a.st_ino!=0,(long long)x.st_size);
    close(fd);unlink(p);return 0;
}
