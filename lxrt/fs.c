/* Linux i386 metadata and pathname operations over Win32.
 * Ownership/mode policy is deliberately synthetic: uid/gid 1000, directories
 * 0755, files 0644, with write bits controlled by FILE_ATTRIBUTE_READONLY.
 * chown is a no-op; umask is retained but cannot represent arbitrary ACLs.
 * statvfs inode capacities are estimated from block capacity, not NTFS MFT
 * counts. realpath returns a lexical, existing guest path, never a host path.
 * Symlink creation needs Developer Mode or Windows privilege; mkfifo is ENOSYS.
 */
#include "lxrt.h"
#include <winioctl.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

extern int lx_chdir(const char *);
#define AT_FDCWD_ (-100)
#define AT_SYMLINK_NOFOLLOW_ 0x100
#define AT_REMOVEDIR_ 0x200
#define AT_EMPTY_PATH_ 0x1000
#pragma pack(push, 4)
typedef struct { int32_t sec, nsec; } fs_ts;
typedef struct {
    uint64_t dev; uint32_t pad1, ino, mode, nlink, uid, gid;
    uint64_t rdev; uint32_t pad2; int32_t size, blksize, blocks;
    fs_ts atim, mtim, ctim; uint32_t reserved[2];
} fs_stat;
typedef struct {
    uint64_t dev; uint32_t pad1, old_ino, mode, nlink, uid, gid;
    uint64_t rdev; uint32_t pad2; int64_t size; int32_t blksize;
    int64_t blocks; fs_ts atim, mtim, ctim; uint64_t ino;
} fs_stat64;
typedef struct { uint32_t bsize, frsize, blocks, bfree, bavail, files, ffree, favail, fsid, unused, flag, namemax; int32_t spare[6]; } fs_vfs;
typedef struct { uint32_t bsize, frsize; uint64_t blocks, bfree, bavail, files, ffree, favail; uint32_t fsid, unused, flag, namemax; int32_t spare[6]; } fs_vfs64;
typedef struct { uint32_t type, bsize, blocks, bfree, bavail, files, ffree, fsid[2], namelen, frsize, flags, spare[4]; } fs_sfs;
typedef struct { uint32_t type, bsize; uint64_t blocks, bfree, bavail, files, ffree; uint32_t fsid[2], namelen, frsize, flags, spare[4]; } fs_sfs64;
typedef struct { int32_t sec, usec; } fs_tv;
typedef struct { int32_t actime, modtime; } fs_utimbuf;
#pragma pack(pop)
#define PIN(t, field, off) static_assert(offsetof(t, field) == off, #t "." #field)
static_assert(sizeof(fs_ts)==8, "timespec");
static_assert(sizeof(fs_tv)==8, "timeval");
static_assert(sizeof(fs_utimbuf)==8, "utimbuf");
static_assert(sizeof(fs_stat)==88, "stat");
PIN(fs_stat, ino,12); PIN(fs_stat, mode,16); PIN(fs_stat,size,44); PIN(fs_stat,atim,56); PIN(fs_stat,mtim,64); PIN(fs_stat,ctim,72);
static_assert(sizeof(fs_stat64)==96, "stat64");
PIN(fs_stat64,old_ino,12); PIN(fs_stat64,size,44); PIN(fs_stat64,blocks,56); PIN(fs_stat64,atim,64); PIN(fs_stat64,ino,88);
static_assert(sizeof(fs_vfs)==72, "statvfs"); PIN(fs_vfs,fsid,32); PIN(fs_vfs,flag,40);
static_assert(sizeof(fs_vfs64)==96, "statvfs64"); PIN(fs_vfs64,fsid,56); PIN(fs_vfs64,flag,64);
static_assert(sizeof(fs_sfs)==64, "statfs"); PIN(fs_sfs,fsid,28); PIN(fs_sfs,namelen,36);
static_assert(sizeof(fs_sfs64)==84, "statfs64"); PIN(fs_sfs64,fsid,48); PIN(fs_sfs64,namelen,56);

static wchar_t *at_path(int fd, const char *path)
{
    if (!path) { lx_set_errno(LX_EFAULT); return NULL; }
    if (!*path) { lx_set_errno(LX_ENOENT); return NULL; }
    if (fd==AT_FDCWD_ || path[0]=='/' || (path[0] && path[1]==':')) return lx_map_path(path);
    lx_fd *f=lx_fd_get(fd);
    if (!f) return NULL;
    if (f->kind!=LX_FD_DIR || !f->wpath) { lx_set_errno(LX_ENOTDIR); return NULL; }
    wchar_t *tail=lx_utf8_to_wide(path);
    size_t n=wcslen(f->wpath), m=wcslen(tail);
    wchar_t *w=malloc((n+m+2)*sizeof *w);
    if (!w) { free(tail); lx_set_errno(LX_ENOMEM); return NULL; }
    memcpy(w,f->wpath,n*sizeof *w); w[n]=L'\\'; memcpy(w+n+1,tail,(m+1)*sizeof *w); free(tail);
    return w;
}
static HANDLE metadata_open(const wchar_t *w, int nofollow, DWORD access)
{
    return CreateFileW(w,access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | (nofollow?FILE_FLAG_OPEN_REPARSE_POINT:0),NULL);
}
static HANDLE attributes_handle(lx_fd *f)
{
    HANDLE h=ReOpenFile(f->h,FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,FILE_FLAG_BACKUP_SEMANTICS);
    if(h==INVALID_HANDLE_VALUE&&f->wpath)h=metadata_open(f->wpath,0,FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES);
    return h;
}
static long readlinkat_(int,const char *,char *,unsigned);
static void ft_to_ts(FILETIME t, fs_ts *s)
{
    uint64_t v=((uint64_t)t.dwHighDateTime<<32)|t.dwLowDateTime;
    int64_t u=(int64_t)v-INT64_C(116444736000000000);
    int64_t sec=u/10000000, rem=u%10000000;
    if(rem<0) { rem+=10000000; --sec; }
    s->sec=(int32_t)sec; s->nsec=(int32_t)(rem*100);
}
static int fill_stat(HANDLE h, int kind, int nofollow, fs_stat64 *s)
{
    memset(s,0,sizeof *s); s->uid=s->gid=1000; s->blksize=4096; s->nlink=1;
    if(kind==LX_FD_MEM) { s->mode=0100444; s->ino=s->old_ino=1; return 0; }
    DWORD type=kind==LX_FD_SOCKET?0:GetFileType(h);
    if(kind==LX_FD_SOCKET || kind==LX_FD_PIPE || kind==LX_FD_CONSOLE || kind==LX_FD_NULL || kind==LX_FD_RANDOM || kind==LX_FD_ZERO || type==FILE_TYPE_CHAR || type==FILE_TYPE_PIPE) {
        s->mode=(kind==LX_FD_SOCKET?0140000:(kind==LX_FD_PIPE||type==FILE_TYPE_PIPE)?0010000:0020000)|0666;
        s->ino=s->old_ino=(uint32_t)(uintptr_t)h+1; return 0;
    }
    BY_HANDLE_FILE_INFORMATION i;
    if(!GetFileInformationByHandle(h,&i)) return lx_fail_win32();
    s->dev=i.dwVolumeSerialNumber; s->ino=((uint64_t)i.nFileIndexHigh<<32)|i.nFileIndexLow; s->old_ino=(uint32_t)s->ino;
    s->nlink=i.nNumberOfLinks; s->size=((int64_t)i.nFileSizeHigh<<32)|i.nFileSizeLow;
    s->mode=(i.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)?0040755:0100644;
    if(i.dwFileAttributes&FILE_ATTRIBUTE_READONLY) s->mode&=~0222;
    if(nofollow && (i.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) s->mode=0120777;
    s->blocks=(s->size+511)/512;
    ft_to_ts(i.ftLastAccessTime,&s->atim); ft_to_ts(i.ftLastWriteTime,&s->mtim); ft_to_ts(i.ftCreationTime,&s->ctim);
    FILE_BASIC_INFO b;
    if(GetFileInformationByHandleEx(h,FileBasicInfo,&b,sizeof b)) {
        FILETIME t={(DWORD)b.ChangeTime.LowPart,(DWORD)b.ChangeTime.HighPart}; ft_to_ts(t,&s->ctim);
    }
    return 0;
}
/* Handle-free stat (Windows 11 24H2+): GetFileInformationByName(FileStatBasicByNameInfo) returns
 * the same file id / link count / volume serial / times as the handle path without the
 * CreateFile cost that dominates stat-heavy guest scans under Prism and AV filters. */
typedef struct {
    LARGE_INTEGER FileId, CreationTime, LastAccessTime, LastWriteTime, ChangeTime, AllocationSize, EndOfFile;
    DWORD FileAttributes, ReparseTag, NumberOfLinks, DeviceType, DeviceCharacteristics, Reserved;
    LARGE_INTEGER VolumeSerialNumber;
    unsigned char FileId128[16];
} stat_basic_info;
typedef BOOL (WINAPI *by_name_fn)(PCWSTR, int, PVOID, ULONG);
static by_name_fn by_name(void)
{
    static by_name_fn fn; static volatile LONG done;
    if(!done){ HMODULE k=GetModuleHandleW(L"kernelbase.dll"); fn=k?(by_name_fn)(void *)GetProcAddress(k,"GetFileInformationByName"):NULL; InterlockedExchange(&done,1); }
    return fn;
}
static int fill_stat_by_name(const stat_basic_info *i, fs_stat64 *s)
{
    memset(s,0,sizeof *s); s->uid=s->gid=1000; s->blksize=4096;
    s->dev=(uint32_t)i->VolumeSerialNumber.QuadPart; s->ino=(uint64_t)i->FileId.QuadPart; s->old_ino=(uint32_t)s->ino;
    s->nlink=i->NumberOfLinks; s->size=i->EndOfFile.QuadPart;
    s->mode=(i->FileAttributes&FILE_ATTRIBUTE_DIRECTORY)?0040755:0100644;
    if(i->FileAttributes&FILE_ATTRIBUTE_READONLY) s->mode&=~0222;
    s->blocks=(s->size+511)/512;
    FILETIME t;
    t.dwLowDateTime=i->LastAccessTime.LowPart; t.dwHighDateTime=(DWORD)i->LastAccessTime.HighPart; ft_to_ts(t,&s->atim);
    t.dwLowDateTime=i->LastWriteTime.LowPart; t.dwHighDateTime=(DWORD)i->LastWriteTime.HighPart; ft_to_ts(t,&s->mtim);
    t.dwLowDateTime=i->ChangeTime.LowPart; t.dwHighDateTime=(DWORD)i->ChangeTime.HighPart; ft_to_ts(t,&s->ctim);
    return 0;
}
static int stat_uncached(int fd,const char *p,wchar_t *w,fs_stat64 *s,int flags);
static int stat_path_impl(int fd,const char *p,fs_stat64 *s,int flags);
/* [debug] trace / trace_missing: failed path lookups are the usual cause of "resource missing" bugs; log them. */
static int stat_path(int fd,const char *p,fs_stat64 *s,int flags)
{
    int r=stat_path_impl(fd,p,s,flags);
    if(r&&(lx_trace||lx_trace_missing)) { int e=lx_errno; lx_log_missing((flags&AT_SYMLINK_NOFOLLOW_)?"lstat":"stat",p,e); lx_errno=e; }
    return r;
}
static int stat_path_impl(int fd,const char *p,fs_stat64 *s,int flags)
{
    if(!s) return lx_set_errno(LX_EFAULT);
    if(flags&~(AT_SYMLINK_NOFOLLOW_|AT_EMPTY_PATH_)) return lx_set_errno(LX_EINVAL);
    if(p && !*p && (flags&AT_EMPTY_PATH_)) {
        lx_fd *f=lx_fd_get(fd); if(!f)return -1;
        int r=fill_stat(f->h,f->kind,0,s); if(!r&&f->kind==LX_FD_MEM){s->size=f->memlen;s->blocks=(s->size+511)/512;} return r;
    }
    wchar_t *w=at_path(fd,p); if(!w)return -1;
    int kind=(flags&AT_SYMLINK_NOFOLLOW_)?LX_META_LSTAT:LX_META_STAT;
    if(lx_host_immutable(w)) {
        void *v; size_t n; int e;
        if(lx_meta_get(kind,w,&v,&n,&e)) { free(w); if(e){free(v);return lx_set_errno(e);} memcpy(s,v,sizeof *s); free(v); return 0; }
        int r=stat_uncached(fd,p,w,s,flags);
        lx_meta_put(kind,w,r?NULL:s,r?0:sizeof *s,r?lx_errno:0);
        free(w);
        return r;
    }
    int r=stat_uncached(fd,p,w,s,flags);
    free(w);
    return r;
}
/* `w` stays owned by the caller. */
static int stat_uncached(int fd,const char *p,wchar_t *w,fs_stat64 *s,int flags)
{
    by_name_fn fn=by_name();
    stat_basic_info info;
    if(fn) {
        if(fn(w,3 /* FileStatBasicByNameInfo */,&info,sizeof info)) {
            if(!(info.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) return fill_stat_by_name(&info,s);
        } else {
            DWORD e=GetLastError();
            if(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND) return lx_set_errno(LX_ENOENT);
        }
    }
    HANDLE h=metadata_open(w,flags&AT_SYMLINK_NOFOLLOW_,FILE_READ_ATTRIBUTES);
    DWORD err=GetLastError();
    if(h==INVALID_HANDLE_VALUE)return lx_set_errno(lx_errno_from_win32(err));
    int r=fill_stat(h,LX_FD_FILE,flags&AT_SYMLINK_NOFOLLOW_,s); CloseHandle(h);
    if(!r&&(s->mode&0170000)==0120000){char target[32768];long n=readlinkat_(fd,p,target,sizeof target);if(n>=0){s->size=n;s->blocks=0;}}
    return r;
}
static int fstat64_(int fd,fs_stat64 *s) { lx_fd *f=lx_fd_get(fd); if(!f)return -1; if(!s)return lx_set_errno(LX_EFAULT);
    if(f->kind==LX_FD_DIR&&!f->h&&f->wpath) return stat_uncached(AT_FDCWD_,NULL,f->wpath,s,0); /* handle-less cached dir fd (dir.c) */
    int r=fill_stat(f->h,f->kind,0,s);if(!r&&f->kind==LX_FD_MEM){s->size=f->memlen;s->blocks=(s->size+511)/512;}return r; }
static int narrow_stat(const fs_stat64 *a,fs_stat *b)
{
    if(!b)return lx_set_errno(LX_EFAULT);
    if(a->size>INT32_MAX || a->blocks>INT32_MAX)return lx_set_errno(LX_EOVERFLOW);
    memset(b,0,sizeof *b); b->dev=a->dev;b->ino=a->old_ino;b->mode=a->mode;b->nlink=a->nlink;b->uid=a->uid;b->gid=a->gid;b->rdev=a->rdev;b->size=(int32_t)a->size;b->blksize=a->blksize;b->blocks=(int32_t)a->blocks;b->atim=a->atim;b->mtim=a->mtim;b->ctim=a->ctim; return 0;
}
static int stat64_(const char *p,fs_stat64 *s){return stat_path(AT_FDCWD_,p,s,0);}
static int lstat64_(const char *p,fs_stat64 *s){return stat_path(AT_FDCWD_,p,s,AT_SYMLINK_NOFOLLOW_);}
static int fstatat64_(int fd,const char *p,fs_stat64 *s,int f){return stat_path(fd,p,s,f);}
static int stat_(const char *p,fs_stat *s){fs_stat64 a;return stat64_(p,&a)?-1:narrow_stat(&a,s);}
static int lstat_(const char *p,fs_stat *s){fs_stat64 a;return lstat64_(p,&a)?-1:narrow_stat(&a,s);}
static int fstat_(int fd,fs_stat *s){fs_stat64 a;return fstat64_(fd,&a)?-1:narrow_stat(&a,s);}
static int fstatat_(int fd,const char *p,fs_stat *s,int f){fs_stat64 a;return stat_path(fd,p,&a,f)?-1:narrow_stat(&a,s);}
static int xstat_(int v,const char *p,fs_stat *s){return v!=3?lx_set_errno(LX_EINVAL):stat_(p,s);}
static int lxstat_(int v,const char *p,fs_stat *s){return v!=3?lx_set_errno(LX_EINVAL):lstat_(p,s);}
static int fxstat_(int v,int fd,fs_stat *s){return v!=3?lx_set_errno(LX_EINVAL):fstat_(fd,s);}
static int xstat64_(int v,const char *p,fs_stat64 *s){return v!=3?lx_set_errno(LX_EINVAL):stat64_(p,s);}
static int lxstat64_(int v,const char *p,fs_stat64 *s){return v!=3?lx_set_errno(LX_EINVAL):lstat64_(p,s);}
static int fxstat64_(int v,int fd,fs_stat64 *s){return v!=3?lx_set_errno(LX_EINVAL):fstat64_(fd,s);}
static int fxstatat64_(int v,int fd,const char *p,fs_stat64 *s,int f){return v!=3?lx_set_errno(LX_EINVAL):stat_path(fd,p,s,f);}
static int fxstatat_(int v,int fd,const char *p,fs_stat *s,int f){return v!=3?lx_set_errno(LX_EINVAL):fstatat_(fd,p,s,f);}

static int disk_info(const wchar_t *w,fs_vfs64 *s)
{
    wchar_t root[32768]; ULARGE_INTEGER avail,total,freeb;
    if(!GetVolumePathNameW(w,root,LX_COUNT(root)))return lx_fail_win32();
    if(!GetDiskFreeSpaceExW(root,&avail,&total,&freeb))return lx_fail_win32();
    DWORD serial=0,maxname=255,flags=0; GetVolumeInformationW(root,NULL,0,&serial,&maxname,&flags,NULL,0);
    memset(s,0,sizeof *s); s->bsize=s->frsize=4096;s->blocks=total.QuadPart/4096;s->bfree=freeb.QuadPart/4096;s->bavail=avail.QuadPart/4096;
    s->files=s->blocks;s->ffree=s->favail=s->bavail;
    s->fsid=serial;s->flag=(flags&FILE_READ_ONLY_VOLUME)?1:0;s->namemax=maxname;return 0;
}
static int statvfs64_(const char *p,fs_vfs64 *s){if(!s)return lx_set_errno(LX_EFAULT);wchar_t *w=lx_map_path(p);if(!w)return -1;DWORD a=GetFileAttributesW(w);int r=a==INVALID_FILE_ATTRIBUTES?lx_fail_win32():disk_info(w,s);free(w);return r;}
static int fstatvfs64_(int fd,fs_vfs64 *s){lx_fd *f=lx_fd_get(fd);if(!f)return -1;if(!s)return lx_set_errno(LX_EFAULT);return f->wpath?disk_info(f->wpath,s):lx_set_errno(LX_EINVAL);}
static int vfs32(const fs_vfs64 *a,fs_vfs *b){if(!b)return lx_set_errno(LX_EFAULT);if(a->blocks>UINT32_MAX||a->bfree>UINT32_MAX||a->bavail>UINT32_MAX)return lx_set_errno(LX_EOVERFLOW);memset(b,0,sizeof *b);b->bsize=a->bsize;b->frsize=a->frsize;b->blocks=(uint32_t)a->blocks;b->bfree=(uint32_t)a->bfree;b->bavail=(uint32_t)a->bavail;b->files=(uint32_t)a->files;b->ffree=(uint32_t)a->ffree;b->favail=(uint32_t)a->favail;b->fsid=a->fsid;b->flag=a->flag;b->namemax=a->namemax;return 0;}
static int statvfs_(const char *p,fs_vfs *s){fs_vfs64 a;return statvfs64_(p,&a)?-1:vfs32(&a,s);}
static int fstatvfs_(int fd,fs_vfs *s){fs_vfs64 a;return fstatvfs64_(fd,&a)?-1:vfs32(&a,s);}
static void to_sfs64(const fs_vfs64 *a,fs_sfs64 *s){memset(s,0,sizeof *s);s->type=0x5346544e;s->bsize=a->bsize;s->blocks=a->blocks;s->bfree=a->bfree;s->bavail=a->bavail;s->files=a->files;s->ffree=a->ffree;s->fsid[0]=a->fsid;s->namelen=a->namemax;s->frsize=a->frsize;s->flags=a->flag;}
static int statfs64_(const char *p,fs_sfs64 *s){if(!s)return lx_set_errno(LX_EFAULT);fs_vfs64 a;int r=statvfs64_(p,&a);if(!r)to_sfs64(&a,s);return r;}
static int fstatfs64_(int fd,fs_sfs64 *s){if(!s)return lx_set_errno(LX_EFAULT);fs_vfs64 a;int r=fstatvfs64_(fd,&a);if(!r)to_sfs64(&a,s);return r;}
static int sfs32(const fs_sfs64 *a,fs_sfs *s){if(!s)return lx_set_errno(LX_EFAULT);if(a->blocks>UINT32_MAX)return lx_set_errno(LX_EOVERFLOW);memset(s,0,sizeof *s);s->type=a->type;s->bsize=a->bsize;s->blocks=(uint32_t)a->blocks;s->bfree=(uint32_t)a->bfree;s->bavail=(uint32_t)a->bavail;s->files=(uint32_t)a->files;s->ffree=(uint32_t)a->ffree;memcpy(s->fsid,a->fsid,8);s->namelen=a->namelen;s->frsize=a->frsize;s->flags=a->flags;return 0;}
static int statfs_(const char *p,fs_sfs *s){fs_sfs64 a;return statfs64_(p,&a)?-1:sfs32(&a,s);}
static int fstatfs_(int fd,fs_sfs *s){fs_sfs64 a;return fstatfs64_(fd,&a)?-1:sfs32(&a,s);}

static int faccessat_(int fd,const char *p,int mode,int flags)
{
    if((mode&~7)||(flags&~(AT_SYMLINK_NOFOLLOW_|0x200)))return lx_set_errno(LX_EINVAL);
    wchar_t *w=at_path(fd,p);if(!w)return -1;DWORD a=GetFileAttributesW(w);free(w);
    if(a==INVALID_FILE_ATTRIBUTES)return lx_fail_win32();
    if((mode&2)&&(a&FILE_ATTRIBUTE_READONLY)&&!(a&FILE_ATTRIBUTE_DIRECTORY))return lx_set_errno(LX_EACCES);
    if((mode&1)&&!(a&FILE_ATTRIBUTE_DIRECTORY))return lx_set_errno(LX_EACCES);
    return 0;
}
static int access_(const char *p,int m){return faccessat_(AT_FDCWD_,p,m,0);}
/* Linux reports EEXIST for any existing path (including "/" and drive roots, where Windows says
 * ERROR_ACCESS_DENIED), checked after the failure so the common success path stays one call. */
static int mkdirat_(int fd,const char *p,unsigned mode){wchar_t *w=at_path(fd,p);if(!w)return -1;int r=0;
    if(!CreateDirectoryW(w,NULL)){DWORD e=GetLastError();r=GetFileAttributesW(w)!=INVALID_FILE_ATTRIBUTES?lx_set_errno(LX_EEXIST):lx_set_errno(lx_errno_from_win32(e));}
    free(w);return r;}
static int mkdir_(const char *p,unsigned m){return mkdirat_(AT_FDCWD_,p,m);}
static int unlinkat_(int fd,const char *p,int flags){if(flags&~AT_REMOVEDIR_)return lx_set_errno(LX_EINVAL);wchar_t *w=at_path(fd,p);if(!w)return -1;DWORD a=GetFileAttributesW(w);BOOL ok;
    if(flags&AT_REMOVEDIR_)ok=RemoveDirectoryW(w);else if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY)){if(a&FILE_ATTRIBUTE_REPARSE_POINT)ok=RemoveDirectoryW(w);else {free(w);return lx_set_errno(LX_EISDIR);}}else ok=DeleteFileW(w);
    int r=ok?0:lx_fail_win32();free(w);return r;}
static int unlink_(const char *p){return unlinkat_(AT_FDCWD_,p,0);}
static int rmdir_(const char *p){return unlinkat_(AT_FDCWD_,p,AT_REMOVEDIR_);}
static int remove_(const char *p){wchar_t *w=lx_map_path(p);if(!w)return -1;DWORD a=GetFileAttributesW(w);free(w);return unlinkat_(AT_FDCWD_,p,(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY)&&!(a&FILE_ATTRIBUTE_REPARSE_POINT))?AT_REMOVEDIR_:0);}
static int renameat_(int od,const char *op,int nd,const char *np){wchar_t *a=at_path(od,op);if(!a)return -1;wchar_t *b=at_path(nd,np);if(!b){free(a);return -1;}int r=MoveFileExW(a,b,MOVEFILE_REPLACE_EXISTING)?0:lx_fail_win32();free(a);free(b);return r;}
static int rename_(const char *a,const char *b){return renameat_(AT_FDCWD_,a,AT_FDCWD_,b);}
static int linkat_(int od,const char *op,int nd,const char *np,int flags){if(flags&~0x400)return lx_set_errno(LX_EINVAL);wchar_t *a=at_path(od,op);if(!a)return -1;wchar_t *b=at_path(nd,np);if(!b){free(a);return -1;}int r=CreateHardLinkW(b,a,NULL)?0:lx_fail_win32();free(a);free(b);return r;}
static int link_(const char *a,const char *b){return linkat_(AT_FDCWD_,a,AT_FDCWD_,b,0);}
static int symlinkat_(const char *target,int fd,const char *name)
{
    if(!target)return lx_set_errno(LX_EFAULT);
    wchar_t *w=at_path(fd,name);if(!w)return -1;
    wchar_t *t=(target[0]=='/'||(target[0]&&target[1]==':'))?lx_map_path(target):lx_utf8_to_wide(target);
    if(!t){free(w);return -1;}
    for(wchar_t *c=t;*c;c++)if(*c==L'/')*c=L'\\';
    DWORD a=GetFileAttributesW(t);DWORD flags=2|((a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY))?1:0);
    /* A relative target is relative to the new link's parent, not the Windows cwd. */
    if(target[0]!='/'&&!(target[0]&&target[1]==':')){wchar_t *parent=_wcsdup(w);wchar_t *slash=wcsrchr(parent,L'\\');if(slash){slash[1]=0;size_t n=wcslen(parent)+wcslen(t)+1;wchar_t *full=malloc(n*sizeof *full);if(full){wcscpy(full,parent);wcscat(full,t);a=GetFileAttributesW(full);if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY))flags|=1;free(full);}}free(parent);}
    BOOLEAN ok=CreateSymbolicLinkW(w,t,flags);DWORD e=GetLastError();
    if(!ok&&e==ERROR_INVALID_PARAMETER){ok=CreateSymbolicLinkW(w,t,flags&~2u);e=GetLastError();}
    free(w);free(t);return ok?0:lx_set_errno(e==ERROR_PRIVILEGE_NOT_HELD?LX_EPERM:lx_errno_from_win32(e));
}
static int symlink_(const char *a,const char *b){return symlinkat_(a,AT_FDCWD_,b);}
static long readlinkat_(int fd,const char *p,char *out,unsigned n)
{
    if(!out)return lx_set_errno(LX_EFAULT);if(!n)return lx_set_errno(LX_EINVAL);
    wchar_t *w=at_path(fd,p);if(!w)return -1;HANDLE h=metadata_open(w,1,0);free(w);if(h==INVALID_HANDLE_VALUE)return lx_fail_win32();
    union { uint64_t align; unsigned char b[MAXIMUM_REPARSE_DATA_BUFFER_SIZE]; } data;DWORD got;
    BOOL ok=DeviceIoControl(h,FSCTL_GET_REPARSE_POINT,NULL,0,data.b,sizeof data.b,&got,NULL);DWORD e=GetLastError();CloseHandle(h);
    if(!ok)return lx_set_errno(e==ERROR_NOT_A_REPARSE_POINT?LX_EINVAL:lx_errno_from_win32(e));
    DWORD tag;memcpy(&tag,data.b,4);uint16_t off,len;unsigned start;int relative=0;
    if(tag==IO_REPARSE_TAG_SYMLINK){uint32_t f;memcpy(&f,data.b+16,4);relative=f&1;start=20;memcpy(&off,data.b+12,2);memcpy(&len,data.b+14,2);if(!len){memcpy(&off,data.b+8,2);memcpy(&len,data.b+10,2);}}
    else if(tag==IO_REPARSE_TAG_MOUNT_POINT){start=16;memcpy(&off,data.b+12,2);memcpy(&len,data.b+14,2);if(!len){memcpy(&off,data.b+8,2);memcpy(&len,data.b+10,2);}}
    else return lx_set_errno(LX_EINVAL);
    if(start+off+len>got||len%2)return lx_set_errno(LX_EIO);
    wchar_t *t=malloc(len+2);if(!t)return lx_set_errno(LX_ENOMEM);memcpy(t,data.b+start+off,len);t[len/2]=0;
    wchar_t *v=t;if(!wcsncmp(v,L"\\??\\",4)||!wcsncmp(v,L"\\\\?\\",4))v+=4;
    char *s=relative?lx_wide_to_utf8(v):lx_guest_path(v);free(t);if(!s)return -1;
    for(char *c=s;*c;c++)if(*c=='\\')*c='/';size_t size=strlen(s);if(size>n)size=n;memcpy(out,s,size);free(s);return (long)size;
}
static long readlink_(const char *p,char *b,unsigned n){return readlinkat_(AT_FDCWD_,p,b,n);}
static char *realpath_(const char *p,char *out){wchar_t *w=lx_map_path(p);if(!w)return NULL;DWORD a=GetFileAttributesW(w);free(w);if(a==INVALID_FILE_ATTRIBUTES){lx_fail_win32();return NULL;}char *abs=lx_absolute_path(p);if(!abs)return NULL;if(!out)out=lx_malloc(strlen(abs)+1);if(out)strcpy(out,abs);else lx_set_errno(LX_ENOMEM);free(abs);return out;}
static char *canonicalize_(const char *p){return realpath_(p,NULL);}
static int truncate64_(const char *p,int64_t size){if(size<0)return lx_set_errno(LX_EINVAL);wchar_t *w=lx_map_path(p);if(!w)return -1;HANDLE h=metadata_open(w,0,GENERIC_WRITE);free(w);if(h==INVALID_HANDLE_VALUE)return lx_fail_win32();LARGE_INTEGER pos;pos.QuadPart=size;int r=SetFilePointerEx(h,pos,NULL,FILE_BEGIN)&&SetEndOfFile(h)?0:lx_fail_win32();CloseHandle(h);return r;}
static int truncate_(const char *p,int32_t n){return truncate64_(p,n);}
static int chmodat_(int fd,const char *p,unsigned mode,int flags){if(flags&~AT_SYMLINK_NOFOLLOW_)return lx_set_errno(LX_EINVAL);wchar_t *w=at_path(fd,p);if(!w)return -1;DWORD a=GetFileAttributesW(w);int r;if(a==INVALID_FILE_ATTRIBUTES)r=lx_fail_win32();else{a=(mode&0222)?a&~FILE_ATTRIBUTE_READONLY:a|FILE_ATTRIBUTE_READONLY;r=SetFileAttributesW(w,a)?0:lx_fail_win32();}free(w);return r;}
static int chmod_(const char *p,unsigned m){return chmodat_(AT_FDCWD_,p,m,0);}
static int fchmod_(int fd,unsigned m)
{
    lx_fd *f=lx_fd_get(fd);if(!f)return -1;HANDLE h=attributes_handle(f);if(h==INVALID_HANDLE_VALUE)return lx_fail_win32();
    FILE_BASIC_INFO b;int r=0;
    if(!GetFileInformationByHandleEx(h,FileBasicInfo,&b,sizeof b))r=lx_fail_win32();
    else {b.FileAttributes=(m&0222)?b.FileAttributes&~FILE_ATTRIBUTE_READONLY:b.FileAttributes|FILE_ATTRIBUTE_READONLY;if(!SetFileInformationByHandle(h,FileBasicInfo,&b,sizeof b))r=lx_fail_win32();}
    CloseHandle(h);return r;
}
static int chown_(const char *p,unsigned u,unsigned g){return 0;}
static int fchown_(int fd,unsigned u,unsigned g){return lx_fd_get(fd)?0:-1;}
static int fchownat_(int fd,const char *p,unsigned u,unsigned g,int flags){return 0;}
static volatile LONG mask_=0022;
static unsigned umask_(unsigned m){return (unsigned)InterlockedExchange(&mask_,m&0777);}
static int set_times(HANDLE h,const fs_ts *ts)
{
    FILETIME t[2],now;GetSystemTimeAsFileTime(&now);
    const FILETIME *a=&t[0],*m=&t[1];
    for(int i=0;i<2;i++){
        if(!ts||ts[i].nsec==1073741823)t[i]=now;
        else if(ts[i].nsec==1073741822){if(!i)a=NULL;else m=NULL;}
        else{if(ts[i].nsec<0||ts[i].nsec>=1000000000)return lx_set_errno(LX_EINVAL);uint64_t v=INT64_C(116444736000000000)+(int64_t)ts[i].sec*10000000+ts[i].nsec/100;t[i].dwLowDateTime=(DWORD)v;t[i].dwHighDateTime=(DWORD)(v>>32);}
    }
    return SetFileTime(h,NULL,a,m)?0:lx_fail_win32();
}
static int futimens_(int fd,const fs_ts *t){lx_fd *f=lx_fd_get(fd);if(!f)return -1;HANDLE h=attributes_handle(f);if(h==INVALID_HANDLE_VALUE)return lx_fail_win32();int r=set_times(h,t);CloseHandle(h);return r;}
static int utimensat_(int fd,const char *p,const fs_ts *t,int flags){if(flags&~AT_SYMLINK_NOFOLLOW_)return lx_set_errno(LX_EINVAL);if(!p)return futimens_(fd,t);wchar_t *w=at_path(fd,p);if(!w)return -1;HANDLE h=metadata_open(w,flags&AT_SYMLINK_NOFOLLOW_,FILE_WRITE_ATTRIBUTES);free(w);if(h==INVALID_HANDLE_VALUE)return lx_fail_win32();int r=set_times(h,t);CloseHandle(h);return r;}
static int tv_to_ts(const fs_tv *v,fs_ts t[2]){for(int i=0;i<2;i++){if(v[i].usec<0||v[i].usec>=1000000)return lx_set_errno(LX_EINVAL);t[i].sec=v[i].sec;t[i].nsec=v[i].usec*1000;}return 0;}
static int utimes_(const char *p,const fs_tv *v){fs_ts t[2];if(v&&tv_to_ts(v,t))return -1;return utimensat_(AT_FDCWD_,p,v?t:NULL,0);}
static int lutimes_(const char *p,const fs_tv *v){fs_ts t[2];if(v&&tv_to_ts(v,t))return -1;return utimensat_(AT_FDCWD_,p,v?t:NULL,AT_SYMLINK_NOFOLLOW_);}
static int futimes_(int fd,const fs_tv *v){fs_ts t[2];if(v&&tv_to_ts(v,t))return -1;return futimens_(fd,v?t:NULL);}
static int futimesat_(int fd,const char *p,const fs_tv *v){fs_ts t[2];if(v&&tv_to_ts(v,t))return -1;return utimensat_(fd,p,v?t:NULL,0);}
static int utime_(const char *p,const fs_utimbuf *v){fs_ts t[2];if(v){t[0].sec=v->actime;t[1].sec=v->modtime;t[0].nsec=t[1].nsec=0;}return utimensat_(AT_FDCWD_,p,v?t:NULL,0);}
static int mkfifo_(const char *p,unsigned m){return lx_set_errno(LX_ENOSYS);}
static int mkfifoat_(int fd,const char *p,unsigned m){return lx_set_errno(LX_ENOSYS);}
static long sendfile64_(int out,int in,int64_t *offset,unsigned count)
{
    if(!lx_fd_get(out)||!lx_fd_get(in))return -1;if(offset&&*offset<0)return lx_set_errno(LX_EINVAL);
    int64_t saved=0;if(offset){saved=lx_lseek64(in,0,1);if(saved<0||lx_lseek64(in,*offset,0)<0)return -1;}
    char b[16384];unsigned done=0;int error=0;
    if(count>INT32_MAX)count=INT32_MAX;
    while(done<count){unsigned want=count-done;if(want>sizeof b)want=sizeof b;long n=lx_read(in,b,want);if(n<=0){if(n<0)error=lx_errno;break;}long pos=0;while(pos<n){long w=lx_write(out,b+pos,(unsigned)(n-pos));if(w<=0){error=w<0?lx_errno:LX_EIO;break;}pos+=w;done+=(unsigned)w;}if(pos<n){lx_lseek64(in,pos-n,1);break;}}
    if(offset){*offset+=done;lx_lseek64(in,saved,0);}if(!done&&error)return lx_set_errno(error);return (long)done;
}
static long sendfile_(int o,int i,int32_t *off,unsigned n){int64_t p=off?*off:0;long r=sendfile64_(o,i,off?&p:NULL,n);if(off&&r>=0)*off=(int32_t)p;return r;}
static int fchdir_(int fd){lx_fd *f=lx_fd_get(fd);if(!f)return -1;if(f->kind!=LX_FD_DIR||!f->wpath)return lx_set_errno(LX_ENOTDIR);char *p=lx_guest_path(f->wpath);if(!p)return -1;int r=lx_chdir(p);free(p);return r;}

LX_SYMTAB(fs,
 LX_SYM("__xstat",xstat_),LX_SYM("__lxstat",lxstat_),LX_SYM("__fxstat",fxstat_),LX_SYM("__xstat64",xstat64_),LX_SYM("__lxstat64",lxstat64_),LX_SYM("__fxstat64",fxstat64_),LX_SYM("__fxstatat64",fxstatat64_),
 LX_SYM("__fxstatat",fxstatat_),
 LX_SYM("stat",stat_),LX_SYM("lstat",lstat_),LX_SYM("fstat",fstat_),LX_SYM("fstatat",fstatat_),LX_SYM("stat64",stat64_),LX_SYM("lstat64",lstat64_),LX_SYM("fstat64",fstat64_),LX_SYM("fstatat64",fstatat64_),
 LX_SYM("statvfs",statvfs_),LX_SYM("statvfs64",statvfs64_),LX_SYM("fstatvfs",fstatvfs_),LX_SYM("fstatvfs64",fstatvfs64_),LX_SYM("statfs",statfs_),LX_SYM("statfs64",statfs64_),LX_SYM("fstatfs",fstatfs_),LX_SYM("fstatfs64",fstatfs64_),
 LX_SYM("access",access_),LX_SYM("euidaccess",access_),LX_SYM("eaccess",access_),LX_SYM("faccessat",faccessat_),LX_SYM("faccessat2",faccessat_),LX_SYM("mkdir",mkdir_),LX_SYM("mkdirat",mkdirat_),LX_SYM("rmdir",rmdir_),LX_SYM("unlink",unlink_),LX_SYM("unlinkat",unlinkat_),LX_SYM("remove",remove_),LX_SYM("rename",rename_),LX_SYM("renameat",renameat_),LX_SYM("link",link_),LX_SYM("linkat",linkat_),LX_SYM("symlink",symlink_),LX_SYM("symlinkat",symlinkat_),LX_SYM("readlink",readlink_),LX_SYM("readlinkat",readlinkat_),LX_SYM("realpath",realpath_),LX_SYM("canonicalize_file_name",canonicalize_),
 LX_SYM("truncate",truncate_),LX_SYM("truncate64",truncate64_),LX_SYM("chmod",chmod_),LX_SYM("fchmod",fchmod_),LX_SYM("fchmodat",chmodat_),LX_SYM("chown",chown_),LX_SYM("lchown",chown_),LX_SYM("fchown",fchown_),LX_SYM("fchownat",fchownat_),LX_SYM("umask",umask_),
 LX_SYM("utime",utime_),LX_SYM("utimes",utimes_),LX_SYM("lutimes",lutimes_),LX_SYM("futimes",futimes_),LX_SYM("futimesat",futimesat_),LX_SYM("futimens",futimens_),LX_SYM("utimensat",utimensat_),LX_SYM("mkfifo",mkfifo_),LX_SYM("mkfifoat",mkfifoat_),LX_SYM("sendfile",sendfile_),LX_SYM("sendfile64",sendfile64_),LX_SYM("fchdir",fchdir_)
);
