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
