/*
 * ox_log.c — shared diagnostic logger for Ox apps.
 *
 * Children spawned from oxsrv have a hosed `stderr` (oxsrv took over
 * the framebuffer console; its children inherit an fd 2 that no
 * longer renders or forwards to serial). So `fprintf(stderr, …)` from
 * an Ox app goes into the void.
 *
 * `ox_log` opens /dev/ttyS0 on the first call and writes formatted
 * text directly to it. QEMU's `-serial file:serial.log` captures
 * the byte stream verbatim, so every Ox app's log lands in
 * osnos/serial.log next to oxsrv's own heartbeat output.
 *
 * Convention: prefix every line with `"<app-name>: "` so multi-app
 * traces stay readable. The bigger app traces (oxsrv, oxjs,
 * oxsqliteview) already do this; new callers should too.
 *
 * When we no longer need the firehose, we can:
 *   (a) gate writes on `getenv("OX_LOG")` at the top of ox_log, or
 *   (b) compile this stub away in release builds.
 */

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "include/ox.h"

static int   g_oxlog_fd       = -1;
static int   g_oxlog_inited   = 0;

static void ox_log_init(void) {
    if (g_oxlog_inited) return;
    g_oxlog_inited = 1;
    g_oxlog_fd = open("/dev/ttyS0", O_WRONLY);
}

void ox_log(const char *fmt, ...) {
    ox_log_init();
    if (g_oxlog_fd < 0) return;
    char buf[768];
    va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    write(g_oxlog_fd, buf, (size_t)n);
}
