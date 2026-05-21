/*
 * tools/date.c — print seconds-since-boot as a human-ish value.
 *
 * osnos doesn't have an RTC yet; time(2) returns seconds since boot.
 * We render that as "uptime: HH:MM:SS" so the output is useful even
 * without wall-clock. -u / +format options are unsupported.
 */

#include <stdio.h>
#include <time.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    time_t now = time(0);
    long s = (long)now;
    long h = s / 3600;
    long m = (s / 60) % 60;
    long ss = s % 60;
    printf("uptime: %02ld:%02ld:%02ld (%ld seconds since boot)\n", h, m, ss, s);
    return 0;
}
