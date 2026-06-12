#pragma once

#include <stdint.h>

/* CMOS RTC (MC146818) — read once at boot to anchor the wall clock.
 * After rtc_init(), rtc_boot_epoch() returns the UNIX epoch seconds
 * that corresponded to timer_ms()==0, so
 *   now = rtc_boot_epoch() + timer_ms()/1000
 * is real time. Returns 0 if the RTC read failed (callers fall back
 * to boot-relative time, the pre-FASE-15.2 behaviour). */
void    rtc_init(void);
int64_t rtc_boot_epoch(void);
