#define _GNU_SOURCE
#include <time.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
_Static_assert(sizeof(time_t) == 4, "time_t");
_Static_assert(sizeof(struct timespec) == 8, "timespec");
_Static_assert(sizeof(struct timeval) == 8, "timeval");
_Static_assert(sizeof(struct tm) == 44, "tm");
_Static_assert(offsetof(struct tm, tm_gmtoff) == 36, "tm_gmtoff");
_Static_assert(offsetof(struct tm, tm_zone) == 40, "tm_zone");
_Static_assert(sizeof(struct tms) == 16, "tms");
_Static_assert(sizeof(struct rusage) == 72, "rusage");
#define CHECK(x) do { if (!(x)) { printf("FAIL time line %d\n", __LINE__); exit(1); } } while (0)
static int64_t ns(const struct timespec *t) { return (int64_t)t->tv_sec * 1000000000 + t->tv_nsec; }
int main(void)
{
    const clockid_t ids[] = {CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW, CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID, CLOCK_BOOTTIME};
    struct timespec a, b, res;
    for (unsigned j = 0; j < sizeof(ids)/sizeof(ids[0]); ++j) {
        CHECK(clock_getres(ids[j], &res) == 0 && res.tv_sec >= 0 && res.tv_nsec >= 0 && res.tv_nsec < 1000000000);
        CHECK(clock_gettime(ids[j], &a) == 0);
        for (int i = 0; i < 100; ++i) { CHECK(clock_gettime(ids[j], &b) == 0 && ns(&b) >= ns(&a)); a = b; }
    }
    CHECK(clock_gettime(-123, &a) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &a) == 0);
    puts("clock IDs resolution monotonicity syscall: pass");
    struct timespec ten_ms = {0, 10000000}, rem = {9, 9};
    /* Warm the translated call path before judging the actual sleep interval. */
    CHECK(nanosleep(&ten_ms, NULL) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0); CHECK(nanosleep(&ten_ms, &rem) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
    CHECK(ns(&b) - ns(&a) >= 8000000 && ns(&b) - ns(&a) <= 12000000);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0); b = a; b.tv_nsec += 10000000;
    if (b.tv_nsec >= 1000000000) { ++b.tv_sec; b.tv_nsec -= 1000000000; }
    CHECK(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &b, NULL) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0 && ns(&a) >= ns(&b));
    struct timespec bad = {0, 1000000000};
    CHECK(nanosleep(&bad, NULL) == -1 && errno == EINVAL);
    CHECK(clock_nanosleep(CLOCK_MONOTONIC, 0, &bad, NULL) == EINVAL);
    ten_ms.tv_nsec = 1000000; CHECK(syscall(SYS_nanosleep, &ten_ms, NULL) == 0);
    CHECK(usleep(1000) == 0 && sleep(0) == 0);
    puts("10ms sleep within 2ms absolute sleep validation: pass");
    time_t now, now2; struct timeval tv;
    time_t returned = time(&now);
    CHECK(returned == now); CHECK(gettimeofday(&tv, NULL) == 0);
    now2 = time(NULL); CHECK(tv.tv_sec >= now && tv.tv_sec <= now2 && tv.tv_usec >= 0 && tv.tv_usec < 1000000);
    struct tms ticks; struct rusage usage;
    CHECK(times(&ticks) != (clock_t)-1); CHECK(clock() >= 0);
    CHECK(getrusage(RUSAGE_SELF, &usage) == 0); CHECK(usage.ru_utime.tv_sec >= 0 && usage.ru_maxrss >= 0);
    CHECK(getrusage(RUSAGE_THREAD, &usage) == 0);
    puts("wall time process thread accounting: pass");
    time_t stamp = 1582979696;
    struct tm utc, local;
    CHECK(gmtime_r(&stamp, &utc) == &utc);
    CHECK(utc.tm_year == 120 && utc.tm_mon == 1 && utc.tm_mday == 29 && utc.tm_hour == 12 && utc.tm_min == 34 && utc.tm_sec == 56);
    CHECK(timegm(&utc) == stamp);
    CHECK(localtime_r(&stamp, &local) == &local); CHECK(mktime(&local) == stamp);
    CHECK(timelocal(&local) == stamp);
    CHECK(gmtime(&stamp)->tm_mday == 29); CHECK(localtime(&stamp) != NULL);
    CHECK(difftime(stamp + 100, stamp) == 100.0);
    struct tm normalize = utc; normalize.tm_mon = 13; normalize.tm_mday = 1; normalize.tm_isdst = 0;
    CHECK(timegm(&normalize) != (time_t)-1 && normalize.tm_year == 121 && normalize.tm_mon == 1);
    puts("calendar UTC local normalization roundtrip: pass");
    char text[512], small[2], asc[26], ct[26];
    CHECK(asctime_r(&utc, asc) == asc && strcmp(asc, "Sat Feb 29 12:34:56 2020\n") == 0);
    CHECK(strcmp(asctime(&utc), asc) == 0);
    CHECK(ctime_r(&stamp, ct) == ct && strcmp(ct, asctime(&local)) == 0); CHECK(strcmp(ctime(&stamp), ct) == 0);
    const char *formats[] = {"%a %A %b %B %h", "%Y-%m-%d %H:%M:%S %w %u %j", "%c|%x|%X|%r|%R|%T", "%C %D %e %F %I %p %U %W %%", "%G-W%V-%u %g", "%z %Z %s", "%n%t"};
    for (unsigned i = 0; i < sizeof(formats)/sizeof(formats[0]); ++i) {
        CHECK(strftime(text, sizeof(text), formats[i], &utc) > 0);
        printf("strftime %u: %s\n", i, text);
    }
    CHECK(strftime(small, sizeof(small), "%Y", &utc) == 0);
    CHECK(strftime(text, sizeof(text), "", &utc) == 0 && !text[0]);
    tzset(); CHECK(tzname[0] != NULL && tzname[1] != NULL);
    puts("asctime ctime timezone data: pass");
    return 0;
}
