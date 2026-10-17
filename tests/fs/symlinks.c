#define _GNU_SOURCE
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d errno=%d\n",__LINE__,errno);exit(1);}}while(0)
int main(void)
{
    const char *root="/tmp/lxrt-fs-links";mkdir(root,0755);CHECK(chdir(root)==0);unlink("relative");unlink("absolute");unlink("target");
    int fd=open("target",O_CREAT|O_TRUNC|O_RDWR,0644);CHECK(fd>=0);CHECK(write(fd,"hello",5)==5);close(fd);
    if(symlink("target","relative")){
        CHECK(errno==EPERM);unlink("target");CHECK(chdir("/tmp")==0);rmdir(root);puts("symlinks unavailable EPERM");return 0;
    }
    struct stat s;CHECK(lstat("relative",&s)==0);printf("lstat link=%d size=%ld ino=%d\n",S_ISLNK(s.st_mode),(long)s.st_size,s.st_ino!=0);CHECK(S_ISLNK(s.st_mode)&&s.st_size==6);
    char b[256]={0};ssize_t n=readlink("relative",b,sizeof b);CHECK(n==6);printf("readlink %.*s\n",(int)n,b);CHECK(stat("relative",&s)==0&&S_ISREG(s.st_mode)&&s.st_size==5);
    CHECK(symlink("/tmp/lxrt-fs-links/target","absolute")==0);memset(b,0,sizeof b);n=readlink("absolute",b,sizeof b);CHECK(n==(ssize_t)strlen("/tmp/lxrt-fs-links/target"));printf("absolute guest=%d\n",!memcmp(b,"/tmp/lxrt-fs-links/target",n));
    int df=open(".",O_RDONLY|O_DIRECTORY);CHECK(df>=0);CHECK(readlinkat(df,"relative",b,3)==3);CHECK(!memcmp(b,"tar",3));close(df);
    unlink("relative");unlink("absolute");unlink("target");CHECK(chdir("/tmp")==0);CHECK(rmdir(root)==0);puts("symlinks ok");return 0;
}
