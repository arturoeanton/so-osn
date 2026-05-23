#include <errno.h>
#include <sys/time.h>
#include <time.h>

#include "syscall.h"

/*
 * Time-of-day routines. osnos has no RTC, so all of these report
 * seconds (and ns) since boot. Useful for elapsed-time arithmetic,
 * not for absolute calendar dates.
 */

time_t time(time_t *t) {
    long r = osnos_syscall1(SYS_TIME, (long)t);
    if (r < 0) { errno = (int)(-r); return (time_t)-1; }
    return (time_t)r;
}

int clock_gettime(int clk_id, struct timespec *tp) {
    long r = osnos_syscall2(SYS_CLOCK_GETTIME, (long)clk_id, (long)tp);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

/* localtime / gmtime: osnos has no RTC, so we map "seconds since
 * boot" through a synthetic epoch. Just enough for callers that
 * only need `__DATE__` / `__TIME__` macro values (like TCC). */
static struct tm g_tm_cache;

static struct tm *fill_tm(const time_t *t, struct tm *out) {
    if (!t || !out) return 0;
    /* Reference epoch: 2026-01-01 00:00:00 UTC. We pretend osnos
     * booted that day at midnight, add `*t` seconds of uptime. */
    long secs = (long)*t;
    int sec  = (int)( secs            % 60); secs /= 60;
    int min  = (int)( secs            % 60); secs /= 60;
    int hour = (int)( secs            % 24); secs /= 24;
    /* `secs` now = days since 2026-01-01. Don't bother with leap
     * years past the first one — pin to Jan 1 + day-of-year. */
    int doy  = (int)secs;
    out->tm_sec  = sec;
    out->tm_min  = min;
    out->tm_hour = hour;
    out->tm_mday = 1 + (doy % 365);
    out->tm_mon  = 0;
    out->tm_year = 2026 - 1900;
    out->tm_wday = (4 + doy) % 7;     /* 2026-01-01 was Thursday */
    out->tm_yday = doy % 365;
    out->tm_isdst = 0;
    return out;
}

struct tm *localtime(const time_t *t) {
    return fill_tm(t, &g_tm_cache);
}

struct tm *localtime_r(const time_t *t, struct tm *out) {
    return fill_tm(t, out);
}

struct tm *gmtime(const time_t *t) {
    return fill_tm(t, &g_tm_cache);     /* no timezone — UTC */
}

/* gettimeofday — micro-resolution wall clock. osnos exposes only
 * monotonic-since-boot, so we read clock_gettime(CLOCK_MONOTONIC)
 * and split into sec/usec. Good enough for TCC's timing and any
 * "elapsed since X" math. */
int gettimeofday(struct timeval *tv, struct timezone *tz) {
    (void)tz;
    if (!tv) { errno = EFAULT; return -1; }
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    tv->tv_sec  = (int64_t)ts.tv_sec;
    tv->tv_usec = (int64_t)(ts.tv_nsec / 1000);
    return 0;
}
