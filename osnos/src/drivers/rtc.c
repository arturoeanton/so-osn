/*
 * rtc.c — CMOS RTC (MC146818) one-shot read at boot (FASE 15.2).
 *
 * Gives osnos a real wall clock: sys_time / sys_clock_gettime add
 * rtc_boot_epoch() to the PIT-based uptime. QEMU's RTC ticks in UTC
 * by default (-rtc base=utc), so the reported epoch matches the
 * host. Needed by TLS certificate validation (BearSSL X.509 expiry
 * checks) among others.
 */

#include "rtc.h"
#include <stddef.h>
#include "serial.h"
#include "../micro/timer.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static int64_t g_boot_epoch; /* 0 = RTC unavailable */

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, (uint8_t)(0x80 | reg));   /* NMI disabled bit set */
    return inb(CMOS_DATA);
}

static int rtc_updating(void) {
    return (cmos_read(0x0A) & 0x80) != 0;
}

typedef struct {
    uint8_t sec, min, hour, day, month, year, century;
} rtc_raw_t;

static void rtc_read_raw(rtc_raw_t *r) {
    r->sec     = cmos_read(0x00);
    r->min     = cmos_read(0x02);
    r->hour    = cmos_read(0x04);
    r->day     = cmos_read(0x07);
    r->month   = cmos_read(0x08);
    r->year    = cmos_read(0x09);
    r->century = cmos_read(0x32);   /* may be garbage on some boards */
}

static int raw_equal(const rtc_raw_t *a, const rtc_raw_t *b) {
    return a->sec == b->sec && a->min == b->min && a->hour == b->hour &&
           a->day == b->day && a->month == b->month && a->year == b->year;
}

static uint8_t bcd2bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

/* Tiny serial logger — the kernel has no printf; this is only for
 * the one-line boot diagnostic. */
static void log_str(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    serial_puts(s, n);
}

static void log_num(int64_t v) {
    char buf[24];
    int i = sizeof(buf);
    int neg = v < 0;
    if (neg) v = -v;
    buf[--i] = 0;
    do { buf[--i] = (char)('0' + (v % 10)); v /= 10; } while (v && i > 0);
    if (neg && i > 0) buf[--i] = '-';
    log_str(buf + i);
}

/* Howard Hinnant's days_from_civil — civil date → days since
 * 1970-01-01 (negative OK, not needed here). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);                  /* [0,399] */
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;      /* [0,146096] */
    return era * 146097 + (int64_t)doe - 719468;
}

void rtc_init(void) {
    /* Wait out an in-progress update, then double-read until two
     * consecutive snapshots agree (classic MC146818 race dance). */
    int spins = 0;
    while (rtc_updating() && spins++ < 100000) { /* spin */ }

    rtc_raw_t a, b;
    rtc_read_raw(&a);
    for (int tries = 0; tries < 8; tries++) {
        rtc_read_raw(&b);
        if (raw_equal(&a, &b)) break;
        a = b;
    }

    uint8_t status_b = cmos_read(0x0B);
    int is_bcd  = !(status_b & 0x04);
    int is_12h  = !(status_b & 0x02);

    uint8_t hour_raw = a.hour;
    uint8_t pm = 0;
    if (is_12h) {
        pm = (uint8_t)(hour_raw & 0x80);
        hour_raw &= 0x7F;
    }

    unsigned sec   = is_bcd ? bcd2bin(a.sec)   : a.sec;
    unsigned min   = is_bcd ? bcd2bin(a.min)   : a.min;
    unsigned hour  = is_bcd ? bcd2bin(hour_raw): hour_raw;
    unsigned day   = is_bcd ? bcd2bin(a.day)   : a.day;
    unsigned month = is_bcd ? bcd2bin(a.month) : a.month;
    unsigned year  = is_bcd ? bcd2bin(a.year)  : a.year;
    unsigned cent  = is_bcd ? bcd2bin(a.century) : a.century;

    if (is_12h) {
        if (pm && hour != 12) hour += 12;
        if (!pm && hour == 12) hour = 0;
    }

    int64_t full_year;
    if (cent >= 19 && cent <= 21) full_year = (int64_t)cent * 100 + year;
    else                          full_year = 2000 + year;   /* sane default */

    /* Sanity: reject obvious garbage so callers can fall back. */
    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 60 ||
        full_year < 2000 || full_year > 2100) {
        log_str("rtc: implausible CMOS date - wall clock disabled\n");
        g_boot_epoch = 0;
        return;
    }

    int64_t days = days_from_civil(full_year, month, day);
    int64_t epoch_now = days * 86400 + (int64_t)hour * 3600 +
                        (int64_t)min * 60 + sec;
    g_boot_epoch = epoch_now - (int64_t)(timer_ms() / 1000);
    log_str("rtc: ");
    log_num(full_year); log_str("-"); log_num(month); log_str("-");
    log_num(day); log_str(" "); log_num(hour); log_str(":");
    log_num(min); log_str(":"); log_num(sec);
    log_str(" UTC epoch="); log_num(epoch_now); log_str("\n");
}

int64_t rtc_boot_epoch(void) {
    return g_boot_epoch;
}
