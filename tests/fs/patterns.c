#define _GNU_SOURCE
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <glob.h>
#include <dirent.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d errno=%d\n",__LINE__,errno);exit(1);}}while(0)
static void file(const char *p){int f=open(p,O_CREAT|O_TRUNC|O_WRONLY,0644);CHECK(f>=0);close(f);}
static void show(const char *p,int f){glob_t g={0};int r=glob(p,f,NULL,&g);printf("pattern %s %d",p,r);if(!r)for(size_t i=0;i<g.gl_pathc;i++)printf(" %s",g.gl_pathv[i]);puts("");globfree(&g);}
static int nondot(const struct dirent *e){return e->d_name[0]!='.';}
static int nondot64(const struct dirent64 *e){return e->d_name[0]!='.';}
int main(void)
{
    CHECK(environ&&environ[0]); /* Keep a guest R_386_COPY of environ live. */
    mkdir("/tmp/lxrt-fs-patterns",0755);CHECK(chdir("/tmp/lxrt-fs-patterns")==0);mkdir("child",0755);file("child/inside");file(".dot");
    const char *names[]={"v1","v10","v2","v01","v001","v0","v00","v009","a-b","axb","aaa","aab"};for(size_t i=0;i<sizeof names/sizeof *names;i++)file(names[i]);
    show("child/",0);show("{a{aa,ab},v{1,2}}",GLOB_BRACE);show("a?b",0);show("a\\*b",0);show("a\\*b",GLOB_NOESCAPE);show("[!av.]*",0);show(".*",0);show("*",GLOB_PERIOD);show("missing",GLOB_NOMAGIC);show("missing*",GLOB_NOMAGIC);show("v*",GLOB_ONLYDIR);
    CHECK(setenv("HOME","/tmp/lxrt-fs-patterns",1)==0);show("~/child/*",GLOB_TILDE);show("~",GLOB_TILDE);show("~no_such_lxrt_user",GLOB_TILDE_CHECK);
    struct dirent **v;int n=scandir(".",&v,nondot,versionsort);CHECK(n>=0);printf("version");for(int i=0;i<n;i++){printf(" %s",v[i]->d_name);free(v[i]);}free(v);puts("");
    struct dirent64 **v64;n=scandir64(".",&v64,nondot64,versionsort64);CHECK(n>=0);printf("version64");for(int i=0;i<n;i++){printf(" %s",v64[i]->d_name);free(v64[i]);}free(v64);puts("");
    DIR *d=opendir(".");CHECK(d);struct dirent64 e,*r;int count=0;while(1){CHECK(readdir64_r(d,&e,&r)==0);if(!r)break;CHECK(e.d_ino);count++;}closedir(d);printf("readdir64_r %d\n",count);
    const struct {const char *p,*s;int f;} cases[]={{"a/*","a/.x",FNM_PATHNAME|FNM_PERIOD},{"a/.*","a/.x",FNM_PATHNAME|FNM_PERIOD},{"a/*","a/x/y",FNM_PATHNAME},{"a*","a/x",FNM_PATHNAME|FNM_LEADING_DIR},{"[!a-c]","d",0},{"[]a]","]",0},{"[","[",0},{"A?C","abc",FNM_CASEFOLD},{"a\\*","a\\x",FNM_NOESCAPE}};
    printf("matching");for(size_t i=0;i<sizeof cases/sizeof *cases;i++)printf(" %d",fnmatch(cases[i].p,cases[i].s,cases[i].f));puts("");
    for(size_t i=0;i<sizeof names/sizeof *names;i++)unlink(names[i]);unlink(".dot");unlink("child/inside");rmdir("child");CHECK(chdir("/tmp")==0);CHECK(rmdir("/tmp/lxrt-fs-patterns")==0);puts("patterns ok");return 0;
}
