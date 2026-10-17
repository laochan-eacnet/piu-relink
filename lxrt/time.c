/* glibc 2.31 i386 time ABI; guest time_t is signed 32-bit and tm is 44 bytes.
 * Windows 10 high-resolution waitable timers provide sleeps, with no guest signal
 * interruption/cancellation. RAW shares the unslewed, suspend-excluding MONOTONIC
 * source; BOOTTIME includes suspend. RAW sleep is unsupported, as on Linux.
 * Calendar conversion uses Windows local/dynamic TZ rules, not the guest TZ
 * environment; zone labels are Windows UTF-8 names, not POSIX abbreviations.
 * Leap seconds are not represented. Ambiguous mktime dates prefer standard time;
 * missing hours normalize forward unless tm_isdst explicitly selects an offset.
 * Rusage has real CPU/RSS/page-fault/I/O accounting where Windows exposes it;
 * page faults are unsplit and I/O counts are Windows operation counts. Other
 * counters and the empty guest child aggregate are zero. No UCRT time layouts
 * or heap allocations are exposed to the guest. */
#include "lxrt.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <realtimeapiset.h>
#include <psapi.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#define NS_PER_SEC INT64_C(1000000000)
#define FT_PER_SEC INT64_C(10000000)
#define UNIX_FILETIME INT64_C(116444736000000000)

enum {
    CLOCK_REALTIME_ = 0, CLOCK_MONOTONIC_ = 1,
    CLOCK_PROCESS_CPUTIME_ = 2, CLOCK_THREAD_CPUTIME_ = 3,
    CLOCK_MONOTONIC_RAW_ = 4, CLOCK_BOOTTIME_ = 7,
    TIMER_ABSTIME_ = 1
};

typedef struct { int32_t tv_sec, tv_nsec; } guest_timespec;
typedef struct { int32_t tv_sec, tv_usec; } guest_timeval;
typedef struct { int32_t tz_minuteswest, tz_dsttime; } guest_timezone;
typedef struct {
    int32_t tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int32_t tm_wday, tm_yday, tm_isdst, tm_gmtoff;
    const char *tm_zone;
} guest_tm;
typedef struct { int32_t tms_utime, tms_stime, tms_cutime, tms_cstime; } guest_tms;
typedef struct {
    guest_timeval ru_utime, ru_stime;
    int32_t ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt;
    int32_t ru_nswap, ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv;
    int32_t ru_nsignals, ru_nvcsw, ru_nivcsw;
} guest_rusage;

#define PIN(type, field, off) static_assert(offsetof(type, field) == (off), #type "." #field)
static_assert(sizeof(int32_t) == 4 && sizeof(void *) == 4, "i386 time ABI");
static_assert(sizeof(guest_timespec) == 8, "i386 timespec");
PIN(guest_timespec, tv_sec, 0); PIN(guest_timespec, tv_nsec, 4);
static_assert(sizeof(guest_timeval) == 8, "i386 timeval");
PIN(guest_timeval, tv_sec, 0); PIN(guest_timeval, tv_usec, 4);
static_assert(sizeof(guest_timezone) == 8, "i386 timezone");
PIN(guest_timezone, tz_minuteswest, 0); PIN(guest_timezone, tz_dsttime, 4);
static_assert(sizeof(guest_tm) == 44, "glibc i386 tm");
PIN(guest_tm, tm_sec, 0); PIN(guest_tm, tm_min, 4); PIN(guest_tm, tm_hour, 8);
PIN(guest_tm, tm_mday, 12); PIN(guest_tm, tm_mon, 16); PIN(guest_tm, tm_year, 20);
PIN(guest_tm, tm_wday, 24); PIN(guest_tm, tm_yday, 28); PIN(guest_tm, tm_isdst, 32);
PIN(guest_tm, tm_gmtoff, 36); PIN(guest_tm, tm_zone, 40);
static_assert(sizeof(guest_tms) == 16, "i386 tms");
PIN(guest_tms, tms_utime, 0); PIN(guest_tms, tms_stime, 4);
PIN(guest_tms, tms_cutime, 8); PIN(guest_tms, tms_cstime, 12);
static_assert(sizeof(guest_rusage) == 72, "i386 rusage");
PIN(guest_rusage, ru_utime, 0); PIN(guest_rusage, ru_stime, 8);
PIN(guest_rusage, ru_maxrss, 16); PIN(guest_rusage, ru_ixrss, 20);
PIN(guest_rusage, ru_idrss, 24); PIN(guest_rusage, ru_isrss, 28);
PIN(guest_rusage, ru_minflt, 32); PIN(guest_rusage, ru_majflt, 36);
PIN(guest_rusage, ru_nswap, 40); PIN(guest_rusage, ru_inblock, 44);
PIN(guest_rusage, ru_oublock, 48); PIN(guest_rusage, ru_msgsnd, 52);
PIN(guest_rusage, ru_msgrcv, 56); PIN(guest_rusage, ru_nsignals, 60);
PIN(guest_rusage, ru_nvcsw, 64); PIN(guest_rusage, ru_nivcsw, 68);
#undef PIN

static uint64_t filetime_ticks(const FILETIME *ft)
{
    return ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
}

static int cpu_times(int thread, uint64_t *user, uint64_t *kernel)
{
    FILETIME created, exited, kt, ut;
    BOOL ok = thread ? GetThreadTimes(GetCurrentThread(), &created, &exited, &kt, &ut)
                     : GetProcessTimes(GetCurrentProcess(), &created, &exited, &kt, &ut);
    if (!ok)
        return lx_fail_win32();
    *user = filetime_ticks(&ut);
    *kernel = filetime_ticks(&kt);
    return 0;
}

static int valid_clock(int id)
{
    return id == CLOCK_REALTIME_ || id == CLOCK_MONOTONIC_ || id == CLOCK_MONOTONIC_RAW_ ||
           id == CLOCK_PROCESS_CPUTIME_ || id == CLOCK_THREAD_CPUTIME_ || id == CLOCK_BOOTTIME_;
}

static int clock_ns(int id, int64_t *out)
{
    ULONGLONG ticks;
    switch (id) {
    case CLOCK_REALTIME_: {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        uint64_t raw = filetime_ticks(&ft);
        if (raw > INT64_MAX)
            return lx_set_errno(LX_EOVERFLOW);
        int64_t unix_ticks = (int64_t)raw - UNIX_FILETIME;
        if (unix_ticks > INT64_MAX / 100 || unix_ticks < INT64_MIN / 100)
            return lx_set_errno(LX_EOVERFLOW);
        *out = unix_ticks * 100;
        return 0;
    }
    case CLOCK_MONOTONIC_:
    case CLOCK_MONOTONIC_RAW_:
        /* This QPC-interpolated Windows clock excludes suspended time and is not
         * disciplined by changes to the wall clock. Windows has no separate RAW clock. */
        QueryUnbiasedInterruptTimePrecise(&ticks);
        break;
    case CLOCK_BOOTTIME_:
        QueryInterruptTimePrecise(&ticks);
        break;
    case CLOCK_PROCESS_CPUTIME_:
    case CLOCK_THREAD_CPUTIME_: {
        uint64_t user, kernel;
        if (cpu_times(id == CLOCK_THREAD_CPUTIME_, &user, &kernel))
            return -1;
        ticks = user + kernel;
        break;
    }
    default:
        return lx_set_errno(LX_EINVAL);
    }
    if (ticks > (uint64_t)INT64_MAX / 100)
        return lx_set_errno(LX_EOVERFLOW);
    *out = (int64_t)ticks * 100;
    return 0;
}

static int timespec_from_ns(int64_t ns, guest_timespec *out)
{
    int64_t sec = ns / NS_PER_SEC;
    int64_t sub = ns % NS_PER_SEC;
    if (sub < 0) {
        sub += NS_PER_SEC;
        --sec;
    }
    if (sec < INT32_MIN || sec > INT32_MAX)
        return lx_set_errno(LX_EOVERFLOW);
    out->tv_sec = (int32_t)sec;
    out->tv_nsec = (int32_t)sub;
    return 0;
}

/* Shared with pthread/sem timed waits; intentionally not static. */
int lx_clock_gettime(int id, void *ptr)
{
    int64_t ns;
    if (!ptr)
        return lx_set_errno(LX_EFAULT);
    if (clock_ns(id, &ns))
        return -1;
    return timespec_from_ns(ns, ptr);
}

static int clock_getres_(int id, guest_timespec *out)
{
    if (!valid_clock(id))
        return lx_set_errno(LX_EINVAL);
    if (out) {
        out->tv_sec = 0;
        out->tv_nsec = 100; /* Native counters and FILETIME are in 100-ns units. */
    }
    return 0;
}

static int valid_request(const guest_timespec *req)
{
    return req && req->tv_sec >= 0 && req->tv_nsec >= 0 && req->tv_nsec < NS_PER_SEC;
}

static int sleep_clock(int id, int flags, const guest_timespec *req)
{
    if (!req)
        return lx_set_errno(LX_EFAULT);
    if (!valid_request(req) || (flags & ~TIMER_ABSTIME_))
        return lx_set_errno(LX_EINVAL);
    if (id == CLOCK_THREAD_CPUTIME_ || !valid_clock(id))
        return lx_set_errno(LX_EINVAL);
    if (id == CLOCK_MONOTONIC_RAW_)
        return lx_set_errno(LX_EOPNOTSUPP); /* Linux does not sleep on the RAW clock. */

    /* Linux measures relative REALTIME sleeps against a monotonic clock. */
    int elapsed_clock = id == CLOCK_REALTIME_ && !(flags & TIMER_ABSTIME_) ? CLOCK_MONOTONIC_ : id;
    int64_t now;
    if (clock_ns(elapsed_clock, &now))
        return -1;
    int64_t target = (int64_t)req->tv_sec * NS_PER_SEC + req->tv_nsec;
    if (!(flags & TIMER_ABSTIME_))
        target += now;
    if (target <= now)
        return 0;

    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (!timer)
        return lx_fail_win32();
    int result = 0;
    while (now < target) {
        int64_t remaining = target - now;
        LARGE_INTEGER due;
        if (elapsed_clock == CLOCK_REALTIME_) {
            due.QuadPart = UNIX_FILETIME + (target + 99) / 100;
        } else if (elapsed_clock == CLOCK_BOOTTIME_) {
            FILETIME ft;
            GetSystemTimePreciseAsFileTime(&ft);
            /* Absolute Windows timers become due during suspend, unlike relative timers. */
            due.QuadPart = (int64_t)filetime_ticks(&ft) + (remaining + 99) / 100;
        } else {
            /* A process CPU clock advances only while some process thread runs.
             * Polling its accounting counter is necessary; a wall timer is not a CPU timer. */
            if (elapsed_clock == CLOCK_PROCESS_CPUTIME_ && remaining > INT64_C(1000000))
                remaining = INT64_C(1000000);
            due.QuadPart = -((remaining + 99) / 100);
        }
        if (!SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0)) {
            result = lx_fail_win32();
            break;
        }
        /* An absolute BOOTTIME proxy must be recomputed if wall time moves back.
         * Its timer still handles suspend; this awake-time bound prevents a wall
         * adjustment from indefinitely postponing a suspend-inclusive deadline. */
        DWORD timeout = elapsed_clock == CLOCK_BOOTTIME_ ? 1000 : INFINITE;
        DWORD wait;
        do {
            wait = WaitForSingleObjectEx(timer, timeout, TRUE);
        } while (wait == WAIT_IO_COMPLETION);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) {
            result = wait == WAIT_FAILED ? lx_fail_win32() : lx_set_errno(LX_EIO);
            break;
        }
        if (clock_ns(elapsed_clock, &now)) {
            result = -1;
            break;
        }
    }
    DWORD saved_error = GetLastError();
    CloseHandle(timer);
    SetLastError(saved_error);
    return result;
}

static int clock_nanosleep_(int id, int flags, const guest_timespec *req, guest_timespec *remain)
{
    (void)remain; /* No guest signal delivery currently interrupts a Windows wait. */
    int saved_errno = lx_errno;
    int result = sleep_clock(id, flags, req);
    int error = result ? lx_errno : 0;
    lx_errno = saved_errno; /* Unlike nanosleep, this API returns its error number. */
    return error;
}

int lx_nanosleep(const void *req, void *remain)
{
    (void)remain; /* On successful completion Linux leaves the remainder untouched. */
    return sleep_clock(CLOCK_MONOTONIC_, 0, req);
}

static int usleep_(uint32_t usec)
{
    guest_timespec req = { (int32_t)(usec / 1000000), (int32_t)(usec % 1000000) * 1000 };
    return lx_nanosleep(&req, NULL);
}

static unsigned sleep_(unsigned seconds)
{
    /* sleep(unsigned) can represent a longer interval than signed time_t. */
    unsigned left = seconds;
    while (left) {
        unsigned part = left > INT32_MAX ? INT32_MAX : left;
        guest_timespec req = { (int32_t)part, 0 };
        int64_t start, end;
        if (clock_ns(CLOCK_MONOTONIC_, &start))
            return left;
        if (lx_nanosleep(&req, NULL)) {
            if (!clock_ns(CLOCK_MONOTONIC_, &end) && end > start) {
                uint64_t elapsed = (uint64_t)(end - start) / NS_PER_SEC;
                left -= elapsed < part ? (unsigned)elapsed : part;
            }
            return left;
        }
        left -= part;
    }
    return 0;
}

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return q - (a % b < 0);
}

/* Proleptic Gregorian civil date conversions, with a Unix epoch. */
static int64_t days_from_civil(int64_t year, unsigned month, unsigned day)
{
    year -= month <= 2;
    int64_t era = floor_div(year, 400);
    unsigned yoe = (unsigned)(year - era * 400);
    unsigned mp = month > 2 ? month - 3 : month + 9;
    unsigned doy = (153 * mp + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int tm_from_seconds(int64_t seconds, guest_tm *out)
{
    int64_t days = floor_div(seconds, 86400);
    int64_t daytime = seconds - days * 86400;
    int64_t z = days + 719468;
    int64_t era = floor_div(z, 146097);
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t year = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned day = doy - (153 * mp + 2) / 5 + 1;
    unsigned month = mp < 10 ? mp + 3 : mp - 9;
    year += month <= 2;
    if (year - 1900 < INT32_MIN || year - 1900 > INT32_MAX)
        return lx_set_errno(LX_EOVERFLOW);
    out->tm_sec = (int32_t)(daytime % 60);
    out->tm_min = (int32_t)(daytime / 60 % 60);
    out->tm_hour = (int32_t)(daytime / 3600);
    out->tm_mday = (int32_t)day;
    out->tm_mon = (int32_t)month - 1;
    out->tm_year = (int32_t)(year - 1900);
    out->tm_wday = (int32_t)((days + 4) % 7);
    if (out->tm_wday < 0)
        out->tm_wday += 7;
    out->tm_yday = (int32_t)(days - days_from_civil(year, 1, 1));
    out->tm_isdst = 0;
    out->tm_gmtoff = 0;
    out->tm_zone = "GMT";
    return 0;
}

static int64_t normalized_seconds(const guest_tm *tm)
{
    int64_t month_years = floor_div(tm->tm_mon, 12);
    unsigned month = (unsigned)((int64_t)tm->tm_mon - month_years * 12) + 1;
    int64_t year = (int64_t)tm->tm_year + 1900 + month_years;
    return (days_from_civil(year, month, 1) + (int64_t)tm->tm_mday - 1) * 86400 +
           (int64_t)tm->tm_hour * 3600 + (int64_t)tm->tm_min * 60 + tm->tm_sec;
}

/* These are permanent libc-owned objects, not heap allocations a guest may free. */
static char zone_names[2][128] = { "UTC", "UTC" };
static char *tzname_[2] = { zone_names[0], zone_names[1] };
static int32_t timezone_, daylight_;
static DYNAMIC_TIME_ZONE_INFORMATION local_zone;
static SRWLOCK zone_lock = SRWLOCK_INIT;
static INIT_ONCE zone_once = INIT_ONCE_STATIC_INIT;

static void timezone_name(char *out, const WCHAR *name, LONG bias)
{
    if (name[0] && WideCharToMultiByte(CP_UTF8, 0, name, -1, out, 128, NULL, NULL))
        return;
    int64_t east = -(int64_t)bias;
    uint64_t magnitude = east < 0 ? (uint64_t)-east : (uint64_t)east;
    if (!east)
        strcpy(out, "UTC");
    else
        snprintf(out, 128, "UTC%c%02llu:%02llu", east < 0 ? '-' : '+',
                 (unsigned long long)(magnitude / 60), (unsigned long long)(magnitude % 60));
}

static int reload_timezone(void)
{
    DYNAMIC_TIME_ZONE_INFORMATION zone;
    if (GetDynamicTimeZoneInformation(&zone) == TIME_ZONE_ID_INVALID)
        return lx_fail_win32();
    char names[2][128];
    timezone_name(names[0], zone.StandardName, zone.Bias + zone.StandardBias);
    timezone_name(names[1], zone.DaylightName, zone.Bias + zone.DaylightBias);
    AcquireSRWLockExclusive(&zone_lock);
    local_zone = zone;
    memcpy(zone_names, names, sizeof names);
    char **live_names = lx_data_addr(tzname_);
    live_names[0] = zone_names[0];
    live_names[1] = zone_names[1];
    *(int32_t *)lx_data_addr(&timezone_) = (zone.Bias + zone.StandardBias) * 60;
    *(int32_t *)lx_data_addr(&daylight_) =
        !zone.DynamicDaylightTimeDisabled && zone.DaylightDate.wMonth != 0;
    ReleaseSRWLockExclusive(&zone_lock);
    return 0;
}

static BOOL CALLBACK init_timezone(PINIT_ONCE once, PVOID arg, PVOID *context)
{
    (void)once; (void)arg; (void)context;
    return reload_timezone() == 0;
}

void lx_time_init(void)
{
    InitOnceExecuteOnce(&zone_once, init_timezone, NULL, NULL);
}

static int timezone_snapshot(DYNAMIC_TIME_ZONE_INFORMATION *zone)
{
    if (!InitOnceExecuteOnce(&zone_once, init_timezone, NULL, NULL))
        return -1;
    AcquireSRWLockShared(&zone_lock);
    *zone = local_zone;
    ReleaseSRWLockShared(&zone_lock);
    return 0;
}

static void tzset_(void)
{
    if (InitOnceExecuteOnce(&zone_once, init_timezone, NULL, NULL))
        reload_timezone();
}

static int year_zone(DYNAMIC_TIME_ZONE_INFORMATION *zone, unsigned year, TIME_ZONE_INFORMATION *out)
{
    if (year > 65535)
        return lx_set_errno(LX_EOVERFLOW);
    if (!GetTimeZoneInformationForYear((USHORT)year, zone, out))
        return lx_fail_win32();
    if (zone->DynamicDaylightTimeDisabled) {
        out->DaylightDate.wMonth = 0;
        out->StandardDate.wMonth = 0;
        out->DaylightBias = out->StandardBias;
    }
    return 0;
}

static int local_tm(int64_t seconds, DYNAMIC_TIME_ZONE_INFORMATION *zone, guest_tm *out)
{
    int64_t ticks = seconds * FT_PER_SEC + UNIX_FILETIME;
    if (ticks < 0)
        return lx_set_errno(LX_EOVERFLOW);
    FILETIME ft = { (DWORD)(uint64_t)ticks, (DWORD)((uint64_t)ticks >> 32) };
    SYSTEMTIME utc, local;
    if (!FileTimeToSystemTime(&ft, &utc) ||
        !SystemTimeToTzSpecificLocalTimeEx(zone, &utc, &local))
        return lx_fail_win32();
    int64_t naive = days_from_civil(local.wYear, local.wMonth, local.wDay) * 86400 +
                    local.wHour * 3600 + local.wMinute * 60 + local.wSecond;
    if (tm_from_seconds(naive, out))
        return -1;
    TIME_ZONE_INFORMATION rule;
    if (year_zone(zone, local.wYear, &rule))
        return -1;
    out->tm_gmtoff = (int32_t)(naive - seconds);
    out->tm_isdst = rule.DaylightDate.wMonth && rule.StandardDate.wMonth &&
                    out->tm_gmtoff == -(rule.Bias + rule.DaylightBias) * 60 &&
                    rule.DaylightBias != rule.StandardBias;
    out->tm_zone = ((char **)lx_data_addr(tzname_))[out->tm_isdst != 0];
    return 0;
}

static guest_tm *gmtime_r_(const int32_t *timer, guest_tm *out)
{
    if (!timer || !out) {
        lx_set_errno(LX_EFAULT);
        return NULL;
    }
    return tm_from_seconds(*timer, out) ? NULL : out;
}

static guest_tm *localtime_r_(const int32_t *timer, guest_tm *out)
{
    if (!timer || !out) {
        lx_set_errno(LX_EFAULT);
        return NULL;
    }
    DYNAMIC_TIME_ZONE_INFORMATION zone;
    if (timezone_snapshot(&zone) || local_tm(*timer, &zone, out))
        return NULL;
    return out;
}

/* As in glibc, the non-reentrant functions share their static result. */
static guest_tm calendar_result;
static guest_tm *gmtime_(const int32_t *timer) { return gmtime_r_(timer, &calendar_result); }
static guest_tm *localtime_(const int32_t *timer) { return localtime_r_(timer, &calendar_result); }

static int mktime_value(const guest_tm *input, int64_t *seconds, guest_tm *normalized)
{
    int64_t naive = normalized_seconds(input);
    /* No UTC offset can bring a date farther away than this into signed time_t. */
    if (naive < (int64_t)INT32_MIN - 86400 || naive > (int64_t)INT32_MAX + 86400)
        return lx_set_errno(LX_EOVERFLOW);
    guest_tm civil;
    DYNAMIC_TIME_ZONE_INFORMATION zone;
    TIME_ZONE_INFORMATION rule;
    if (tm_from_seconds(naive, &civil) || timezone_snapshot(&zone) ||
        year_zone(&zone, (unsigned)(civil.tm_year + 1900), &rule))
        return -1;
    int64_t standard = naive + ((int64_t)rule.Bias + rule.StandardBias) * 60;
    int observes_dst = rule.DaylightDate.wMonth && rule.StandardDate.wMonth;
    int64_t summer = naive + ((int64_t)rule.Bias +
                     (observes_dst ? rule.DaylightBias : rule.StandardBias - 60)) * 60;
    int64_t selected = standard;
    if (input->tm_isdst > 0) {
        selected = summer;
    } else if (input->tm_isdst < 0 && observes_dst) {
        guest_tm check;
        if (local_tm(standard, &zone, &check))
            return -1;
        if (normalized_seconds(&check) != naive || check.tm_isdst != 0) {
            if (local_tm(summer, &zone, &check))
                return -1;
            if (normalized_seconds(&check) == naive && check.tm_isdst > 0)
                selected = summer;
            /* A missing local hour is normalized forward using the standard offset. */
        }
    }
    if (selected < INT32_MIN || selected > INT32_MAX)
        return lx_set_errno(LX_EOVERFLOW);
    if (local_tm(selected, &zone, normalized))
        return -1;
    *seconds = selected;
    return 0;
}

static int32_t mktime_(guest_tm *tm)
{
    if (!tm)
        return lx_set_errno(LX_EFAULT);
    int64_t seconds;
    guest_tm normalized;
    if (mktime_value(tm, &seconds, &normalized))
        return -1;
    *tm = normalized;
    return (int32_t)seconds;
}

static int32_t timegm_(guest_tm *tm)
{
    if (!tm)
        return lx_set_errno(LX_EFAULT);
    int64_t seconds = normalized_seconds(tm);
    if (seconds < INT32_MIN || seconds > INT32_MAX)
        return lx_set_errno(LX_EOVERFLOW);
    guest_tm normalized;
    if (tm_from_seconds(seconds, &normalized))
        return -1;
    *tm = normalized;
    return (int32_t)seconds;
}

static int gettimeofday_(guest_timeval *tv, guest_timezone *tz)
{
    if (tv) {
        int64_t ns;
        guest_timespec ts;
        if (clock_ns(CLOCK_REALTIME_, &ns) || timespec_from_ns(ns, &ts))
            return -1;
        tv->tv_sec = ts.tv_sec;
        tv->tv_usec = ts.tv_nsec / 1000;
    }
    if (tz) {
        /* glibc 2.31 treats this obsolete argument as an all-zero timezone. */
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }
    return 0;
}

static int32_t time_(int32_t *out)
{
    guest_timespec ts;
    if (lx_clock_gettime(CLOCK_REALTIME_, &ts))
        return -1;
    if (out)
        *out = ts.tv_sec;
    return ts.tv_sec;
}

static int stime_(const int32_t *ignored)
{
    (void)ignored;
    return lx_set_errno(LX_EPERM);
}

static int settimeofday_(const guest_timeval *tv, const guest_timezone *tz)
{
    (void)tv; (void)tz;
    return lx_set_errno(LX_EPERM);
}

static int32_t times_(guest_tms *out)
{
    uint64_t user, kernel;
    if (cpu_times(0, &user, &kernel))
        return -1;
    if (out) {
        out->tms_utime = (int32_t)(uint32_t)(user / 100000); /* Linux USER_HZ = 100. */
        out->tms_stime = (int32_t)(uint32_t)(kernel / 100000);
        out->tms_cutime = 0; /* Guest fork/wait is unsupported: there are no reaped children. */
        out->tms_cstime = 0;
    }
    return (int32_t)(uint32_t)(GetTickCount64() / 10);
}

static int32_t clock_(void)
{
    uint64_t user, kernel;
    if (cpu_times(0, &user, &kernel))
        return -1;
    uint64_t usec = (user + kernel) / 10; /* glibc CLOCKS_PER_SEC = 1000000. */
    if (usec > INT32_MAX)
        return lx_set_errno(LX_EOVERFLOW);
    return (int32_t)usec;
}

static int timeval_from_ticks(uint64_t ticks, guest_timeval *out)
{
    if (ticks / FT_PER_SEC > INT32_MAX)
        return lx_set_errno(LX_EOVERFLOW);
    out->tv_sec = (int32_t)(ticks / FT_PER_SEC);
    out->tv_usec = (int32_t)(ticks % FT_PER_SEC / 10);
    return 0;
}

static int32_t capped_long(uint64_t value)
{
    return value > INT32_MAX ? INT32_MAX : (int32_t)value;
}

static int getrusage_(int who, guest_rusage *out)
{
    if (who != 0 && who != 1 && who != -1)
        return lx_set_errno(LX_EINVAL);
    if (!out)
        return lx_set_errno(LX_EFAULT);
    guest_rusage usage = { 0 };
    if (who != -1) {
        uint64_t user, kernel;
        if (cpu_times(who == 1, &user, &kernel) ||
            timeval_from_ticks(user, &usage.ru_utime) || timeval_from_ticks(kernel, &usage.ru_stime))
            return -1;
        if (!who) {
            PROCESS_MEMORY_COUNTERS memory;
            memset(&memory, 0, sizeof memory);
            memory.cb = sizeof memory;
            if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof memory))
                return lx_fail_win32();
            usage.ru_maxrss = capped_long((uint64_t)memory.PeakWorkingSetSize / 1024);
            /* Windows reports total page faults, without a soft/hard split here. */
            usage.ru_minflt = capped_long(memory.PageFaultCount);
            IO_COUNTERS io;
            if (!GetProcessIoCounters(GetCurrentProcess(), &io))
                return lx_fail_win32();
            usage.ru_inblock = capped_long(io.ReadOperationCount);
            usage.ru_oublock = capped_long(io.WriteOperationCount);
        }
    }
    /* Unsupported Windows accounting fields are zero, as Linux documents for
     * unmaintained rusage fields. RUSAGE_CHILDREN is empty without guest fork. */
    *out = usage;
    return 0;
}

static const char *const short_days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const long_days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const short_months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *const long_months[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };

static char *asctime_into(const guest_tm *tm, char *out, size_t capacity)
{
    if (!tm || !out) {
        lx_set_errno(!tm ? LX_EINVAL : LX_EFAULT);
        return NULL;
    }
    if (tm->tm_year > INT32_MAX - 1900) {
        lx_set_errno(LX_EOVERFLOW);
        return NULL;
    }
    const char *day = tm->tm_wday >= 0 && tm->tm_wday < 7 ? short_days[tm->tm_wday] : "???";
    const char *month = tm->tm_mon >= 0 && tm->tm_mon < 12 ? short_months[tm->tm_mon] : "???";
    int n = snprintf(out, capacity, "%s %s%3d %.2d:%.2d:%.2d %lld\n", day, month,
                     tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
                     (long long)tm->tm_year + 1900);
    if (n < 0 || (size_t)n >= capacity) {
        lx_set_errno(LX_EOVERFLOW);
        return NULL;
    }
    return out;
}

static char asctime_result[128];
static char *asctime_(const guest_tm *tm) { return asctime_into(tm, asctime_result, sizeof asctime_result); }
static char *asctime_r_(const guest_tm *tm, char *out) { return asctime_into(tm, out, 26); }
static char *ctime_(const int32_t *timer)
{
    guest_tm *tm = localtime_(timer);
    return tm ? asctime_(tm) : NULL;
}
static char *ctime_r_(const int32_t *timer, char *out)
{
    guest_tm tm;
    return localtime_r_(timer, &tm) ? asctime_r_(&tm, out) : NULL;
}

static double difftime_(int32_t later, int32_t earlier)
{
    return (double)later - (double)earlier;
}

typedef struct { char *out; size_t capacity, used; int failed; } time_output;

static void append_time(time_output *out, const char *text, size_t n)
{
    if (out->failed)
        return;
    if (out->used >= out->capacity || n >= out->capacity - out->used) {
        out->failed = 1;
        return;
    }
    memcpy(out->out + out->used, text, n);
    out->used += n;
}

static void pad_time(time_output *out, char c, size_t n)
{
    if (out->failed)
        return;
    if (out->used >= out->capacity || n >= out->capacity - out->used) {
        out->failed = 1;
        return;
    }
    memset(out->out + out->used, c, n);
    out->used += n;
}

static int leap_year(int64_t year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int iso_weeks(int64_t year)
{
    int jan1 = (int)((days_from_civil(year, 1, 1) + 4) % 7);
    if (jan1 < 0)
        jan1 += 7;
    return jan1 == 4 || (jan1 == 3 && leap_year(year)) ? 53 : 52;
}

static void iso_date(const guest_tm *tm, int64_t *year, int *week)
{
    *year = (int64_t)tm->tm_year + 1900;
    int weekday = tm->tm_wday == 0 ? 7 : tm->tm_wday;
    *week = (int)(((int64_t)tm->tm_yday + 11 - weekday) / 7);
    if (*week < 1) {
        --*year;
        *week = iso_weeks(*year);
    } else if (*week > iso_weeks(*year)) {
        ++*year;
        *week = 1;
    }
}

static void format_time(time_output *out, const char *format, const guest_tm *tm)
{
    while (*format && !out->failed) {
        if (*format != '%') {
            const char *start = format;
            while (*format && *format != '%')
                ++format;
            append_time(out, start, (size_t)(format - start));
            continue;
        }
        const char *conversion = format++;
        char pad = 0;
        int no_pad = 0, uppercase = 0, swapcase = 0;
        for (;;) {
            if (*format == '_') pad = ' ';
            else if (*format == '0') pad = '0';
            else if (*format == '-') no_pad = 1;
            else if (*format == '^') uppercase = 1;
            else if (*format == '#') swapcase = 1;
            else break;
            ++format;
        }
        size_t width = 0;
        int explicit_width = 0;
        while (*format >= '0' && *format <= '9') {
            unsigned digit = (unsigned)(*format++ - '0');
            explicit_width = 1;
            width = width > (SIZE_MAX - digit) / 10 ? SIZE_MAX : width * 10 + digit;
        }
        if (*format == 'E' || *format == 'O')
            ++format; /* C-locale alternative eras/digits are the ordinary ones. */
        unsigned colons = 0;
        while (*format == ':') {
            ++colons;
            ++format;
        }
        char code = *format;
        if (code)
            ++format;
        char text[192];
        const char *value = text;
        size_t length = 0;
        int numeric = 0, default_width = 0;
        char default_pad = '0';
        int64_t number = 0, year = (int64_t)tm->tm_year + 1900;
        const char *composite = NULL;
        int64_t iso_year;
        int iso_week;
        if (colons && code != 'z')
            goto unknown;
        switch (code) {
        case 'a': value = tm->tm_wday >= 0 && tm->tm_wday < 7 ? short_days[tm->tm_wday] : "?"; break;
        case 'A': value = tm->tm_wday >= 0 && tm->tm_wday < 7 ? long_days[tm->tm_wday] : "?"; break;
        case 'b': case 'h': value = tm->tm_mon >= 0 && tm->tm_mon < 12 ? short_months[tm->tm_mon] : "?"; break;
        case 'B': value = tm->tm_mon >= 0 && tm->tm_mon < 12 ? long_months[tm->tm_mon] : "?"; break;
        case 'c': composite = "%a %b %e %H:%M:%S %Y"; break;
        case 'C': numeric = 1; number = floor_div(year, 100); default_width = 2; break;
        case 'd': numeric = 1; number = tm->tm_mday; default_width = 2; break;
        case 'D': case 'x': composite = "%m/%d/%y"; break;
        case 'e': numeric = 1; number = tm->tm_mday; default_width = 2; default_pad = ' '; break;
        case 'F': composite = "%Y-%m-%d"; break;
        case 'g': case 'G':
            iso_date(tm, &iso_year, &iso_week);
            numeric = 1;
            number = code == 'g' ? iso_year - floor_div(iso_year, 100) * 100 : iso_year;
            default_width = code == 'g' ? 2 : 4;
            break;
        case 'H': case 'k':
            numeric = 1; number = tm->tm_hour; default_width = 2;
            if (code == 'k') default_pad = ' ';
            break;
        case 'I': case 'l':
            numeric = 1; number = tm->tm_hour % 12;
            if (!number) number = 12;
            default_width = 2;
            if (code == 'l') default_pad = ' ';
            break;
        case 'j': numeric = 1; number = (int64_t)tm->tm_yday + 1; default_width = 3; break;
        case 'm': numeric = 1; number = (int64_t)tm->tm_mon + 1; default_width = 2; break;
        case 'M': numeric = 1; number = tm->tm_min; default_width = 2; break;
        case 'n': value = "\n"; break;
        case 'p': value = tm->tm_hour < 12 ? "AM" : "PM"; break;
        case 'P': value = tm->tm_hour < 12 ? "am" : "pm"; break;
        case 'r': composite = "%I:%M:%S %p"; break;
        case 'R': composite = "%H:%M"; break;
        case 's': {
            guest_tm normalized;
            if (mktime_value(tm, &number, &normalized))
                number = -1;
            numeric = 1;
            break;
        }
        case 'S': numeric = 1; number = tm->tm_sec; default_width = 2; break;
        case 't': value = "\t"; break;
        case 'T': case 'X': composite = "%H:%M:%S"; break;
        case 'u': numeric = 1; number = tm->tm_wday ? tm->tm_wday : 7; break;
        case 'U': numeric = 1; number = ((int64_t)tm->tm_yday + 7 - tm->tm_wday) / 7; default_width = 2; break;
        case 'V':
            iso_date(tm, &iso_year, &iso_week);
            numeric = 1; number = iso_week; default_width = 2;
            break;
        case 'w': numeric = 1; number = tm->tm_wday; break;
        case 'W': numeric = 1; number = ((int64_t)tm->tm_yday + 7 - ((int64_t)tm->tm_wday + 6) % 7) / 7; default_width = 2; break;
        case 'y': numeric = 1; number = year - floor_div(year, 100) * 100; default_width = 2; break;
        case 'Y': numeric = 1; number = year; default_width = 4; break;
        case 'z': {
            if (tm->tm_isdst < 0) { value = ""; break; }
            int64_t offset = tm->tm_gmtoff;
            char sign = offset < 0 ? '-' : '+';
            if (offset < 0) offset = -offset;
            long long hours = (long long)(offset / 3600);
            int minutes = (int)(offset / 60 % 60), seconds = (int)(offset % 60);
            if (colons == 0)
                snprintf(text, sizeof text, "%c%02lld%02d", sign, hours, minutes);
            else if (colons == 1)
                snprintf(text, sizeof text, "%c%02lld:%02d", sign, hours, minutes);
            else if (colons == 2 || (colons == 3 && seconds))
                snprintf(text, sizeof text, "%c%02lld:%02d:%02d", sign, hours, minutes, seconds);
            else if (colons == 3 && minutes)
                snprintf(text, sizeof text, "%c%02lld:%02d", sign, hours, minutes);
            else if (colons == 3)
                snprintf(text, sizeof text, "%c%02lld", sign, hours);
            else goto unknown;
            break;
        }
        case 'Z':
            if (tm->tm_zone)
                value = tm->tm_zone;
            else if (tm->tm_isdst >= 0) {
                lx_time_init();
                value = ((char **)lx_data_addr(tzname_))[tm->tm_isdst != 0];
            } else value = "";
            break;
        case '%': value = "%"; break;
        default:
        unknown:
            append_time(out, conversion, (size_t)(format - conversion));
            continue;
        }
        if (composite) {
            time_output nested = { text, sizeof text, 0, 0 };
            format_time(&nested, composite, tm);
            if (nested.failed) {
                out->failed = 1;
                return;
            }
            text[nested.used] = 0;
        } else if (numeric) {
            snprintf(text, sizeof text, "%lld", (long long)number);
        }
        length = strlen(value);
        if (!explicit_width || (numeric && width < (size_t)default_width))
            width = numeric ? (size_t)default_width : 0;
        if (!pad)
            pad = numeric ? default_pad : ' ';
        size_t padding = !no_pad && width > length ? width - length : 0;
        /* Keep the sign ahead of zero padding for negative years/numbers. */
        if (padding && pad == '0' && numeric && value[0] == '-') {
            append_time(out, "-", 1);
            ++value;
            --length;
        }
        pad_time(out, pad, padding);
        int lowercase = code == 'P' || (swapcase && (code == 'p' || code == 'Z'));
        if (swapcase && strchr("aAbBh", code))
            uppercase = 1;
        if (lowercase)
            uppercase = 0;
        if (uppercase || lowercase) {
            for (size_t i = 0; i < length && !out->failed; ++i) {
                char c = value[i];
                if (uppercase && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                else if (lowercase && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                append_time(out, &c, 1);
            }
        } else {
            append_time(out, value, length);
        }
    }
}

/* Internal locale wrappers use exactly this C-locale formatter. */
size_t lx_strftime(char *dst, size_t capacity, const char *format, const void *ptr)
{
    if (!format || !ptr || (!dst && capacity)) {
        lx_set_errno(LX_EINVAL);
        return 0;
    }
    if (!capacity)
        return 0;
    time_output out = { dst, capacity, 0, 0 };
    format_time(&out, format, ptr);
    dst[out.used] = 0;
    return out.failed ? 0 : out.used;
}

LX_SYMTAB(time,
    LX_SYM("clock_gettime", lx_clock_gettime),
    LX_SYM("__clock_gettime", lx_clock_gettime),
    LX_SYM("clock_getres", clock_getres_),
    LX_SYM("clock_nanosleep", clock_nanosleep_),
    LX_SYM("gettimeofday", gettimeofday_),
    LX_SYM("__gettimeofday", gettimeofday_),
    LX_SYM("time", time_),
    LX_SYM("nanosleep", lx_nanosleep),
    LX_SYM("__nanosleep", lx_nanosleep),
    LX_SYM("usleep", usleep_),
    LX_SYM("sleep", sleep_),
    LX_SYM("times", times_),
    LX_SYM("clock", clock_),
    LX_SYM("getrusage", getrusage_),
    LX_SYM("gmtime", gmtime_), LX_SYM("gmtime_r", gmtime_r_),
    LX_SYM("localtime", localtime_), LX_SYM("localtime_r", localtime_r_),
    LX_SYM("mktime", mktime_), LX_SYM("timelocal", mktime_),
    LX_SYM("timegm", timegm_),
    LX_SYM("asctime", asctime_), LX_SYM("asctime_r", asctime_r_),
    LX_SYM("ctime", ctime_), LX_SYM("ctime_r", ctime_r_),
    LX_SYM("difftime", difftime_), LX_SYM("strftime", lx_strftime),
    LX_SYM("tzset", tzset_), LX_SYM("__tzset", tzset_),
    LX_SYM("tzname", tzname_), LX_SYM("__tzname", tzname_),
    LX_SYM("timezone", &timezone_), LX_SYM("__timezone", &timezone_),
    LX_SYM("daylight", &daylight_), LX_SYM("__daylight", &daylight_),
    LX_SYM("stime", stime_), LX_SYM("settimeofday", settimeofday_)
);
