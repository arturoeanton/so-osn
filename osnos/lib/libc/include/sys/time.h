#pragma once

#include <stdint.h>
#include <sys/types.h>

/*
 * Linux x86_64 struct timeval — both fields are 64-bit. Used by
 * select(2). gettimeofday(2) would also live here once we have a
 * real wall clock (for now there's only the monotonic timer).
 */
struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

int gettimeofday(struct timeval *tv, struct timezone *tz);
