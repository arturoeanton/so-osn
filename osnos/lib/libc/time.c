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

/* ISO C clock(): microseconds since boot, packed in clock_t. */
clock_t clock(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return (clock_t)-1;
    return (clock_t)(ts.tv_sec * 1000000L + ts.tv_nsec / 1000L);
}

/* difftime: trivial. */
double difftime(time_t b, time_t a) {
    return (double)b - (double)a;
}

/* mktime: struct tm → time_t. Without an RTC we can't honor a real
 * calendar; we just compute "seconds since 2026-01-01 UTC" using
 * the simplified epoch from localtime/gmtime. Good enough for
 * Lua's os.time() round-trips within one boot. */
static int days_in_month(int year, int mon) {
    static const int dm[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (mon != 1) return dm[mon];
    /* Feb: leap year? Gregorian rule. */
    int y = year + 1900;
    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    return 28 + leap;
}

time_t mktime(struct tm *t) {
    if (!t) return (time_t)-1;
    long days = 0;
    for (int y = 2026; y < t->tm_year + 1900; y++) {
        days += ((y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365);
    }
    for (int m = 0; m < t->tm_mon; m++) days += days_in_month(t->tm_year, m);
    days += t->tm_mday - 1;
    long secs = days * 86400L
              + (long)t->tm_hour * 3600L
              + (long)t->tm_min  * 60L
              + (long)t->tm_sec;
    return (time_t)secs;
}

/* strftime: subset that Lua's os.date() uses. Format specifiers
 * not in the supported list expand to a literal `%X`. Returns
 * the number of bytes written (excluding NUL) or 0 on overflow. */
static int put_int_padded(char *out, size_t cap, size_t *pos,
                            int v, int width, char pad) {
    if (v < 0) return 0;
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    else while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
    while (n < width) tmp[n++] = pad;
    while (n-- > 0) {
        if (*pos >= cap) return 0;
        out[(*pos)++] = tmp[n];
    }
    return 1;
}

static int put_str(char *out, size_t cap, size_t *pos, const char *s) {
    while (*s) {
        if (*pos >= cap) return 0;
        out[(*pos)++] = *s++;
    }
    return 1;
}

size_t strftime(char *buf, size_t buf_size, const char *fmt,
                const struct tm *t) {
    if (!buf || buf_size == 0 || !fmt || !t) return 0;
    static const char *months_short[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    static const char *months_long[] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    static const char *days_short[] = {
        "Sun","Mon","Tue","Wed","Thu","Fri","Sat"
    };
    static const char *days_long[] = {
        "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
    };

    size_t pos = 0;
    while (*fmt) {
        if (*fmt != '%') {
            if (pos >= buf_size - 1) return 0;
            buf[pos++] = *fmt++;
            continue;
        }
        fmt++;
        char c = *fmt++;
        switch (c) {
        case 'Y': put_int_padded(buf, buf_size - 1, &pos,
                                  t->tm_year + 1900, 4, '0'); break;
        case 'm': put_int_padded(buf, buf_size - 1, &pos,
                                  t->tm_mon + 1, 2, '0'); break;
        case 'd': put_int_padded(buf, buf_size - 1, &pos,
                                  t->tm_mday, 2, '0'); break;
        case 'H': put_int_padded(buf, buf_size - 1, &pos,
                                  t->tm_hour, 2, '0'); break;
        case 'M': put_int_padded(buf, buf_size - 1, &pos,
                                  t->tm_min,  2, '0'); break;
        case 'S': put_int_padded(buf, buf_size - 1, &pos,
                                  t->tm_sec,  2, '0'); break;
        case 'j': put_int_padded(buf, buf_size - 1, &pos,
                                  t->tm_yday + 1, 3, '0'); break;
        case 'a': put_str(buf, buf_size - 1, &pos,
                          days_short[t->tm_wday & 7]); break;
        case 'A': put_str(buf, buf_size - 1, &pos,
                          days_long[t->tm_wday & 7]); break;
        case 'b': put_str(buf, buf_size - 1, &pos,
                          months_short[t->tm_mon % 12]); break;
        case 'B': put_str(buf, buf_size - 1, &pos,
                          months_long[t->tm_mon % 12]); break;
        case 'p': put_str(buf, buf_size - 1, &pos,
                          t->tm_hour < 12 ? "AM" : "PM"); break;
        case '%':
            if (pos >= buf_size - 1) return 0;
            buf[pos++] = '%';
            break;
        default:
            /* Unknown spec — emit literal "%X". */
            if (pos >= buf_size - 2) return 0;
            buf[pos++] = '%';
            buf[pos++] = c;
            break;
        }
    }
    buf[pos] = 0;
    return pos;
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
