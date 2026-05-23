#pragma once

#include <sys/types.h>

/*
 * struct timespec — layout matches Linux x86_64 + osnos kernel
 * (osnos_timespec_t). Both fields are 8 bytes signed.
 */
struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

int nanosleep(const struct timespec *req, struct timespec *rem);

/*
 * POSIX time(2) — seconds "since the epoch". osnos has no RTC, so
 * we expose seconds since boot. Useful for elapsed-time arithmetic
 * inside one boot, NOT for absolute calendar dates. Same caveat
 * applies to clock_gettime below.
 */
time_t time(time_t *t);

/* clock_gettime clock IDs (subset). */
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

int clock_gettime(int clk_id, struct timespec *tp);

/*
 * struct tm + localtime. osnos has no RTC, so "calendar time" is
 * fictional — localtime returns a fixed reference date (the boot
 * tick converted with /sd ctime as best-effort). Good enough for
 * TCC's `__DATE__` / `__TIME__` macro expansion, not for anything
 * that needs real wall-clock accuracy.
 */
struct tm {
    int tm_sec;     /* 0..60 */
    int tm_min;     /* 0..59 */
    int tm_hour;    /* 0..23 */
    int tm_mday;    /* 1..31 */
    int tm_mon;     /* 0..11 */
    int tm_year;    /* years since 1900 */
    int tm_wday;    /* 0=Sunday..6 */
    int tm_yday;    /* 0..365 */
    int tm_isdst;
};

struct tm *localtime  (const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *out);
struct tm *gmtime     (const time_t *t);
