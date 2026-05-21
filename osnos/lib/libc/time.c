#include <errno.h>
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
