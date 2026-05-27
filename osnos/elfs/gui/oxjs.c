/*
 * oxjs — Duktape-based JavaScript app runner for Ox.
 *
 * Usage:  oxjs <path/to/app.js>
 *
 * Loads the file, evaluates it with Duktape, and runs an Ox event
 * loop. JS scripts get a small `ox.*` API to create a window and
 * register event callbacks:
 *
 *     const win = ox.window("My App", 400, 300);
 *     win.onPaint(() => {
 *         ox.clear("#202020");
 *         ox.text(10, 20, "hello from JS", "#ffffff");
 *     });
 *     win.onKey((ascii, code) => console.log("key", ascii));
 *     win.onClick((x, y) => console.log("click", x, y));
 *
 * Color strings are CSS-style `#rrggbb`. Numbers map to integer args.
 * `console.log` prints to the parent terminal (stderr).
 */

#include <errno.h>
#include <fcntl.h>
#include <ox.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "duktape.h"

static ox_win_t g_win = -1;
static int      g_w = 400;
static int      g_h = 300;
static duk_context *g_ctx = NULL;

/* All logging goes through libc's ox_log → /dev/ttyS0. */
#define oxlog ox_log

/* Stashed JS callback references. They live in the Duktape heap
 * under the global "_ox_cb_*" stash keys; we look them up at event
 * time and invoke. */
#define CB_PAINT  "_ox_cb_paint"
#define CB_KEY    "_ox_cb_key"
#define CB_CLICK  "_ox_cb_click"
#define CB_TICK   "_ox_cb_tick"

/* --- color parser --------------------------------------------------- */
static uint32_t parse_color(const char *s) {
    if (!s) return OX_RGB(0, 0, 0);
    if (s[0] == '#') s++;
    /* Accept 3-digit (#rgb) or 6-digit (#rrggbb). */
    unsigned r = 0, g = 0, b = 0;
    size_t L = strlen(s);
    if (L == 3) {
        unsigned v = (unsigned)strtoul(s, NULL, 16);
        r = ((v >> 8) & 0xf) * 0x11;
        g = ((v >> 4) & 0xf) * 0x11;
        b = ( v        & 0xf) * 0x11;
    } else {
        unsigned v = (unsigned)strtoul(s, NULL, 16);
        r = (v >> 16) & 0xff;
        g = (v >>  8) & 0xff;
        b =  v        & 0xff;
    }
    return OX_RGB(r, g, b);
}

/* --- ox.* bindings -------------------------------------------------- */

/* ox.clear(color)  — fill the entire window. */
static duk_ret_t bind_clear(duk_context *ctx) {
    if (g_win < 0) return 0;
    const char *col = duk_safe_to_string(ctx, 0);
    ox_draw_rect(g_win, 0, 0, g_w, g_h, parse_color(col));
    return 0;
}

/* ox.rect(x, y, w, h, color) */
static duk_ret_t bind_rect(duk_context *ctx) {
    if (g_win < 0) return 0;
    int x = (int)duk_to_int(ctx, 0);
    int y = (int)duk_to_int(ctx, 1);
    int w = (int)duk_to_int(ctx, 2);
    int h = (int)duk_to_int(ctx, 3);
    const char *col = duk_safe_to_string(ctx, 4);
    ox_draw_rect(g_win, x, y, w, h, parse_color(col));
    return 0;
}

/* ox.text(x, y, str, color) */
static duk_ret_t bind_text(duk_context *ctx) {
    if (g_win < 0) return 0;
    int x = (int)duk_to_int(ctx, 0);
    int y = (int)duk_to_int(ctx, 1);
    const char *s = duk_safe_to_string(ctx, 2);
    const char *col = duk_safe_to_string(ctx, 3);
    ox_draw_text(g_win, x, y, s, parse_color(col));
    return 0;
}

/* ox.present() — flush the backing buffer to the screen. */
static duk_ret_t bind_present(duk_context *ctx) {
    (void)ctx;
    if (g_win < 0) return 0;
    ox_present(g_win);
    return 0;
}

/* Forward decls — needed because bind_window constructs the wrapper
 * object whose methods point to these. */
static duk_ret_t bind_on_paint(duk_context *ctx);
static duk_ret_t bind_on_key  (duk_context *ctx);
static duk_ret_t bind_on_click(duk_context *ctx);
static duk_ret_t bind_on_tick (duk_context *ctx);

/* ox.window(title, w, h) — create the (single) window. Returns a
 * JS object exposing onPaint/onKey/onClick/onTick methods. */
static duk_ret_t bind_window(duk_context *ctx) {
    if (g_win < 0) {
        const char *title = duk_safe_to_string(ctx, 0);
        int w = (int)duk_to_int(ctx, 1);
        int h = (int)duk_to_int(ctx, 2);
        if (w < 64)  w = 64;
        if (h < 48)  h = 48;
        if (w > 1280) w = 1280;
        if (h > 720)  h = 720;
        g_w = w;
        g_h = h;
        g_win = ox_window_create(w, h, title ? title : "JS");
    }
    /* Build the wrapper object. Methods are the same C bindings used
     * by the legacy ox.onPaint/etc API — registration goes to the
     * global stash either way. */
    duk_push_object(ctx);
    duk_push_c_function(ctx, bind_on_paint, 1);
    duk_put_prop_string(ctx, -2, "onPaint");
    duk_push_c_function(ctx, bind_on_key,   1);
    duk_put_prop_string(ctx, -2, "onKey");
    duk_push_c_function(ctx, bind_on_click, 1);
    duk_put_prop_string(ctx, -2, "onClick");
    duk_push_c_function(ctx, bind_on_tick,  1);
    duk_put_prop_string(ctx, -2, "onTick");
    duk_push_int(ctx, g_w);
    duk_put_prop_string(ctx, -2, "w");
    duk_push_int(ctx, g_h);
    duk_put_prop_string(ctx, -2, "h");
    return 1;
}

/* console.log(...args) — print to /dev/ttyS0 via oxlog. */
static duk_ret_t bind_console_log(duk_context *ctx) {
    int n = duk_get_top(ctx);
    char line[512];
    int off = 0;
    off += snprintf(line + off, sizeof(line) - off, "js: ");
    for (int i = 0; i < n; i++) {
        const char *s = duk_safe_to_string(ctx, i);
        off += snprintf(line + off, sizeof(line) - off,
                        "%s%s", i ? " " : "", s);
        if (off >= (int)sizeof(line) - 2) break;
    }
    snprintf(line + off, sizeof(line) - off, "\n");
    oxlog("%s", line);
    return 0;
}

/* Callback stash helpers: store the function at top-of-stack under
 * the given global property name; later lookups push it back. */
static duk_ret_t bind_on_paint(duk_context *ctx) {
    duk_push_global_object(ctx);
    duk_dup(ctx, 0);
    duk_put_prop_string(ctx, -2, CB_PAINT);
    duk_pop(ctx);
    return 0;
}
static duk_ret_t bind_on_key(duk_context *ctx) {
    duk_push_global_object(ctx);
    duk_dup(ctx, 0);
    duk_put_prop_string(ctx, -2, CB_KEY);
    duk_pop(ctx);
    return 0;
}
static duk_ret_t bind_on_click(duk_context *ctx) {
    duk_push_global_object(ctx);
    duk_dup(ctx, 0);
    duk_put_prop_string(ctx, -2, CB_CLICK);
    duk_pop(ctx);
    return 0;
}
static duk_ret_t bind_on_tick(duk_context *ctx) {
    duk_push_global_object(ctx);
    duk_dup(ctx, 0);
    duk_put_prop_string(ctx, -2, CB_TICK);
    duk_pop(ctx);
    return 0;
}

/* Invoke a stashed callback with `nargs` already pushed below it.
 * Pops the args+callback when done. Returns 0 on success. */
static int call_stashed(duk_context *ctx, const char *name, int nargs) {
    duk_push_global_object(ctx);
    if (!duk_get_prop_string(ctx, -1, name) ||
        !duk_is_callable(ctx, -1)) {
        duk_pop_2(ctx);
        /* Also drop the args caller pushed. */
        duk_pop_n(ctx, nargs);
        return -1;
    }
    /* Stack: [args..., global, fn]. Move fn under the args. */
    duk_insert(ctx, -2 - nargs);   /* fn now at -2-nargs from top */
    duk_pop(ctx);                  /* drop global */
    /* Stack: [fn, args...]. Call. */
    if (duk_pcall(ctx, nargs) != 0) {
        oxlog( "oxjs: %s callback threw: %s\n",
                name, duk_safe_to_string(ctx, -1));
    }
    duk_pop(ctx);   /* return value or error */
    return 0;
}

/* --- runtime setup -------------------------------------------------- */

static void setup_runtime(duk_context *ctx) {
    /* Create the `ox` object on globalThis. */
    duk_push_global_object(ctx);
    duk_push_object(ctx);                                /* [global, ox] */

    duk_push_c_function(ctx, bind_window,  3);
    duk_put_prop_string(ctx, -2, "window");
    duk_push_c_function(ctx, bind_clear,   1);
    duk_put_prop_string(ctx, -2, "clear");
    duk_push_c_function(ctx, bind_rect,    5);
    duk_put_prop_string(ctx, -2, "rect");
    duk_push_c_function(ctx, bind_text,    4);
    duk_put_prop_string(ctx, -2, "text");
    duk_push_c_function(ctx, bind_present, 0);
    duk_put_prop_string(ctx, -2, "present");
    duk_push_c_function(ctx, bind_on_paint, 1);
    duk_put_prop_string(ctx, -2, "onPaint");
    duk_push_c_function(ctx, bind_on_key,   1);
    duk_put_prop_string(ctx, -2, "onKey");
    duk_push_c_function(ctx, bind_on_click, 1);
    duk_put_prop_string(ctx, -2, "onClick");
    duk_push_c_function(ctx, bind_on_tick,  1);
    duk_put_prop_string(ctx, -2, "onTick");
    duk_put_prop_string(ctx, -2, "ox");                  /* global.ox = ... */

    /* Create the `console` object. */
    duk_push_object(ctx);
    duk_push_c_function(ctx, bind_console_log, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "log");
    duk_push_c_function(ctx, bind_console_log, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "error");
    duk_put_prop_string(ctx, -2, "console");

    duk_pop(ctx);                                        /* pop global */
}

/* --- file slurp ----------------------------------------------------- */

static char *slurp_file(const char *path, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0 || st.st_size > 1 * 1024 * 1024) {
        close(fd);
        return NULL;
    }
    size_t L = (size_t)st.st_size;
    char *buf = malloc(L + 1);
    if (!buf) { close(fd); return NULL; }
    size_t got = 0;
    while (got < L) {
        long n = read(fd, buf + got, L - got);
        if (n <= 0) { free(buf); close(fd); return NULL; }
        got += (size_t)n;
    }
    buf[L] = 0;
    close(fd);
    if (out_len) *out_len = L;
    return buf;
}

/* --- main ----------------------------------------------------------- */

int main(int argc, char **argv) {
    oxlog("oxjs: argc=%d argv[1]=%s\n",
          argc, (argc > 1 && argv[1]) ? argv[1] : "(null)");
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        oxlog( "usage: oxjs <path/to/app.js>\n");
        return 1;
    }
    size_t src_len = 0;
    char *src = slurp_file(argv[1], &src_len);
    if (!src) {
        oxlog( "oxjs: cannot read %s (errno=%d)\n", argv[1], errno);
        return 1;
    }
    oxlog( "oxjs: script slurped %lu bytes\n",
            (unsigned long)src_len);
    if (ox_init() < 0) {
        oxlog( "oxjs: ox_init failed (errno=%d)\n", errno);
        free(src);
        return 1;
    }
    oxlog( "oxjs: ox_init OK\n");

    g_ctx = duk_create_heap_default();
    if (!g_ctx) {
        oxlog( "oxjs: duk_create_heap failed\n");
        free(src);
        return 1;
    }
    oxlog( "oxjs: duktape heap created\n");
    setup_runtime(g_ctx);
    oxlog( "oxjs: runtime setup done, evaluating script...\n");

    /* Evaluate the script (sets up the window + callbacks). */
    if (duk_peval_lstring(g_ctx, src, src_len) != 0) {
        oxlog( "oxjs: SCRIPT ERROR: %s\n",
                duk_safe_to_string(g_ctx, -1));
        duk_destroy_heap(g_ctx);
        free(src);
        return 1;
    }
    duk_pop(g_ctx);
    free(src);
    oxlog( "oxjs: script eval OK, g_win=%d\n", (int)g_win);

    if (g_win < 0) {
        /* Script didn't open a window — exit quietly. */
        oxlog( "oxjs: no window created — exiting\n");
        duk_destroy_heap(g_ctx);
        return 0;
    }
    /* Initial paint. */
    call_stashed(g_ctx, CB_PAINT, 0);
    ox_present(g_win);

    int frame = 0;
    int running = 1;
    while (running) {
        ox_event_t ev;
        int got = ox_poll_event(&ev);
        if (got) {
            if (ev.type == OX_EV_CLOSE) break;
            if (ev.type == OX_EV_KEY) {
                duk_push_int(g_ctx, ev.ascii);
                duk_push_int(g_ctx, ev.keycode);
                call_stashed(g_ctx, CB_KEY, 2);
                call_stashed(g_ctx, CB_PAINT, 0);
                ox_present(g_win);
            } else if (ev.type == OX_EV_MOUSE &&
                       ev.mouse_kind == OX_MOUSE_DOWN &&
                       (ev.buttons & 0x01)) {
                duk_push_int(g_ctx, ev.x);
                duk_push_int(g_ctx, ev.y);
                call_stashed(g_ctx, CB_CLICK, 2);
                call_stashed(g_ctx, CB_PAINT, 0);
                ox_present(g_win);
            }
            continue;
        }
        /* Idle tick — fire onTick at ~30 Hz; also repaint if it
         * actually consumed the tick (so animation apps work). */
        struct timespec ts = { 0, 33 * 1000000 };
        nanosleep(&ts, 0);
        frame++;
        if (frame >= 1) {
            frame = 0;
            duk_push_int(g_ctx, (int)(time(NULL)));
            call_stashed(g_ctx, CB_TICK, 1);
            call_stashed(g_ctx, CB_PAINT, 0);
            ox_present(g_win);
        }
    }
    ox_window_destroy(g_win);
    duk_destroy_heap(g_ctx);
    return 0;
}
