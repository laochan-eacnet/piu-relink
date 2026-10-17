/* Per-thread Linux errno and Win32 error translation. */
#include "lxrt.h"

static __declspec(thread) int tls_errno;

int *lx_errno_location(void)
{
    return &tls_errno;
}

int lx_set_errno(int e)
{
    tls_errno = e;
    return -1;
}

int lx_errno_from_win32(DWORD err)
{
    switch (err) {
    case ERROR_SUCCESS: return 0;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_INVALID_NAME:
    case ERROR_MOD_NOT_FOUND: return LX_ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_NETWORK_ACCESS_DENIED: return LX_EACCES;
    case ERROR_INVALID_HANDLE: return LX_EBADF;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_COMMITMENT_LIMIT: return LX_ENOMEM;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return LX_EEXIST;
    case ERROR_DIR_NOT_EMPTY: return LX_ENOTEMPTY;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return LX_ENOSPC;
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA: return LX_EPIPE;
    case ERROR_NOT_SAME_DEVICE: return LX_EXDEV;
    case ERROR_DIRECTORY: return LX_ENOTDIR;
    case ERROR_FILENAME_EXCED_RANGE: return LX_ENAMETOOLONG;
    case ERROR_WRITE_PROTECT: return LX_EROFS;
    case ERROR_NEGATIVE_SEEK:
    case ERROR_INVALID_PARAMETER: return LX_EINVAL;
    case ERROR_TOO_MANY_OPEN_FILES: return LX_EMFILE;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED: return LX_ENOSYS;
    case ERROR_BUSY:
    case ERROR_DRIVE_LOCKED: return LX_EBUSY;
    case ERROR_TIMEOUT:
    case WAIT_TIMEOUT: return LX_ETIMEDOUT;
    case ERROR_CANT_RESOLVE_FILENAME: return LX_ELOOP;
    default: return LX_EIO;
    }
}

int lx_fail_win32(void)
{
    return lx_set_errno(lx_errno_from_win32(GetLastError()));
}

static int *errno_location_(void)
{
    return &tls_errno;
}

LX_SYMTAB(errnum, /* not "errno": UCRT defines errno as a macro */
    LX_SYM("__errno_location", errno_location_),
);
