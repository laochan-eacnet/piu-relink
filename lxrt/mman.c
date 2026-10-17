/* Linux page mappings. Reservations and live subranges are tracked separately:
 * Windows cannot release a partial mapped view, so holes are protected until its
 * last live page is unmapped. File offset alignment is handled by a larger view.
 * Private views fall back to VirtualAlloc+read when native views are impossible.
 * MAP_FIXED cannot replace a live PE image or reuse an occupied 64K reservation;
 * those requests fail rather than return a different address. MADV_DONTNEED
 * discards anonymous pages; file-backed discard fails EINVAL. Other advice is a
 * hint. mremap grows complete mappings with MAYMOVE; partial mappings fail.
 */
#include "lxrt.h"
#include <stdlib.h>
#include <string.h>

#define PAGE_ 4096u
#define MAP_SHARED_ 1
#define MAP_PRIVATE_ 2
#define MAP_FIXED_ 0x10
#define MAP_ANON_ 0x20
#define FAILED_ ((void *)(intptr_t)-1)
typedef struct mm_owner mm_owner;
typedef struct mm_span { uintptr_t start,end; mm_owner *owner; struct mm_span *next; } mm_span;
struct mm_owner { void *base,*address; size_t reserve,length,live; HANDLE file; int view,prot,flags; int64_t offset; struct mm_owner *next; };
static mm_owner *owners;
static mm_span *spans;
static SRWLOCK mm_lock=SRWLOCK_INIT;
static int munmap_(void *,size_t);
static DWORD protection(int prot,int copy)
{
    if(prot&4)return (prot&2)?(copy?PAGE_EXECUTE_WRITECOPY:PAGE_EXECUTE_READWRITE):(prot&1)?PAGE_EXECUTE_READ:PAGE_EXECUTE;
    return (prot&2)?(copy?PAGE_WRITECOPY:PAGE_READWRITE):(prot&1)?PAGE_READONLY:PAGE_NOACCESS;
}
static size_t round_page(size_t n){return (n+PAGE_-1)&~(PAGE_-1);}
static void *map_error(int e){lx_set_errno(e);return FAILED_;}
static void release_owner(mm_owner *o)
{
    if(o->view)UnmapViewOfFile(o->base);else VirtualFree(o->base,0,MEM_RELEASE);
    if(o->file&&o->file!=INVALID_HANDLE_VALUE)CloseHandle(o->file);free(o);
}
static int overlap(uintptr_t a,uintptr_t b)
{
    for(mm_span *s=spans;s;s=s->next)if(a<s->end&&b>s->start)return 1;return 0;
}
static void *map_handle(void *hint,size_t len,int prot,int flags,HANDLE file,int64_t offset)
{
    if(!len||len>SIZE_MAX-PAGE_||(prot&~7)||offset<0||((uint64_t)offset&(PAGE_-1)))return map_error(LX_EINVAL);
    if((flags&3)!=MAP_SHARED_&&(flags&3)!=MAP_PRIVATE_)return map_error(LX_EINVAL);
    size_t length=round_page(len);uintptr_t target=(uintptr_t)hint;
    if((flags&MAP_FIXED_)&&((target&(PAGE_-1))||target+length<target))return map_error(LX_EINVAL);
    mm_owner *o=calloc(1,sizeof *o);mm_span *s=malloc(sizeof *s);if(!o||!s){free(o);free(s);return map_error(LX_ENOMEM);}
    o->file=NULL;o->length=o->live=length;o->prot=prot;o->flags=flags;o->offset=offset;
    int anon=(flags&MAP_ANON_)!=0;
    if(!anon&&(!file||file==INVALID_HANDLE_VALUE)){free(o);free(s);return map_error(LX_EBADF);}
    if(!anon&&!DuplicateHandle(GetCurrentProcess(),file,GetCurrentProcess(),&o->file,0,FALSE,DUPLICATE_SAME_ACCESS)){int e=lx_errno_from_win32(GetLastError());free(o);free(s);return map_error(e);}
    if(flags&MAP_FIXED_){
        AcquireSRWLockShared(&mm_lock);int overlaps=overlap(target,target+length);ReleaseSRWLockShared(&mm_lock);
        if(overlaps&&munmap_(hint,length)){if(o->file)CloseHandle(o->file);free(o);free(s);return FAILED_;}
    }
    DWORD error=ERROR_NOT_ENOUGH_MEMORY;
    if(anon){
        uintptr_t base=target&~(uintptr_t)65535;size_t delta=target-base;
        o->reserve=length+((flags&MAP_FIXED_)?delta:0);
        void *request=(flags&MAP_FIXED_)?(void *)base:hint;
        o->base=VirtualAlloc(request,o->reserve,MEM_RESERVE|MEM_COMMIT,protection(prot,0));
        if(!o->base&&!(flags&MAP_FIXED_)&&hint)o->base=VirtualAlloc(NULL,o->reserve,MEM_RESERVE|MEM_COMMIT,protection(prot,0));
        if(o->base)o->address=(char *)o->base+((flags&MAP_FIXED_)?delta:0);else error=GetLastError();
    }else{
        uint64_t aligned=(uint64_t)offset&~UINT64_C(65535);size_t delta=(size_t)((uint64_t)offset-aligned);
        if(length>SIZE_MAX-delta){CloseHandle(o->file);free(o);free(s);return map_error(LX_ENOMEM);}
        int copy=(flags&3)==MAP_PRIVATE_;
        DWORD mp=copy?((prot&4)?PAGE_EXECUTE_WRITECOPY:PAGE_WRITECOPY):
            ((prot&4)?((prot&2)?PAGE_EXECUTE_READWRITE:PAGE_EXECUTE_READ):((prot&2)?PAGE_READWRITE:PAGE_READONLY));
        LARGE_INTEGER filesize;
        if(!GetFileSizeEx(file,&filesize)){int e=lx_errno_from_win32(GetLastError());CloseHandle(o->file);free(o);free(s);return map_error(e);}
        uint64_t available=(uint64_t)filesize.QuadPart>aligned?(uint64_t)filesize.QuadPart-aligned:0;
        size_t viewbytes=length+delta;
        if(available<viewbytes)viewbytes=(size_t)available;
        HANDLE mapping=CreateFileMappingW(file,NULL,mp,0,0,NULL);
        if(mapping){
            DWORD access=copy?FILE_MAP_COPY:(prot&2)?FILE_MAP_WRITE:FILE_MAP_READ;if(prot&4)access|=FILE_MAP_EXECUTE;
            void *request=hint?(void *)(target-delta):NULL;
            if(viewbytes&&round_page(viewbytes)>=length+delta&&(!request||!((uintptr_t)request&65535)))o->base=MapViewOfFileEx(mapping,access,(DWORD)(aligned>>32),(DWORD)aligned,viewbytes,request);
            if(!o->base&&!(flags&MAP_FIXED_)&&viewbytes&&round_page(viewbytes)>=length+delta)o->base=MapViewOfFile(mapping,access,(DWORD)(aligned>>32),(DWORD)aligned,viewbytes);
            error=GetLastError();CloseHandle(mapping);
            if(o->base){o->view=1;o->address=(char *)o->base+delta;o->reserve=length+delta;
                DWORD old;if(!VirtualProtect(o->address,length,protection(prot,copy),&old)){error=GetLastError();UnmapViewOfFile(o->base);o->base=NULL;}
            }
        }else error=GetLastError();
        /* MAP_FIXED at a page rather than allocation-granularity address cannot
         * use a native view. Private copies retain the required isolation. */
        if(!o->base&&copy){
            uintptr_t base=target&~(uintptr_t)65535;size_t prefix=(flags&MAP_FIXED_)?target-base:0;
            o->view=0;o->reserve=length+prefix;o->base=VirtualAlloc((flags&MAP_FIXED_)?(void *)base:NULL,o->reserve,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
            if(o->base){o->address=(char *)o->base+prefix;LARGE_INTEGER save={0},pos;pos.QuadPart=offset;
                BOOL havepos=SetFilePointerEx(file,(LARGE_INTEGER){0},&save,FILE_CURRENT);
                BOOL ok=havepos&&SetFilePointerEx(file,pos,NULL,FILE_BEGIN);DWORD got=0;
                if(ok)ok=ReadFile(file,o->address,(DWORD)len,&got,NULL);
                DWORD readerr=GetLastError();if(havepos)SetFilePointerEx(file,save,NULL,FILE_BEGIN);
                DWORD old;if(ok){ok=VirtualProtect(o->address,length,protection(prot,0),&old);if(!ok)readerr=GetLastError();}
                if(!ok){error=readerr;VirtualFree(o->base,0,MEM_RELEASE);o->base=NULL;}
            }else error=GetLastError();
        }
    }
    if(!o->base){if(o->file)CloseHandle(o->file);free(o);free(s);return map_error(lx_errno_from_win32(error));}
    if((flags&MAP_FIXED_)&&o->address!=hint){release_owner(o);free(s);return map_error(LX_ENOMEM);}
    s->start=(uintptr_t)o->address;s->end=s->start+length;s->owner=o;
    AcquireSRWLockExclusive(&mm_lock);o->next=owners;owners=o;s->next=spans;spans=s;ReleaseSRWLockExclusive(&mm_lock);
    if(prot&4)FlushInstructionCache(GetCurrentProcess(),o->address,length);return o->address;
}
static void *mmap64_(void *hint,size_t len,int prot,int flags,int fd,int64_t off)
{
    HANDLE h=NULL;if(!(flags&MAP_ANON_)){lx_fd *f=lx_fd_get(fd);if(!f)return FAILED_;if(f->kind!=LX_FD_FILE)return map_error(LX_ENODEV);if((f->flags&LX_O_ACCMODE)==LX_O_WRONLY)return map_error(LX_EACCES);if((flags&3)==MAP_SHARED_&&(prot&2)&&(f->flags&LX_O_ACCMODE)!=LX_O_RDWR)return map_error(LX_EACCES);h=f->h;}
    return map_handle(hint,len,prot,flags,h,off);
}
static void *mmap_(void *hint,size_t len,int prot,int flags,int fd,int32_t off){return mmap64_(hint,len,prot,flags,fd,off);}
static int munmap_(void *address,size_t len)
{
    uintptr_t a=(uintptr_t)address;if((a&(PAGE_-1))||!len||len>SIZE_MAX-PAGE_)return lx_set_errno(LX_EINVAL);size_t n=round_page(len);uintptr_t b=a+n;if(b<a)return lx_set_errno(LX_EINVAL);
    AcquireSRWLockExclusive(&mm_lock);
    /* Reserve splitting nodes before changing any mapping. */
    mm_span *extra=NULL;for(mm_span *s=spans;s;s=s->next)if(a>s->start&&b<s->end){extra=malloc(sizeof *extra);if(!extra){ReleaseSRWLockExclusive(&mm_lock);return lx_set_errno(LX_ENOMEM);}break;}
    mm_span **link=&spans;
    while(*link){mm_span *s=*link;if(a>=s->end||b<=s->start){link=&s->next;continue;}uintptr_t lo=a>s->start?a:s->start,hi=b<s->end?b:s->end;mm_owner *o=s->owner;o->live-=hi-lo;
        if(o->live){if(o->view){DWORD old;VirtualProtect((void *)lo,hi-lo,PAGE_NOACCESS,&old);}else VirtualFree((void *)lo,hi-lo,MEM_DECOMMIT);}
        if(lo==s->start&&hi==s->end){*link=s->next;free(s);}else if(lo==s->start){s->start=hi;link=&s->next;}else if(hi==s->end){s->end=lo;link=&s->next;}else{*extra=*s;extra->start=hi;extra->next=s->next;s->end=lo;s->next=extra;extra=NULL;link=&s->next;}
    }
    free(extra);mm_owner **op=&owners;while(*op){mm_owner *o=*op;if(!o->live){*op=o->next;release_owner(o);}else op=&o->next;}ReleaseSRWLockExclusive(&mm_lock);return 0;
}
static int mprotect_(void *address,size_t len,int prot)
{
    uintptr_t a=(uintptr_t)address;if((a&(PAGE_-1))||(prot&~7)||len>SIZE_MAX-PAGE_)return lx_set_errno(LX_EINVAL);if(!len)return 0;uintptr_t b=a+round_page(len);if(b<a)return lx_set_errno(LX_EINVAL);
    AcquireSRWLockExclusive(&mm_lock);
    for(uintptr_t p=a;p<b;){MEMORY_BASIC_INFORMATION info;if(!VirtualQuery((void *)p,&info,sizeof info)||info.State!=MEM_COMMIT){ReleaseSRWLockExclusive(&mm_lock);return lx_set_errno(LX_ENOMEM);}uintptr_t end=(uintptr_t)info.BaseAddress+info.RegionSize;if(end>b)end=b;int copy=0;for(mm_owner *o=owners;o;o=o->next)if(o->view&&(o->flags&3)==MAP_PRIVATE_&&p>=(uintptr_t)o->address&&p<(uintptr_t)o->address+o->length){copy=1;break;}DWORD old;if(!VirtualProtect((void *)p,end-p,protection(prot,copy),&old)){int e=lx_errno_from_win32(GetLastError());ReleaseSRWLockExclusive(&mm_lock);return lx_set_errno(e);}p=end;}
    for(mm_owner *o=owners;o;o=o->next)if(a==(uintptr_t)o->address&&b==a+o->length)o->prot=prot;
    ReleaseSRWLockExclusive(&mm_lock);if(prot&4)FlushInstructionCache(GetCurrentProcess(),address,b-a);return 0;
}
static int msync_(void *address,size_t len,int flags)
{
    uintptr_t a=(uintptr_t)address,b=a+len;if((a&(PAGE_-1))||b<a||(flags&~7)||((flags&1)&&(flags&4)))return lx_set_errno(LX_EINVAL);
    AcquireSRWLockShared(&mm_lock);uintptr_t p=a;int error=0;while(p<b){mm_span *found=NULL;for(mm_span *s=spans;s;s=s->next)if(p>=s->start&&p<s->end){found=s;break;}if(!found){error=LX_ENOMEM;break;}uintptr_t end=found->end<b?found->end:b;mm_owner *o=found->owner;if(o->view&&(o->flags&3)==MAP_SHARED_){if(!FlushViewOfFile((void *)p,end-p)||((flags&4)&&!FlushFileBuffers(o->file))){error=lx_errno_from_win32(GetLastError());break;}}p=end;}ReleaseSRWLockShared(&mm_lock);return error?lx_set_errno(error):0;
}
static int madvise_(void *address,size_t len,int advice)
{
    uintptr_t a=(uintptr_t)address;if((a&(PAGE_-1))||len>SIZE_MAX-PAGE_||a+round_page(len)<a)return lx_set_errno(LX_EINVAL);
    if(advice<0||advice>4)return lx_set_errno(LX_EINVAL);
    if(advice!=4)return 0;size_t n=round_page(len);if(!n)return 0;uintptr_t b=a+n;
    AcquireSRWLockExclusive(&mm_lock);uintptr_t p=a;int error=0;
    while(p<b){mm_span *s;for(s=spans;s;s=s->next)if(p>=s->start&&p<s->end)break;if(!s){error=LX_ENOMEM;break;}uintptr_t end=s->end<b?s->end:b;mm_owner *o=s->owner;
        if(o->flags&MAP_ANON_){if(!VirtualFree((void *)p,end-p,MEM_DECOMMIT)||!VirtualAlloc((void *)p,end-p,MEM_COMMIT,protection(o->prot,0))){error=lx_errno_from_win32(GetLastError());break;}}
        else {error=LX_EINVAL;break;}p=end;
    }ReleaseSRWLockExclusive(&mm_lock);return error?lx_set_errno(error):0;
}
static int posix_madvise_(void *p,size_t n,int a){int saved=lx_errno;int r=madvise_(p,n,a);int e=r?lx_errno:0;lx_errno=saved;return e;}
static int mlock_(const void *p,size_t n){return VirtualLock((void *)p,n)?0:lx_fail_win32();}
static int munlock_(const void *p,size_t n){return VirtualUnlock((void *)p,n)?0:lx_fail_win32();}
static void *mremap_(void *old,size_t oldlen,size_t newlen,int flags,...)
{
    if((flags&~3)||((flags&2)&&!(flags&1))||!oldlen||!newlen||oldlen>SIZE_MAX-PAGE_||newlen>SIZE_MAX-PAGE_||((uintptr_t)old&(PAGE_-1)))return map_error(LX_EINVAL);
    void *fixed=NULL;if(flags&2){va_list ap;va_start(ap,flags);fixed=va_arg(ap,void *);va_end(ap);if(((uintptr_t)fixed&(PAGE_-1))||((uintptr_t)fixed<(uintptr_t)old+round_page(oldlen)&&(uintptr_t)fixed+round_page(newlen)>(uintptr_t)old))return map_error(LX_EINVAL);}
    size_t on=round_page(oldlen),nn=round_page(newlen);AcquireSRWLockShared(&mm_lock);mm_owner *found=NULL;for(mm_owner *o=owners;o;o=o->next)if(o->address==old&&o->length==on&&o->live==on){found=o;break;}
    if(!found){ReleaseSRWLockShared(&mm_lock);return map_error(LX_EFAULT);}int prot=found->prot,mflags=found->flags;int64_t off=found->offset;HANDLE file=NULL;if(found->file)DuplicateHandle(GetCurrentProcess(),found->file,GetCurrentProcess(),&file,0,FALSE,DUPLICATE_SAME_ACCESS);ReleaseSRWLockShared(&mm_lock);
    if(!(flags&2)&&nn<=on){if(nn<on)munmap_((char *)old+nn,on-nn);AcquireSRWLockExclusive(&mm_lock);for(mm_owner *o=owners;o;o=o->next)if(o->address==old)o->length=nn;ReleaseSRWLockExclusive(&mm_lock);if(file)CloseHandle(file);return old;}
    if(!(flags&1)){if(file)CloseHandle(file);return map_error(LX_ENOMEM);}
    void *next=map_handle(fixed,newlen,prot,(mflags&~MAP_FIXED_)|((flags&2)?MAP_FIXED_:0),file,off);if(file)CloseHandle(file);if(next==FAILED_)return next;
    if((mflags&3)==MAP_SHARED_&&!(mflags&MAP_ANON_)){munmap_(old,on);return next;}
    size_t copy=on<nn?on:nn;DWORD oldprot=0;
    if(!VirtualProtect(old,copy,PAGE_READONLY,&oldprot)){int e=lx_errno_from_win32(GetLastError());munmap_(next,newlen);return map_error(e);}
    if(mprotect_(next,copy,3)){DWORD ignored;VirtualProtect(old,copy,oldprot,&ignored);int e=lx_errno;munmap_(next,newlen);return map_error(e);}
    memcpy(next,old,copy);DWORD ignored;VirtualProtect(old,copy,oldprot,&ignored);
    if(mprotect_(next,nn,prot)){int e=lx_errno;munmap_(next,newlen);return map_error(e);}
    munmap_(old,on);return next;
}
LX_SYMTAB(mman,LX_SYM("mmap",mmap_),LX_SYM("mmap64",mmap64_),LX_SYM("__mmap",mmap_),LX_SYM("__mmap64",mmap64_),LX_SYM("munmap",munmap_),LX_SYM("mprotect",mprotect_),LX_SYM("msync",msync_),LX_SYM("madvise",madvise_),LX_SYM("posix_madvise",posix_madvise_),LX_SYM("mlock",mlock_),LX_SYM("munlock",munlock_),LX_SYM("mremap",mremap_));
