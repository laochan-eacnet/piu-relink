/* Directory streams and POSIX, case-sensitive wildcard matching. Literal glob
 * components use Win32 path lookup (therefore accept alternate filename case);
 * components containing wildcards are matched case-sensitively like Linux.
 * Streams snapshot FindFirstFileExW results when opened; rewind/seek replay that
 * snapshot. Sorting uses bytewise C-locale order. Tilde expansion supports HOME,
 * not a Linux passwd database; GNU alternate-directory callbacks return NOSYS.
 */
#include "lxrt.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>

#pragma pack(push,4)
typedef struct { uint32_t ino; int32_t off; uint16_t reclen; uint8_t type; char name[256]; } dir_entry;
typedef struct { uint64_t ino; int64_t off; uint16_t reclen; uint8_t type; char name[256]; } dir_entry64;
typedef struct { uint32_t count; char **paths; uint32_t offs; int flags; void (*close)(void *); void *(*read)(void *); void *(*open)(const char *); int (*lstat)(const char *,void *); int (*stat)(const char *,void *); } dir_glob;
#pragma pack(pop)
static_assert(sizeof(dir_entry)==268,"dirent");
static_assert(offsetof(dir_entry,name)==11,"dirent.name");
static_assert(offsetof(dir_entry,type)==10,"dirent.type");
static_assert(sizeof(dir_entry64)==276,"dirent64");
static_assert(offsetof(dir_entry64,name)==19,"dirent64.name");
static_assert(offsetof(dir_entry64,type)==18,"dirent64.type");
static_assert(sizeof(dir_glob)==36,"glob_t");
static_assert(offsetof(dir_glob,close)==16,"glob_t callbacks");
typedef struct { int fd; size_t count,pos; dir_entry64 *entries; int borrowed /* entries owned by metacache */; dir_entry small; dir_entry64 large; SRWLOCK lock; } dir_stream;

static uint64_t entry_ino(const wchar_t *parent,const wchar_t *name)
{
    size_t n=wcslen(parent),m=wcslen(name);wchar_t *w=malloc((n+m+2)*sizeof *w);if(!w)return 1;
    memcpy(w,parent,n*sizeof *w);w[n]=L'\\';memcpy(w+n+1,name,(m+1)*sizeof *w);
    HANDLE h=CreateFileW(w,0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);free(w);
    BY_HANDLE_FILE_INFORMATION i;uint64_t ino=1;if(h!=INVALID_HANDLE_VALUE){if(GetFileInformationByHandle(h,&i))ino=((uint64_t)i.nFileIndexHigh<<32)|i.nFileIndexLow;CloseHandle(h);}return ino?ino:1;
}
static int add_entry_id(dir_stream *d,size_t *cap,const wchar_t *name,size_t namelen,DWORD attrs,uint64_t ino)
{
    char s[256*4];
    int n=WideCharToMultiByte(CP_UTF8,0,name,(int)namelen,s,sizeof s-1,NULL,NULL);
    if(n<=0&&namelen)return lx_set_errno(LX_ENAMETOOLONG);
    s[n]=0;
    if(n>=256)return lx_set_errno(LX_ENAMETOOLONG);
    if(d->count==*cap){size_t next=*cap?*cap*2:32;dir_entry64 *v=realloc(d->entries,next*sizeof *v);if(!v)return lx_set_errno(LX_ENOMEM);d->entries=v;*cap=next;}
    dir_entry64 *e=&d->entries[d->count++];memset(e,0,sizeof *e);e->ino=ino?ino:1;e->off=d->count;e->reclen=(uint16_t)((19+n+1+3)&~3u);e->type=(attrs&FILE_ATTRIBUTE_REPARSE_POINT)?10:(attrs&FILE_ATTRIBUTE_DIRECTORY)?4:8;memcpy(e->name,s,n+1);return 0;
}
static int add_entry(dir_stream *d,size_t *cap,const wchar_t *path,const wchar_t *name,DWORD attrs)
{
    return add_entry_id(d,cap,name,wcslen(name),attrs,entry_ino(path,name));
}
/* One bulk FileIdBothDirectoryInfo enumeration on the directory handle: names, attributes and
 * file ids (d_ino) without opening every entry. "." / ".." are synthesized if the file system
 * does not report them (FAT/exFAT). */
static dir_stream *fdopendir_(int fd)
{
    lx_fd *f=lx_fd_get(fd);if(!f)return NULL;if(f->kind!=LX_FD_DIR||!f->wpath){lx_set_errno(LX_ENOTDIR);return NULL;}
    dir_stream *d=lx_calloc(1,sizeof *d);if(!d){lx_set_errno(LX_ENOMEM);return NULL;}d->fd=fd;size_t cap=0;
    int immutable=lx_host_immutable(f->wpath);
    if(immutable){
        size_t n; int e;
        const void *v=lx_meta_peek(LX_META_DIR,f->wpath,&n,&e);
        if(v){ d->entries=(dir_entry64 *)v; d->borrowed=1; d->count=n/sizeof *d->entries; return d; }
    }
    int dot=0,dotdot=0;
    enum { BUFSZ=64*1024 };
    unsigned char *buf=malloc(BUFSZ);if(!buf){lx_set_errno(LX_ENOMEM);goto fail;}
    FILE_INFO_BY_HANDLE_CLASS cls=FileIdBothDirectoryRestartInfo;
    for(;;){
        if(!GetFileInformationByHandleEx(f->h,cls,buf,BUFSZ)){
            DWORD err=GetLastError();
            if(err==ERROR_NO_MORE_FILES)break;
            free(buf);lx_set_errno(lx_errno_from_win32(err));goto fail;
        }
        cls=FileIdBothDirectoryInfo;
        for(FILE_ID_BOTH_DIR_INFO *e=(FILE_ID_BOTH_DIR_INFO *)buf;;e=(FILE_ID_BOTH_DIR_INFO *)((char *)e+e->NextEntryOffset)){
            size_t len=e->FileNameLength/sizeof(wchar_t);
            if(len==1&&e->FileName[0]==L'.')dot=1;
            if(len==2&&e->FileName[0]==L'.'&&e->FileName[1]==L'.')dotdot=1;
            if(add_entry_id(d,&cap,e->FileName,len,e->FileAttributes,(uint64_t)e->FileId.QuadPart)){free(buf);goto fail;}
            if(!e->NextEntryOffset)break;
        }
    }
    free(buf);
    if(!dotdot&&add_entry(d,&cap,f->wpath,L"..",FILE_ATTRIBUTE_DIRECTORY))goto fail;
    if(!dot&&add_entry(d,&cap,f->wpath,L".",FILE_ATTRIBUTE_DIRECTORY))goto fail;
    if(immutable) lx_meta_put(LX_META_DIR,f->wpath,d->entries,d->count*sizeof *d->entries,0);
    return d;
fail: free(d->entries);lx_free(d);return NULL;
}
static dir_entry64 *readdir64_(dir_stream *d){if(!d){lx_set_errno(LX_EBADF);return NULL;}AcquireSRWLockExclusive(&d->lock);dir_entry64 *r=NULL;if(d->pos<d->count){d->large=d->entries[d->pos++];r=&d->large;}ReleaseSRWLockExclusive(&d->lock);return r;}
static void small_entry(const dir_entry64 *a,dir_entry *b){memset(b,0,sizeof *b);b->ino=(uint32_t)a->ino;b->off=(int32_t)a->off;b->reclen=(uint16_t)((11+strlen(a->name)+1+3)&~3u);b->type=a->type;strcpy(b->name,a->name);}
static dir_entry *readdir_(dir_stream *d){if(!d){lx_set_errno(LX_EBADF);return NULL;}AcquireSRWLockExclusive(&d->lock);dir_entry *r=NULL;if(d->pos<d->count){small_entry(&d->entries[d->pos++],&d->small);r=&d->small;}ReleaseSRWLockExclusive(&d->lock);return r;}
static int readdir_r_(dir_stream *d,dir_entry *e,dir_entry **r){if(!d)return LX_EBADF;AcquireSRWLockExclusive(&d->lock);*r=NULL;if(d->pos<d->count){small_entry(&d->entries[d->pos++],e);*r=e;}ReleaseSRWLockExclusive(&d->lock);return 0;}
static int readdir64_r_(dir_stream *d,dir_entry64 *e,dir_entry64 **r){if(!d)return LX_EBADF;AcquireSRWLockExclusive(&d->lock);*r=NULL;if(d->pos<d->count){*e=d->entries[d->pos++];*r=e;}ReleaseSRWLockExclusive(&d->lock);return 0;}
/* Directories inside [immutable] mounts whose listing is already cached get a handle-less fd
 * (kind LX_FD_DIR, h NULL, wpath set): opendir/readdir/closedir then never touch the file
 * system. Everything else opens a real directory handle. */
static int open_cached_dir(const char *p)
{
    wchar_t *w=lx_map_path(p);if(!w)return -1;
    size_t n; int e;
    if(!lx_host_immutable(w)||!lx_meta_peek(LX_META_DIR,w,&n,&e)){free(w);return -2;}
    int fd=lx_fd_alloc(LX_FD_DIR,NULL,LX_O_RDONLY|LX_O_DIRECTORY|LX_O_CLOEXEC);
    if(fd<0){free(w);return -1;}
    lx_fd_get(fd)->wpath=w;
    return fd;
}
static dir_stream *opendir_(const char *p){int fd=open_cached_dir(p);if(fd==-2)fd=lx_open(p,LX_O_RDONLY|LX_O_DIRECTORY|LX_O_CLOEXEC,0);if(fd<0)return NULL;dir_stream *d=fdopendir_(fd);if(!d){int e=lx_errno;lx_close(fd);lx_errno=e;}return d;}
static int closedir_(dir_stream *d){if(!d)return lx_set_errno(LX_EBADF);int r=lx_close(d->fd);if(!d->borrowed)free(d->entries);lx_free(d);return r;}
static void rewinddir_(dir_stream *d){if(d){AcquireSRWLockExclusive(&d->lock);d->pos=0;ReleaseSRWLockExclusive(&d->lock);}}
static long telldir_(dir_stream *d){if(!d)return lx_set_errno(LX_EBADF);AcquireSRWLockShared(&d->lock);long p=(long)d->pos;ReleaseSRWLockShared(&d->lock);return p;}
static void seekdir_(dir_stream *d,long p){if(d&&p>=0){AcquireSRWLockExclusive(&d->lock);d->pos=(size_t)p;ReleaseSRWLockExclusive(&d->lock);}}
static int dirfd_(dir_stream *d){return d?d->fd:lx_set_errno(LX_EBADF);}
static int alphasort_(const dir_entry **a,const dir_entry **b){return strcmp((*a)->name,(*b)->name);}
static int alphasort64_(const dir_entry64 **a,const dir_entry64 **b){return strcmp((*a)->name,(*b)->name);}
static int version_cmp(const char *a,const char *b)
{
    size_t i=0,digit_start=0;
    int only_zeroes=1;
    while(a[i]==b[i]){
        unsigned c=(unsigned char)a[i];
        if(!c)return 0;
        if(c<'0'||c>'9'){digit_start=i+1;only_zeroes=1;}
        else if(c!='0')only_zeroes=0;
        i++;
    }
    if(a[digit_start]!='0'&&b[digit_start]!='0'){
        size_t j=i;
        while(a[j]>='0'&&a[j]<='9'){
            if(b[j]<'0'||b[j]>'9')return 1;
            j++;
        }
        if(b[j]>='0'&&b[j]<='9')return -1;
    }else if(only_zeroes&&digit_start<i&&
        ((a[i]>='0'&&a[i]<='9')||(b[i]>='0'&&b[i]<='9'))){
        return (unsigned char)(a[i]-'0')-(unsigned char)(b[i]-'0');
    }
    return (unsigned char)a[i]-(unsigned char)b[i];
}
static int versionsort_(const dir_entry **a,const dir_entry **b){return version_cmp((*a)->name,(*b)->name);}
static int versionsort64_(const dir_entry64 **a,const dir_entry64 **b){return version_cmp((*a)->name,(*b)->name);}
static int scandir_common(const char *path,void ***result,int (*filter)(const void *),int (*compare)(const void *,const void *),int wide)
{
    dir_stream *d=opendir_(path);if(!d)return -1;size_t cap=0,n=0;void **v=NULL;int error=0;
    for(size_t i=0;i<d->count;i++){dir_entry s;void *e=wide?(void *)&d->entries[i]:(small_entry(&d->entries[i],&s),(void *)&s);if(filter&&!filter(e))continue;
        if(n==cap){size_t next=cap?cap*2:32;void **w=lx_realloc(v,next*sizeof *w);if(!w){error=LX_ENOMEM;break;}v=w;cap=next;}
        size_t size=wide?sizeof(dir_entry64):sizeof(dir_entry);v[n]=lx_malloc(size);if(!v[n]){error=LX_ENOMEM;break;}memcpy(v[n++],e,size);
    }
    closedir_(d);if(error){for(size_t i=0;i<n;i++)lx_free(v[i]);lx_free(v);return lx_set_errno(error);}if(compare&&n>1)qsort(v,n,sizeof *v,compare);if(!v)v=lx_malloc(sizeof *v);if(!v)return lx_set_errno(LX_ENOMEM);*result=v;return (int)n;
}
static int scandir_(const char *p,void ***v,int (*f)(const void *),int (*c)(const void *,const void *)){return scandir_common(p,v,f,c,0);}
static int scandir64_(const char *p,void ***v,int (*f)(const void *),int (*c)(const void *,const void *)){return scandir_common(p,v,f,c,1);}

#define F_PATH 1
#define F_NOESC 2
#define F_PERIOD 4
#define F_LEADING 8
#define F_CASE 16
static unsigned fold(unsigned c,int flags){return (flags&F_CASE)&&c>='A'&&c<='Z'?c+32:c;}
static int char_class(const char *name,size_t n,unsigned c)
{
#define CLASS(s,fn) if(n==sizeof(s)-1&&!memcmp(name,s,n))return fn((unsigned char)c)!=0
    CLASS("alnum",isalnum);CLASS("alpha",isalpha);CLASS("blank",isblank);CLASS("cntrl",iscntrl);CLASS("digit",isdigit);CLASS("graph",isgraph);CLASS("lower",islower);CLASS("print",isprint);CLASS("punct",ispunct);CLASS("space",isspace);CLASS("upper",isupper);CLASS("xdigit",isxdigit);
#undef CLASS
    return 0;
}
/* Returns -1 for an unmatched '[', and advances only for a valid bracket. */
static int bracket(const char **pp,unsigned c,int flags)
{
    const char *p=*pp;int neg=*p=='!'||*p=='^';if(neg)p++;int hit=0,first=1;
    while(*p&&(*p!=']'||first)){first=0;
        if(p[0]=='['&&p[1]==':'){const char *end=strstr(p+2,":]");if(end){hit|=char_class(p+2,(size_t)(end-p-2),c);p=end+2;continue;}}
        unsigned lo=(unsigned char)*p++;if(lo=='\\'&&!(flags&F_NOESC)&&*p)lo=(unsigned char)*p++;
        if(*p=='-'&&p[1]&&p[1]!=']'){p++;unsigned hi=(unsigned char)*p++;if(hi=='\\'&&!(flags&F_NOESC)&&*p)hi=(unsigned char)*p++;unsigned fc=fold(c,flags);hit|=fc>=fold(lo,flags)&&fc<=fold(hi,flags);}
        else hit|=fold(lo,flags)==fold(c,flags);
    }
    if(*p!=']')return -1;*pp=p+1;return neg?!hit:hit;
}
static int match(const char *p,const char *s,int flags,int start)
{
    for(;;){unsigned c=(unsigned char)*p++;
        if(!c)return !*s||((flags&F_LEADING)&&*s=='/');
        if(start&&(flags&F_PERIOD)&&*s=='.'&&(c=='*'||c=='?'||c=='['))return 0;
        if(c=='*'){
            while(*p=='*')p++;if(!*p){if(!(flags&F_PATH))return 1;return !strchr(s,'/')||(flags&F_LEADING);}
            for(;;){if(match(p,s,flags,start))return 1;if(!*s||((flags&F_PATH)&&*s=='/'))return 0;s++;start=0;}
        }
        if(!*s)return 0;
        if(c=='?'){if((flags&F_PATH)&&*s=='/')return 0;}
        else if(c=='['){if((flags&F_PATH)&&*s=='/')return 0;const char *q=p;int hit=bracket(&q,(unsigned char)*s,flags);if(hit>=0){if(!hit)return 0;p=q;}else if(*s!='[')return 0;}
        else {if(c=='\\'&&!(flags&F_NOESC)&&*p)c=(unsigned char)*p++;if(fold(c,flags)!=fold((unsigned char)*s,flags))return 0;}
        start=(flags&F_PATH)&&*s=='/';s++;
    }
}
static int fnmatch_(const char *p,const char *s,int flags){return match(p,s,flags,1)?0:1;}
#define G_ERR 1
#define G_MARK 2
#define G_NOSORT 4
#define G_DOOFFS 8
#define G_NOCHECK 16
#define G_APPEND 32
#define G_NOESC 64
#define G_PERIOD 128
#define G_MAGIC 256
#define G_ALT 512
#define G_BRACE 1024
#define G_NOMAGIC 2048
#define G_TILDE 4096
#define G_ONLYDIR 8192
#define G_TILDE_CHECK 16384

typedef struct { dir_glob *g; int flags; int (*error)(const char *,int); size_t cap; int magic; } glob_ctx;
static int has_magic(const char *p,int flags){for(;*p;p++){if(*p=='\\'&&!(flags&G_NOESC)&&p[1])p++;else if(*p=='*'||*p=='?'||*p=='[')return 1;}return 0;}
static char *path_join(const char *a,const char *b){size_t n=strlen(a),m=strlen(b);int slash=n&&a[n-1]!='/';char *s=malloc(n+slash+m+1);if(s){memcpy(s,a,n);if(slash)s[n++]='/';memcpy(s+n,b,m+1);}return s;}
static DWORD path_attrs(const char *p){wchar_t *w=lx_map_path(*p?p:".");if(!w)return INVALID_FILE_ATTRIBUTES;DWORD a=GetFileAttributesW(w);free(w);return a;}
static int glob_add(glob_ctx *c,const char *p,int dir)
{
    dir_glob *g=c->g;size_t offs=(c->flags&G_DOOFFS)?g->offs:0;
    if((size_t)g->count+offs+2>c->cap){size_t cap=c->cap?c->cap*2:32;while(cap<(size_t)g->count+offs+2){if(cap>SIZE_MAX/2/sizeof(char *))return 1;cap*=2;}char **v=lx_realloc(g->paths,cap*sizeof *v);if(!v)return 1;if(!g->paths)memset(v,0,offs*sizeof *v);g->paths=v;c->cap=cap;}
    size_t n=strlen(p);int mark=dir&&(c->flags&G_MARK)&&n&&p[n-1]!='/';char *s=lx_malloc(n+mark+1);if(!s)return 1;memcpy(s,p,n);if(mark)s[n++]='/';s[n]=0;g->paths[offs+g->count++]=s;g->paths[offs+g->count]=NULL;return 0;
}
static int glob_error(glob_ctx *c,const char *p,int e){return ((c->error&&c->error(*p?p:".",e))||(c->flags&G_ERR))?2:0;}
static int glob_walk(glob_ctx *c,const char *prefix,const char *rest)
{
    if(!*rest){DWORD a=path_attrs(prefix);if(a==INVALID_FILE_ATTRIBUTES)return 0;if((c->flags&G_ONLYDIR)&&!(a&FILE_ATTRIBUTE_DIRECTORY))return 0;return glob_add(c,*prefix?prefix:".",(a&FILE_ATTRIBUTE_DIRECTORY)!=0);}
    const char *end=strchr(rest,'/');size_t len=end?(size_t)(end-rest):strlen(rest);char *part=malloc(len+1);if(!part)return 1;memcpy(part,rest,len);part[len]=0;const char *tail=end?end+1:rest+len;while(*tail=='/')tail++;
    if(!len){free(part);return glob_walk(c,*prefix?prefix:"/",tail);}
    if(!has_magic(part,c->flags)){
        if(!(c->flags&G_NOESC)){char *o=part;for(char *p=part;*p;p++){if(*p=='\\'&&p[1])p++;*o++=*p;}*o=0;}
        char *full=path_join(prefix,part);free(part);if(!full)return 1;DWORD a=path_attrs(full);int r=0;
        if(a!=INVALID_FILE_ATTRIBUTES){if(end){if(a&FILE_ATTRIBUTE_DIRECTORY){if(!*tail){char *slash=path_join(full,"");if(!slash)r=1;else{r=glob_add(c,slash,1);free(slash);}}else r=glob_walk(c,full,tail);}}else if(!(c->flags&G_ONLYDIR)||(a&FILE_ATTRIBUTE_DIRECTORY))r=glob_add(c,full,(a&FILE_ATTRIBUTE_DIRECTORY)!=0);}
        free(full);return r;
    }
    c->magic=1;dir_stream *d=opendir_(*prefix?prefix:".");if(!d){int e=lx_errno;free(part);return e==LX_ENOENT||e==LX_ENOTDIR?0:glob_error(c,prefix,e);}
    int r=0;for(size_t i=0;i<d->count;i++){dir_entry64 *e=&d->entries[i];int f=(c->flags&G_NOESC)?F_NOESC:0;if(!(c->flags&G_PERIOD))f|=F_PERIOD;if(fnmatch_(part,e->name,f))continue;
        char *full=path_join(prefix,e->name);if(!full){r=1;break;}DWORD a=path_attrs(full);if(a!=INVALID_FILE_ATTRIBUTES){int isdir=(a&FILE_ATTRIBUTE_DIRECTORY)!=0;
            if(end&&isdir){if(*tail)r=glob_walk(c,full,tail);else{char *slash=path_join(full,"");if(!slash)r=1;else{r=glob_add(c,slash,1);free(slash);}}}
            /* glibc treats ONLYDIR as a d_type hint: unresolved symlinks
             * remain candidates, and the caller may check them afterwards. */
            else if(!end&&(!(c->flags&G_ONLYDIR)||isdir||e->type==10))r=glob_add(c,full,isdir);
        }free(full);if(r)break;
    }closedir_(d);free(part);return r;
}
static int brace_expand(glob_ctx *c,const char *pattern)
{
    if(c->flags&G_BRACE){const char *left=NULL,*right=NULL;int level=0,comma=0;for(const char *p=pattern;*p;p++){if(*p=='\\'&&!(c->flags&G_NOESC)&&p[1]){p++;continue;}if(*p=='{'){if(!level)left=p;level++;}else if(*p=='}'&&level){if(!--level){right=p;if(comma)break;left=NULL;}}else if(*p==','&&level==1)comma=1;}
        if(left&&right&&comma){const char *begin=left+1;level=0;for(const char *p=begin;p<=right;p++){if(*p=='\\'&&!(c->flags&G_NOESC)&&p<right){p++;continue;}if(*p=='{')level++;if(*p=='}'&&p!=right)level--;if(p==right||(*p==','&&!level)){size_t a=(size_t)(left-pattern),b=(size_t)(p-begin),d=strlen(right+1);char *s=malloc(a+b+d+1);if(!s)return 1;memcpy(s,pattern,a);memcpy(s+a,begin,b);memcpy(s+a+b,right+1,d+1);int r=brace_expand(c,s);free(s);if(r)return r;begin=p+1;}}return 0;}}
    return glob_walk(c,pattern[0]=='/'?"/":"",pattern[0]=='/'?pattern+1:pattern);
}
static int string_sort(const void *a,const void *b){return strcmp(*(const char *const *)a,*(const char *const *)b);}
static int glob_(const char *pattern,int flags,int (*error)(const char *,int),dir_glob *g)
{
    if(!pattern||!g||(flags&~0x7fff)){lx_set_errno(LX_EINVAL);return -1;}if(flags&G_ALT){lx_set_errno(LX_ENOSYS);return 4;}
    if(!(flags&G_APPEND)){g->count=0;g->paths=NULL;if(!(flags&G_DOOFFS))g->offs=0;}
    glob_ctx c={g,flags,error,0,has_magic(pattern,flags)};size_t old=g->count;char *expanded=NULL;const char *p=pattern;
    if((flags&(G_TILDE|G_TILDE_CHECK))&&p[0]=='~'){
        const char *slash=strchr(p,'/');size_t user=slash?(size_t)(slash-p-1):strlen(p+1);const char *home=NULL;
        if(!user){char **environment=*(char ***)lx_data_addr(&lx_environ);for(char **e=environment;e&&*e;e++)if(!strncmp(*e,"HOME=",5)){home=*e+5;break;}}
        if(home){expanded=slash?path_join(home,slash+1):_strdup(home);if(!expanded)return 1;p=expanded;}
        else if(flags&G_TILDE_CHECK)return 3;
    }
    int r=brace_expand(&c,p);if(!r&&g->count==old&&((flags&G_NOCHECK)||((flags&G_NOMAGIC)&&!c.magic)))r=glob_add(&c,p,0);
    free(expanded);g->flags=flags|(c.magic?G_MAGIC:0);if(r)return r;if(g->count==old)return 3;
    size_t offs=(flags&G_DOOFFS)?g->offs:0;if(!(flags&G_NOSORT)&&g->count-old>1)qsort(g->paths+offs+old,g->count-old,sizeof(char *),string_sort);return 0;
}
static void globfree_(dir_glob *g){if(!g)return;size_t offs=(g->flags&G_DOOFFS)?g->offs:0;for(size_t i=0;i<g->count;i++)lx_free(g->paths[offs+i]);lx_free(g->paths);g->paths=NULL;g->count=0;}
LX_SYMTAB(dir,
 LX_SYM("opendir",opendir_),LX_SYM("fdopendir",fdopendir_),LX_SYM("readdir",readdir_),LX_SYM("readdir64",readdir64_),LX_SYM("readdir_r",readdir_r_),LX_SYM("readdir64_r",readdir64_r_),LX_SYM("closedir",closedir_),LX_SYM("rewinddir",rewinddir_),LX_SYM("telldir",telldir_),LX_SYM("seekdir",seekdir_),LX_SYM("dirfd",dirfd_),LX_SYM("scandir",scandir_),LX_SYM("scandir64",scandir64_),LX_SYM("alphasort",alphasort_),LX_SYM("alphasort64",alphasort64_),LX_SYM("versionsort",versionsort_),LX_SYM("versionsort64",versionsort64_),
 LX_SYMV("glob","GLIBC_2.27",glob_),LX_SYMV("glob64","GLIBC_2.27",glob_),LX_SYM("globfree",globfree_),LX_SYM("globfree64",globfree_),LX_SYM("fnmatch",fnmatch_)
);
