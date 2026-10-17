#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/statfs.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <glob.h>
#include <fnmatch.h>
#include <utime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static void touch(const char *p,const char *s){int f=open(p,O_CREAT|O_TRUNC|O_RDWR,0644);CHECK(f>=0);CHECK(write(f,s,strlen(s))==(ssize_t)strlen(s));CHECK(close(f)==0);}
static void showglob(const char *pattern,int flags){glob_t g={0};int r=glob(pattern,flags,NULL,&g);printf("glob %s %d",pattern,r);if(!r)for(size_t i=0;i<g.gl_pathc;i++)printf(" %s",g.gl_pathv[g.gl_offs+i]);puts("");globfree(&g);}
int main(void)
{
    const char *root="/tmp/lxrt-fs-test";mkdir(root,0755);CHECK(chdir(root)==0);
    unlink("alpha.txt");unlink("beta.txt");unlink("link");unlink("hard");unlink("out");unlink("replace");unlink("readonly");unlink("sub/nested.txt");rmdir("sub");
    touch("alpha.txt","hello");touch("beta.txt","world!");CHECK(mkdir("sub",0755)==0);touch("sub/nested.txt","nested");touch(".hidden","hide");
    struct stat s;struct stat64 s64;CHECK(stat("alpha.txt",&s)==0);printf("stat reg=%d size=%ld ino=%d mtime=%d\n",S_ISREG(s.st_mode),(long)s.st_size,s.st_ino!=0,s.st_mtime>1500000000);
    CHECK(stat64("alpha.txt",&s64)==0);printf("stat64 reg=%d size=%lld ino=%d mtime=%d\n",S_ISREG(s64.st_mode),(long long)s64.st_size,s64.st_ino!=0,s64.st_mtime>1500000000);
    int f=open("alpha.txt",O_RDWR);CHECK(f>=0);CHECK(fstat(f,&s)==0);printf("fstat reg=%d size=%ld ino=%d\n",S_ISREG(s.st_mode),(long)s.st_size,s.st_ino!=0);
    CHECK(stat("sub",&s)==0);printf("stat dir=%d ino=%d\n",S_ISDIR(s.st_mode),s.st_ino!=0);
    CHECK(lstat("alpha.txt",&s)==0);printf("lstat reg=%d ino=%d\n",S_ISREG(s.st_mode),s.st_ino!=0);
    char b[256]={0};errno=0;CHECK(readlink("alpha.txt",b,sizeof b)<0&&errno==EINVAL);
    CHECK(link("alpha.txt","hard")==0);CHECK(stat("hard",&s)==0);CHECK(fstat64(f,&s64)==0);printf("hard same=%d nlink=%d\n",s.st_ino==(ino_t)s64.st_ino,s.st_nlink>=2);
    struct timespec times[2]={{1600000000,123456700},{1600000001,987654300}};CHECK(futimens(f,times)==0);CHECK(fstat(f,&s)==0);printf("times %ld.%09ld %ld.%09ld\n",(long)s.st_atim.tv_sec,s.st_atim.tv_nsec,(long)s.st_mtim.tv_sec,s.st_mtim.tv_nsec);
    char *real=realpath("sub/../alpha.txt",NULL);CHECK(real);printf("realpath %d\n",strcmp(real,"/tmp/lxrt-fs-test/alpha.txt")==0);free(real);
    touch("replace","old");CHECK(rename("beta.txt","replace")==0);int r=open("replace",O_RDONLY);CHECK(r>=0);memset(b,0,sizeof b);CHECK(read(r,b,20)==6);printf("rename %s\n",b);close(r);
    CHECK(truncate("replace",3)==0);CHECK(stat64("replace",&s64)==0);printf("truncate %lld\n",(long long)s64.st_size);
    struct dirent **list;int count=scandir(".",&list,NULL,alphasort);CHECK(count>=0);printf("directory");for(int i=0;i<count;i++){printf(" %s:%d",list[i]->d_name,list[i]->d_type);free(list[i]);}free(list);puts("");
    DIR *d=opendir(".");CHECK(d);struct dirent *e=readdir(d);CHECK(e);long pos=telldir(d);e=readdir(d);CHECK(e);strcpy(b,e->d_name);seekdir(d,pos);e=readdir(d);CHECK(e);printf("seekdir %d\n",strcmp(b,e->d_name)==0);rewinddir(d);CHECK(telldir(d)==0);closedir(d);
    int df=open("sub",O_RDONLY|O_DIRECTORY);CHECK(df>=0);CHECK(fstatat(df,"nested.txt",&s,0)==0);CHECK(s.st_size==6);CHECK(mkdirat(df,"child",0755)==0);CHECK(unlinkat(df,"child",AT_REMOVEDIR)==0);CHECK(fchdir(df)==0);char cwd[512];CHECK(getcwd(cwd,sizeof cwd));printf("fchdir %d\n",strcmp(cwd,"/tmp/lxrt-fs-test/sub")==0);CHECK(chdir("..")==0);d=fdopendir(df);CHECK(d);CHECK(dirfd(d)==df);closedir(d);CHECK(fstat(df,&s)<0&&errno==EBADF);
    showglob("*.txt",0);showglob("sub/*",GLOB_MARK);showglob("{alpha,replace}*",GLOB_BRACE);showglob("[al]*",0);showglob("missing*",0);showglob("missing*",GLOB_NOCHECK);showglob("*",GLOB_ONLYDIR|GLOB_MARK);
    glob_t g={0};g.gl_offs=2;CHECK(glob("alpha*",GLOB_DOOFFS,NULL,&g)==0);CHECK(glob("replace",GLOB_DOOFFS|GLOB_APPEND,NULL,&g)==0);printf("glob append %u %d %s %s\n",(unsigned)g.gl_pathc,!g.gl_pathv[0]&&!g.gl_pathv[1],g.gl_pathv[2],g.gl_pathv[3]);globfree(&g);
    printf("fnmatch %d %d %d %d %d %d\n",fnmatch("*.txt","alpha.txt",0),fnmatch("*",".hidden",FNM_PERIOD),fnmatch("*","a/b",FNM_PATHNAME),fnmatch("ALPHA*","alpha.txt",FNM_CASEFOLD),fnmatch("[[:digit:]]?","3x",0),fnmatch("a\\*","a*",0));
    int out=open("out",O_CREAT|O_TRUNC|O_RDWR,0644);CHECK(out>=0);off64_t off=1;CHECK(sendfile64(out,f,&off,3)==3);CHECK(lseek(f,0,SEEK_CUR)==0);CHECK(lseek(out,0,SEEK_SET)==0);memset(b,0,sizeof b);CHECK(read(out,b,sizeof b)==3);printf("sendfile %s offset=%lld\n",b,(long long)off);close(out);
    struct statvfs v;struct statvfs64 v64;struct statfs fs;struct statfs64 fs64;CHECK(statvfs(".",&v)==0);CHECK(fstatvfs64(f,&v64)==0);CHECK(statfs(".",&fs)==0);CHECK(fstatfs64(f,&fs64)==0);printf("space %d %d %d %d\n",v.f_bsize>0,v64.f_blocks>=v64.f_bavail,fs.f_bsize>0,fs64.f_blocks>0);
    touch("readonly","r");CHECK(chmod("readonly",0444)==0);CHECK(stat("readonly",&s)==0);printf("chmod readonly=%d\n",!(s.st_mode&0222));CHECK(chmod("readonly",0644)==0);CHECK(access("readonly",W_OK)==0);
    int rf=open("readonly",O_RDONLY);CHECK(rf>=0);CHECK(fchmod(rf,0444)==0);CHECK(fchmod(rf,0644)==0);CHECK(futimens(rf,times)==0);close(rf);
    close(f);unlink("alpha.txt");unlink("link");unlink("hard");unlink("out");unlink("replace");unlink("readonly");unlink("sub/nested.txt");unlink(".hidden");rmdir("sub");CHECK(chdir("/tmp")==0);CHECK(rmdir(root)==0);puts("files ok");return 0;
}
