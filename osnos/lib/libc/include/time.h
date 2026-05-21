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
