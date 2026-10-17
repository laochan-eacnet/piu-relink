#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <dirent.h>
#include <glob.h>
#include <utime.h>
#include <stddef.h>
#include <stdio.h>
#define SZ(t,n) _Static_assert(sizeof(t)==n,#t)
#define OFF(t,f,n) _Static_assert(offsetof(t,f)==n,#t "." #f)
SZ(struct stat,88);OFF(struct stat,st_dev,0);OFF(struct stat,st_ino,12);OFF(struct stat,st_mode,16);OFF(struct stat,st_nlink,20);OFF(struct stat,st_uid,24);OFF(struct stat,st_gid,28);OFF(struct stat,st_rdev,32);OFF(struct stat,st_size,44);OFF(struct stat,st_blksize,48);OFF(struct stat,st_blocks,52);OFF(struct stat,st_atim,56);OFF(struct stat,st_mtim,64);OFF(struct stat,st_ctim,72);
SZ(struct stat64,96);OFF(struct stat64,st_dev,0);OFF(struct stat64,__st_ino,12);OFF(struct stat64,st_mode,16);OFF(struct stat64,st_nlink,20);OFF(struct stat64,st_uid,24);OFF(struct stat64,st_gid,28);OFF(struct stat64,st_rdev,32);OFF(struct stat64,st_size,44);OFF(struct stat64,st_blksize,52);OFF(struct stat64,st_blocks,56);OFF(struct stat64,st_atim,64);OFF(struct stat64,st_mtim,72);OFF(struct stat64,st_ctim,80);OFF(struct stat64,st_ino,88);
SZ(struct statvfs,72);OFF(struct statvfs,f_fsid,32);OFF(struct statvfs,f_flag,40);OFF(struct statvfs,f_namemax,44);
SZ(struct statvfs64,96);OFF(struct statvfs64,f_fsid,56);OFF(struct statvfs64,f_flag,64);OFF(struct statvfs64,f_namemax,68);
SZ(struct statfs,64);OFF(struct statfs,f_fsid,28);OFF(struct statfs,f_namelen,36);
SZ(struct statfs64,84);OFF(struct statfs64,f_fsid,48);OFF(struct statfs64,f_namelen,56);
SZ(struct dirent,268);OFF(struct dirent,d_off,4);OFF(struct dirent,d_reclen,8);OFF(struct dirent,d_type,10);OFF(struct dirent,d_name,11);
SZ(struct dirent64,276);OFF(struct dirent64,d_off,8);OFF(struct dirent64,d_reclen,16);OFF(struct dirent64,d_type,18);OFF(struct dirent64,d_name,19);
SZ(glob_t,36);SZ(glob64_t,36);OFF(glob_t,gl_flags,12);OFF(glob_t,gl_closedir,16);OFF(glob_t,gl_stat,32);
SZ(struct timespec,8);SZ(struct timeval,8);SZ(struct utimbuf,8);
int main(void){puts("stat=88 stat64=96 statvfs=72/96 statfs=64/84 dirent=268/276 glob=36");return 0;}
