/*
 * oxjs — Duktape-based JavaScript app runner for Ox.
 *
 * Usage:  oxjs <path/to/app.js>
 *
 * Loads the file, evaluates it with Duktape, and runs an Ox event
 * loop. JS scripts get a rich `ox.*` API:
 *
 *     ox             — drawing + window + events (core)
 *     ox.fs          — readFile/writeFile/listDir/stat/mkdir/unlink/...
 *     ox.os          — exec/spawn/exit/getenv/sleep/hostname
 *     ox.http        — get/post (HTTP client)
 *     ox.net         — tcpConnect/send/recv/listen/accept/udp
 *     ox.draw        — line/circle/pixel/frame (drawing extras)
 *     ox.color       — rgb/hex helpers
 *     ox.sys         — meminfo/tasks/uptime/sysread (sysfs)
 *     ox.time        — now/date/format/sleep
 *     ox.clipboard   — get/set
 *     ox.log         — info/warn/error → /dev/ttyS0
 *     ox.syscall     — raw syscall(num, ...args) + constants
 *     ox.sqlite      — exec/query (shell out to /bin/sqlite3)
 *     ox.ui          — msgbox/prompt (modal helpers, blocking)
 *
 * Color args: CSS-style "#rrggbb" / "#rgb", or integer 0xRRGGBB.
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <ox.h>
#include "duktape.h"

static ox_win_t     g_win = -1;
static int          g_w = 400;
static int          g_h = 300;
static duk_context *g_ctx = NULL;

#define oxlog ox_log

/* Stashed JS callback references on the global object. */
#define CB_PAINT  "_ox_cb_paint"
#define CB_KEY    "_ox_cb_key"
#define CB_CLICK  "_ox_cb_click"
#define CB_MOUSE  "_ox_cb_mouse"
#define CB_TICK   "_ox_cb_tick"

/* -- raw syscall helper for ox.syscall ------------------------------- */
static long ox_syscall6(long n, long a, long b, long c,
                         long d, long e, long f) {
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"(n), "D"(a), "S"(b), "d"(c),
          "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* -- color parsing --------------------------------------------------- */
static uint32_t parse_color(duk_context *ctx, duk_idx_t idx) {
    if (duk_is_number(ctx, idx)) {
        unsigned v = (unsigned)duk_to_int(ctx, idx);
        return OX_RGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
    }
    const char *s = duk_safe_to_string(ctx, idx);
    if (!s) return OX_RGB(0, 0, 0);
    if (s[0] == '#') s++;
    size_t L = strlen(s);
    unsigned v = (unsigned)strtoul(s, NULL, 16);
    if (L == 3) {
        unsigned r = ((v >> 8) & 0xf) * 0x11;
        unsigned g = ((v >> 4) & 0xf) * 0x11;
        unsigned b = ( v        & 0xf) * 0x11;
        return OX_RGB(r, g, b);
    }
    return OX_RGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
}

/* ===================================================================
 * ox (core: window, drawing, event callbacks)
 * =================================================================== */

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
    /* Return a wrapper object with on* methods for nicer JS style. */
    duk_push_object(ctx);
    duk_push_global_object(ctx);
    duk_get_prop_string(ctx, -1, "ox");
    duk_get_prop_string(ctx, -1, "onPaint"); duk_put_prop_string(ctx, -4, "onPaint");
    duk_get_prop_string(ctx, -1, "onKey");   duk_put_prop_string(ctx, -4, "onKey");
    duk_get_prop_string(ctx, -1, "onClick"); duk_put_prop_string(ctx, -4, "onClick");
    duk_get_prop_string(ctx, -1, "onMouse"); duk_put_prop_string(ctx, -4, "onMouse");
    duk_get_prop_string(ctx, -1, "onTick");  duk_put_prop_string(ctx, -4, "onTick");
    duk_pop_2(ctx);
    duk_push_int(ctx, g_w); duk_put_prop_string(ctx, -2, "w");
    duk_push_int(ctx, g_h); duk_put_prop_string(ctx, -2, "h");
    return 1;
}

static duk_ret_t bind_title(duk_context *ctx) {
    if (g_win < 0) return 0;
    const char *t = duk_safe_to_string(ctx, 0);
    if (t) ox_window_set_title(g_win, t);
    return 0;
}

static duk_ret_t bind_size(duk_context *ctx) {
    duk_push_object(ctx);
    duk_push_int(ctx, g_w); duk_put_prop_string(ctx, -2, "w");
    duk_push_int(ctx, g_h); duk_put_prop_string(ctx, -2, "h");
    return 1;
}

static duk_ret_t bind_clear(duk_context *ctx) {
    if (g_win < 0) return 0;
    ox_draw_rect(g_win, 0, 0, g_w, g_h, parse_color(ctx, 0));
    return 0;
}

static duk_ret_t bind_rect(duk_context *ctx) {
    if (g_win < 0) return 0;
    ox_draw_rect(g_win,
        (int)duk_to_int(ctx, 0), (int)duk_to_int(ctx, 1),
        (int)duk_to_int(ctx, 2), (int)duk_to_int(ctx, 3),
        parse_color(ctx, 4));
    return 0;
}

static duk_ret_t bind_text(duk_context *ctx) {
    if (g_win < 0) return 0;
    int x = (int)duk_to_int(ctx, 0);
    int y = (int)duk_to_int(ctx, 1);
    const char *s = duk_safe_to_string(ctx, 2);
    ox_draw_text(g_win, x, y, s ? s : "", parse_color(ctx, 3));
    return 0;
}

static duk_ret_t bind_pixel(duk_context *ctx) {
    if (g_win < 0) return 0;
    ox_draw_rect(g_win,
        (int)duk_to_int(ctx, 0), (int)duk_to_int(ctx, 1),
        1, 1, parse_color(ctx, 2));
    return 0;
}

/* Bresenham line via 1-pixel rects. */
static duk_ret_t bind_line(duk_context *ctx) {
    if (g_win < 0) return 0;
    int x0 = duk_to_int(ctx, 0), y0 = duk_to_int(ctx, 1);
    int x1 = duk_to_int(ctx, 2), y1 = duk_to_int(ctx, 3);
    uint32_t c = parse_color(ctx, 4);
    int dx =  abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        ox_draw_rect(g_win, x0, y0, 1, 1, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    return 0;
}

/* Filled circle (mid-point algorithm with horizontal fill). */
static duk_ret_t bind_circle(duk_context *ctx) {
    if (g_win < 0) return 0;
    int cx = duk_to_int(ctx, 0), cy = duk_to_int(ctx, 1);
    int r  = duk_to_int(ctx, 2);
    uint32_t col = parse_color(ctx, 3);
    if (r < 0) r = -r;
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)(__builtin_sqrt((double)(r * r - dy * dy)));
        ox_draw_rect(g_win, cx - dx, cy + dy, 2 * dx + 1, 1, col);
    }
    return 0;
}

/* Rectangle outline (4 rects). */
static duk_ret_t bind_frame(duk_context *ctx) {
    if (g_win < 0) return 0;
    int x = duk_to_int(ctx, 0), y = duk_to_int(ctx, 1);
    int w = duk_to_int(ctx, 2), h = duk_to_int(ctx, 3);
    uint32_t c = parse_color(ctx, 4);
    int t = duk_get_top(ctx) > 5 ? duk_to_int(ctx, 5) : 1;
    ox_draw_rect(g_win, x,         y,         w, t, c);
    ox_draw_rect(g_win, x,         y + h - t, w, t, c);
    ox_draw_rect(g_win, x,         y,         t, h, c);
    ox_draw_rect(g_win, x + w - t, y,         t, h, c);
    return 0;
}

static duk_ret_t bind_present(duk_context *ctx) {
    (void)ctx;
    if (g_win >= 0) ox_present(g_win);
    return 0;
}

/* Callback stashers — store fn under global[name]. */
#define BIND_CALLBACK(fn_name, key) \
    static duk_ret_t fn_name(duk_context *ctx) { \
        duk_push_global_object(ctx); \
        duk_dup(ctx, 0); \
        duk_put_prop_string(ctx, -2, key); \
        duk_pop(ctx); \
        return 0; \
    }
BIND_CALLBACK(bind_on_paint, CB_PAINT)
BIND_CALLBACK(bind_on_key,   CB_KEY)
BIND_CALLBACK(bind_on_click, CB_CLICK)
BIND_CALLBACK(bind_on_mouse, CB_MOUSE)
BIND_CALLBACK(bind_on_tick,  CB_TICK)

/* Invoke stashed callback. Caller pushed `nargs` args; we pop them. */
static int call_stashed(duk_context *ctx, const char *name, int nargs) {
    duk_push_global_object(ctx);
    if (!duk_get_prop_string(ctx, -1, name) || !duk_is_callable(ctx, -1)) {
        duk_pop_2(ctx);
        duk_pop_n(ctx, nargs);
        return -1;
    }
    duk_insert(ctx, -2 - nargs);
    duk_pop(ctx);
    if (duk_pcall(ctx, nargs) != 0) {
        oxlog("oxjs: %s callback threw: %s\n",
              name, duk_safe_to_string(ctx, -1));
    }
    duk_pop(ctx);
    return 0;
}

/* ===================================================================
 * console (compat with browsers)
 * =================================================================== */

static duk_ret_t bind_console_log(duk_context *ctx) {
    int n = duk_get_top(ctx);
    char line[1024];
    int off = snprintf(line, sizeof(line), "js: ");
    for (int i = 0; i < n && off < (int)sizeof(line) - 2; i++) {
        const char *s = duk_safe_to_string(ctx, i);
        off += snprintf(line + off, sizeof(line) - off,
                        "%s%s", i ? " " : "", s ? s : "(null)");
    }
    if (off < (int)sizeof(line) - 1) line[off++] = '\n';
    line[off] = 0;
    oxlog("%s", line);
    return 0;
}

/* ===================================================================
 * ox.fs — file system
 * =================================================================== */

static duk_ret_t bind_fs_readFile(duk_context *ctx) {
    const char *path = duk_safe_to_string(ctx, 0);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { duk_push_null(ctx); return 1; }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 0 || st.st_size > 16 * 1024 * 1024) {
        close(fd); duk_push_null(ctx); return 1;
    }
    size_t L = (size_t)st.st_size;
    char *buf = malloc(L + 1);
    if (!buf) { close(fd); duk_push_null(ctx); return 1; }
    size_t got = 0;
    while (got < L) {
        long n = read(fd, buf + got, L - got);
        if (n <= 0) break;
        got += n;
    }
    close(fd);
    buf[got] = 0;
    duk_push_lstring(ctx, buf, got);
    free(buf);
    return 1;
}

static duk_ret_t bind_fs_writeFile(duk_context *ctx) {
    const char *path = duk_safe_to_string(ctx, 0);
    duk_size_t L;
    const char *data = duk_safe_to_lstring(ctx, 1, &L);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { duk_push_boolean(ctx, 0); return 1; }
    size_t off = 0;
    while (off < L) {
        long n = write(fd, data + off, L - off);
        if (n <= 0) break;
        off += n;
    }
    close(fd);
    duk_push_boolean(ctx, off == L);
    return 1;
}

static duk_ret_t bind_fs_appendFile(duk_context *ctx) {
    const char *path = duk_safe_to_string(ctx, 0);
    duk_size_t L;
    const char *data = duk_safe_to_lstring(ctx, 1, &L);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { duk_push_boolean(ctx, 0); return 1; }
    size_t off = 0;
    while (off < L) {
        long n = write(fd, data + off, L - off);
        if (n <= 0) break;
        off += n;
    }
    close(fd);
    duk_push_boolean(ctx, off == L);
    return 1;
}

static duk_ret_t bind_fs_listDir(duk_context *ctx) {
    const char *path = duk_safe_to_string(ctx, 0);
    DIR *d = opendir(path);
    if (!d) { duk_push_null(ctx); return 1; }
    duk_push_array(ctx);
    int i = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' && (e->d_name[1] == 0 ||
            (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
        duk_push_string(ctx, e->d_name);
        duk_put_prop_index(ctx, -2, i++);
    }
    closedir(d);
    return 1;
}

static duk_ret_t bind_fs_exists(duk_context *ctx) {
    const char *path = duk_safe_to_string(ctx, 0);
    struct stat st;
    duk_push_boolean(ctx, stat(path, &st) == 0);
    return 1;
}

static duk_ret_t bind_fs_stat(duk_context *ctx) {
    const char *path = duk_safe_to_string(ctx, 0);
    struct stat st;
    if (stat(path, &st) < 0) { duk_push_null(ctx); return 1; }
    duk_push_object(ctx);
    duk_push_int(ctx, (int)st.st_size); duk_put_prop_string(ctx, -2, "size");
    duk_push_boolean(ctx, S_ISDIR(st.st_mode)); duk_put_prop_string(ctx, -2, "isdir");
    duk_push_boolean(ctx, S_ISREG(st.st_mode)); duk_put_prop_string(ctx, -2, "isfile");
    duk_push_uint(ctx, (duk_uint_t)st.st_mtime); duk_put_prop_string(ctx, -2, "mtime");
    duk_push_uint(ctx, (duk_uint_t)st.st_mode); duk_put_prop_string(ctx, -2, "mode");
    return 1;
}

static duk_ret_t bind_fs_mkdir(duk_context *ctx) {
    duk_push_boolean(ctx, mkdir(duk_safe_to_string(ctx, 0), 0755) == 0);
    return 1;
}

static duk_ret_t bind_fs_unlink(duk_context *ctx) {
    duk_push_boolean(ctx, unlink(duk_safe_to_string(ctx, 0)) == 0);
    return 1;
}

static duk_ret_t bind_fs_rmdir(duk_context *ctx) {
    duk_push_boolean(ctx, rmdir(duk_safe_to_string(ctx, 0)) == 0);
    return 1;
}

static duk_ret_t bind_fs_rename(duk_context *ctx) {
    duk_push_boolean(ctx,
        rename(duk_safe_to_string(ctx, 0), duk_safe_to_string(ctx, 1)) == 0);
    return 1;
}

static duk_ret_t bind_fs_chdir(duk_context *ctx) {
    duk_push_boolean(ctx, chdir(duk_safe_to_string(ctx, 0)) == 0);
    return 1;
}

static duk_ret_t bind_fs_cwd(duk_context *ctx) {
    char buf[1024];
    if (getcwd(buf, sizeof(buf))) duk_push_string(ctx, buf);
    else duk_push_string(ctx, "/");
    return 1;
}

/* ===================================================================
 * ox.os — process / env / exec
 * =================================================================== */

static duk_ret_t bind_os_exec(duk_context *ctx) {
    const char *cmd = duk_safe_to_string(ctx, 0);
    FILE *p = popen(cmd, "r");
    if (!p) { duk_push_null(ctx); return 1; }
    char buf[4096];
    char *out = malloc(1);
    size_t out_len = 0;
    if (!out) { pclose(p); duk_push_null(ctx); return 1; }
    out[0] = 0;
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), p);
        if (n == 0) break;
        char *nb = realloc(out, out_len + n + 1);
        if (!nb) { free(out); pclose(p); duk_push_null(ctx); return 1; }
        out = nb;
        memcpy(out + out_len, buf, n);
        out_len += n;
        out[out_len] = 0;
        if (out_len > 8 * 1024 * 1024) break;
    }
    pclose(p);
    duk_push_lstring(ctx, out, out_len);
    free(out);
    return 1;
}

static duk_ret_t bind_os_system(duk_context *ctx) {
    duk_push_int(ctx, system(duk_safe_to_string(ctx, 0)));
    return 1;
}

static duk_ret_t bind_os_exit(duk_context *ctx) {
    int code = duk_get_top(ctx) > 0 ? duk_to_int(ctx, 0) : 0;
    if (g_win >= 0) ox_window_destroy(g_win);
    exit(code);
    return 0;
}

static duk_ret_t bind_os_getpid(duk_context *ctx) {
    duk_push_int(ctx, getpid());
    return 1;
}

static duk_ret_t bind_os_getenv(duk_context *ctx) {
    const char *v = getenv(duk_safe_to_string(ctx, 0));
    if (v) duk_push_string(ctx, v);
    else duk_push_null(ctx);
    return 1;
}

static duk_ret_t bind_os_setenv(duk_context *ctx) {
    duk_push_boolean(ctx,
        setenv(duk_safe_to_string(ctx, 0),
               duk_safe_to_string(ctx, 1), 1) == 0);
    return 1;
}

static duk_ret_t bind_os_sleep(duk_context *ctx) {
    double s = duk_to_number(ctx, 0);
    struct timespec t = { (time_t)s, (long)((s - (time_t)s) * 1e9) };
    nanosleep(&t, NULL);
    return 0;
}

static duk_ret_t bind_os_usleep(duk_context *ctx) {
    long us = duk_to_int(ctx, 0);
    struct timespec t = { us / 1000000, (us % 1000000) * 1000 };
    nanosleep(&t, NULL);
    return 0;
}

static duk_ret_t bind_os_hostname(duk_context *ctx) {
    /* mini-libc no expone gethostname; leemos /etc/hostname directo. */
    int fd = open("/etc/hostname", O_RDONLY);
    if (fd >= 0) {
        char buf[256] = {0};
        long n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            for (long i = 0; i < n; i++)
                if (buf[i] == '\n' || buf[i] == '\r') { buf[i] = 0; break; }
            duk_push_string(ctx, buf);
            return 1;
        }
    }
    duk_push_string(ctx, "osnos");
    return 1;
}

static duk_ret_t bind_os_argv(duk_context *ctx) {
    /* Stashed at startup in oxjs. */
    duk_push_global_object(ctx);
    duk_get_prop_string(ctx, -1, "_ox_argv");
    duk_remove(ctx, -2);
    return 1;
}

/* ===================================================================
 * ox.http — HTTP/1.0 client over TCP
 * =================================================================== */

static int parse_url(const char *url, char *host, size_t hcap,
                     int *port, char *path, size_t pcap) {
    const char *p = url;
    int defport = 80;
    if (strncmp(p, "http://", 7) == 0)  { p += 7; defport = 80; }
    else if (strncmp(p, "https://", 8) == 0) { p += 8; defport = 443; }
    int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi + 1 < (int)hcap) host[hi++] = *p++;
    host[hi] = 0;
    *port = defport;
    if (*p == ':') {
        p++;
        int v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); }
        *port = v;
    }
    if (*p == '/') {
        size_t i = 0;
        while (*p && i + 1 < pcap) path[i++] = *p++;
        path[i] = 0;
    } else { path[0] = '/'; path[1] = 0; }
    return 0;
}

static int http_send_request(const char *url, const char *method,
                              const char *body, const char *content_type,
                              char **out_buf, size_t *out_len) {
    char host[256], path[1024];
    int port;
    if (parse_url(url, host, sizeof(host), &port, path, sizeof(path)) < 0)
        return -1;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char ps[16]; snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { freeaddrinfo(res); return -1; }
    if (connect(s, res->ai_addr, res->ai_addrlen) < 0) {
        close(s); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);

    char req[4096];
    int rlen;
    if (body) {
        rlen = snprintf(req, sizeof(req),
            "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: oxjs/1\r\n"
            "Content-Type: %s\r\nContent-Length: %zu\r\n"
            "Connection: close\r\n\r\n",
            method, path, host,
            content_type ? content_type : "application/octet-stream",
            strlen(body));
    } else {
        rlen = snprintf(req, sizeof(req),
            "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: oxjs/1\r\n"
            "Connection: close\r\n\r\n",
            method, path, host);
    }
    if (write(s, req, rlen) != rlen) { close(s); return -1; }
    if (body && write(s, body, strlen(body)) != (long)strlen(body)) {
        close(s); return -1;
    }

    size_t cap = 16 * 1024, got = 0;
    char *buf = malloc(cap);
    if (!buf) { close(s); return -1; }
    for (;;) {
        if (got + 1 >= cap) {
            cap *= 2;
            if (cap > 8 * 1024 * 1024) break;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); close(s); return -1; }
            buf = nb;
        }
        long n = read(s, buf + got, cap - 1 - got);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EAGAIN) {
                struct timespec t = { 0, 10 * 1000000 };
                nanosleep(&t, NULL);
                continue;
            }
            break;
        }
        got += n;
    }
    close(s);
    buf[got] = 0;
    *out_buf = buf;
    *out_len = got;
    return 0;
}

static void push_http_response(duk_context *ctx, char *raw, size_t len) {
    /* Find header/body split. */
    char *body = NULL;
    size_t body_len = 0;
    for (size_t i = 0; i + 3 < len; i++) {
        if (raw[i] == '\r' && raw[i+1] == '\n' &&
            raw[i+2] == '\r' && raw[i+3] == '\n') {
            body = raw + i + 4;
            body_len = len - i - 4;
            raw[i] = 0;
            break;
        }
    }
    int status = 0;
    /* Parse first line "HTTP/1.x NNN ..." */
    char *sp1 = strchr(raw, ' ');
    if (sp1) status = atoi(sp1 + 1);

    duk_push_object(ctx);
    duk_push_int(ctx, status); duk_put_prop_string(ctx, -2, "status");
    duk_push_lstring(ctx, body ? body : "", body_len);
    duk_put_prop_string(ctx, -2, "body");
    duk_push_string(ctx, raw); duk_put_prop_string(ctx, -2, "headers");
}

static duk_ret_t bind_http_get(duk_context *ctx) {
    const char *url = duk_safe_to_string(ctx, 0);
    char *raw = NULL; size_t len = 0;
    if (http_send_request(url, "GET", NULL, NULL, &raw, &len) < 0) {
        duk_push_null(ctx); return 1;
    }
    push_http_response(ctx, raw, len);
    free(raw);
    return 1;
}

static duk_ret_t bind_http_post(duk_context *ctx) {
    const char *url = duk_safe_to_string(ctx, 0);
    const char *body = duk_safe_to_string(ctx, 1);
    const char *ctype = duk_get_top(ctx) > 2 ? duk_safe_to_string(ctx, 2) : NULL;
    char *raw = NULL; size_t len = 0;
    if (http_send_request(url, "POST", body, ctype, &raw, &len) < 0) {
        duk_push_null(ctx); return 1;
    }
    push_http_response(ctx, raw, len);
    free(raw);
    return 1;
}

/* ===================================================================
 * ox.net — sockets (fd-based)
 * =================================================================== */

static duk_ret_t bind_net_tcpConnect(duk_context *ctx) {
    const char *host = duk_safe_to_string(ctx, 0);
    int port = duk_to_int(ctx, 1);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char ps[16]; snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) {
        duk_push_int(ctx, -1); return 1;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { freeaddrinfo(res); duk_push_int(ctx, -1); return 1; }
    if (connect(s, res->ai_addr, res->ai_addrlen) < 0) {
        close(s); freeaddrinfo(res); duk_push_int(ctx, -1); return 1;
    }
    freeaddrinfo(res);
    duk_push_int(ctx, s);
    return 1;
}

static duk_ret_t bind_net_tcpListen(duk_context *ctx) {
    int port = duk_to_int(ctx, 0);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { duk_push_int(ctx, -1); return 1; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(s); duk_push_int(ctx, -1); return 1;
    }
    if (listen(s, 16) < 0) {
        close(s); duk_push_int(ctx, -1); return 1;
    }
    duk_push_int(ctx, s);
    return 1;
}

static duk_ret_t bind_net_accept(duk_context *ctx) {
    int s = duk_to_int(ctx, 0);
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int c = accept(s, (struct sockaddr *)&sa, &sl);
    if (c < 0) { duk_push_int(ctx, -1); return 1; }
    duk_push_int(ctx, c);
    return 1;
}

static duk_ret_t bind_net_send(duk_context *ctx) {
    int s = duk_to_int(ctx, 0);
    duk_size_t L;
    const char *data = duk_safe_to_lstring(ctx, 1, &L);
    long n = write(s, data, L);
    duk_push_int(ctx, (int)n);
    return 1;
}

static duk_ret_t bind_net_recv(duk_context *ctx) {
    int s = duk_to_int(ctx, 0);
    int max = duk_get_top(ctx) > 1 ? duk_to_int(ctx, 1) : 4096;
    if (max <= 0 || max > 1024 * 1024) max = 4096;
    char *buf = malloc(max);
    if (!buf) { duk_push_null(ctx); return 1; }
    /* Block-retry on EAGAIN (osnos non-blocking semantics). */
    long n = -1;
    for (int i = 0; i < 500; i++) {
        n = read(s, buf, max);
        if (n >= 0) break;
        if (errno != EAGAIN) break;
        struct timespec t = { 0, 10 * 1000000 };
        nanosleep(&t, NULL);
    }
    if (n < 0) { free(buf); duk_push_null(ctx); return 1; }
    duk_push_lstring(ctx, buf, n);
    free(buf);
    return 1;
}

static duk_ret_t bind_net_close(duk_context *ctx) {
    close(duk_to_int(ctx, 0));
    return 0;
}

static duk_ret_t bind_net_udpSend(duk_context *ctx) {
    const char *host = duk_safe_to_string(ctx, 0);
    int port = duk_to_int(ctx, 1);
    duk_size_t L;
    const char *data = duk_safe_to_lstring(ctx, 2, &L);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { duk_push_int(ctx, -1); return 1; }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_aton(host, &sa.sin_addr);
    long n = sendto(s, data, L, 0, (struct sockaddr *)&sa, sizeof(sa));
    close(s);
    duk_push_int(ctx, (int)n);
    return 1;
}

/* ===================================================================
 * ox.color — convenience
 * =================================================================== */

static duk_ret_t bind_color_rgb(duk_context *ctx) {
    int r = duk_to_int(ctx, 0);
    int g = duk_to_int(ctx, 1);
    int b = duk_to_int(ctx, 2);
    char buf[16];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x",
             r & 0xff, g & 0xff, b & 0xff);
    duk_push_string(ctx, buf);
    return 1;
}

static duk_ret_t bind_color_hex(duk_context *ctx) {
    int r = duk_to_int(ctx, 0);
    int g = duk_to_int(ctx, 1);
    int b = duk_to_int(ctx, 2);
    duk_push_int(ctx, ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff));
    return 1;
}

/* ===================================================================
 * ox.sys — /sys readers
 * =================================================================== */

static int slurp_path(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got + 1 < cap) {
        long n = read(fd, buf + got, cap - 1 - got);
        if (n <= 0) break;
        got += n;
    }
    close(fd);
    buf[got] = 0;
    return (int)got;
}

static duk_ret_t bind_sys_sysread(duk_context *ctx) {
    const char *path = duk_safe_to_string(ctx, 0);
    char buf[8192];
    int n = slurp_path(path, buf, sizeof(buf));
    if (n < 0) duk_push_null(ctx);
    else duk_push_lstring(ctx, buf, (size_t)n);
    return 1;
}

static duk_ret_t bind_sys_meminfo(duk_context *ctx) {
    char buf[2048];
    if (slurp_path("/sys/meminfo", buf, sizeof(buf)) < 0) {
        duk_push_null(ctx); return 1;
    }
    duk_push_object(ctx);
    duk_push_string(ctx, buf); duk_put_prop_string(ctx, -2, "raw");
    return 1;
}

static duk_ret_t bind_sys_uptime(duk_context *ctx) {
    char buf[64];
    if (slurp_path("/sys/uptime", buf, sizeof(buf)) < 0) {
        duk_push_null(ctx); return 1;
    }
    duk_push_number(ctx, atof(buf));
    return 1;
}

static duk_ret_t bind_sys_tasks(duk_context *ctx) {
    char buf[8192];
    if (slurp_path("/sys/tasks", buf, sizeof(buf)) < 0) {
        duk_push_null(ctx); return 1;
    }
    duk_push_string(ctx, buf);
    return 1;
}

/* ===================================================================
 * ox.time
 * =================================================================== */

static duk_ret_t bind_time_now(duk_context *ctx) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    duk_push_number(ctx, (double)ts.tv_sec * 1000.0 +
                          (double)ts.tv_nsec / 1e6);
    return 1;
}

static duk_ret_t bind_time_epoch(duk_context *ctx) {
    duk_push_uint(ctx, (duk_uint_t)time(NULL));
    return 1;
}

static duk_ret_t bind_time_date(duk_context *ctx) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    duk_push_string(ctx, buf);
    return 1;
}

static duk_ret_t bind_time_format(duk_context *ctx) {
    time_t t = (time_t)duk_to_int(ctx, 0);
    const char *fmt = duk_safe_to_string(ctx, 1);
    struct tm *tm = localtime(&t);
    char buf[128];
    strftime(buf, sizeof(buf), fmt, tm);
    duk_push_string(ctx, buf);
    return 1;
}

/* ===================================================================
 * ox.clipboard
 * =================================================================== */

static duk_ret_t bind_clipboard_set(duk_context *ctx) {
    duk_size_t L;
    const char *s = duk_safe_to_lstring(ctx, 0, &L);
    duk_push_boolean(ctx, ox_clipboard_set(s, (int)L) == 0);
    return 1;
}

static duk_ret_t bind_clipboard_get(duk_context *ctx) {
    char buf[4096];
    int n = ox_clipboard_get(buf, sizeof(buf));
    if (n < 0) { duk_push_string(ctx, ""); return 1; }
    duk_push_lstring(ctx, buf, n);
    return 1;
}

/* ===================================================================
 * ox.log — stderr-like, /dev/ttyS0
 * =================================================================== */

static duk_ret_t bind_log_info(duk_context *ctx) {
    int n = duk_get_top(ctx);
    char line[1024];
    int off = snprintf(line, sizeof(line), "INFO: ");
    for (int i = 0; i < n && off < (int)sizeof(line) - 2; i++) {
        const char *s = duk_safe_to_string(ctx, i);
        off += snprintf(line + off, sizeof(line) - off, "%s%s", i ? " " : "", s ? s : "");
    }
    if (off < (int)sizeof(line) - 1) line[off++] = '\n';
    line[off] = 0;
    oxlog("%s", line);
    return 0;
}

static duk_ret_t bind_log_warn(duk_context *ctx) {
    int n = duk_get_top(ctx);
    char line[1024];
    int off = snprintf(line, sizeof(line), "WARN: ");
    for (int i = 0; i < n && off < (int)sizeof(line) - 2; i++) {
        const char *s = duk_safe_to_string(ctx, i);
        off += snprintf(line + off, sizeof(line) - off, "%s%s", i ? " " : "", s ? s : "");
    }
    if (off < (int)sizeof(line) - 1) line[off++] = '\n';
    line[off] = 0;
    oxlog("%s", line);
    return 0;
}

static duk_ret_t bind_log_error(duk_context *ctx) {
    int n = duk_get_top(ctx);
    char line[1024];
    int off = snprintf(line, sizeof(line), "ERROR: ");
    for (int i = 0; i < n && off < (int)sizeof(line) - 2; i++) {
        const char *s = duk_safe_to_string(ctx, i);
        off += snprintf(line + off, sizeof(line) - off, "%s%s", i ? " " : "", s ? s : "");
    }
    if (off < (int)sizeof(line) - 1) line[off++] = '\n';
    line[off] = 0;
    oxlog("%s", line);
    return 0;
}

/* ===================================================================
 * ox.syscall — raw syscall (advanced)
 * =================================================================== */

static duk_ret_t bind_syscall(duk_context *ctx) {
    long n = duk_to_int(ctx, 0);
    int nargs = duk_get_top(ctx) - 1;
    long a[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < nargs && i < 6; i++) {
        /* Strings get passed as char* (their internal pointer). */
        if (duk_is_string(ctx, 1 + i)) {
            a[i] = (long)duk_get_string(ctx, 1 + i);
        } else {
            a[i] = (long)duk_to_int(ctx, 1 + i);
        }
    }
    long r = ox_syscall6(n, a[0], a[1], a[2], a[3], a[4], a[5]);
    duk_push_number(ctx, (double)r);
    return 1;
}

/* ===================================================================
 * ox.sqlite — query via shell-out to /bin/sqlite3
 * =================================================================== */

static duk_ret_t bind_sqlite_exec(duk_context *ctx) {
    const char *db = duk_safe_to_string(ctx, 0);
    const char *sql = duk_safe_to_string(ctx, 1);
    char cmd[4096];
    /* Escape simple-quote in SQL by doubling. Keep it simple — apps
     * that need exotic queries can pass SQL via writeFile + .read. */
    char sql_esc[2048];
    int j = 0;
    for (size_t i = 0; sql[i] && j + 2 < (int)sizeof(sql_esc); i++) {
        if (sql[i] == '\'') { sql_esc[j++] = '\''; sql_esc[j++] = '\''; }
        else sql_esc[j++] = sql[i];
    }
    sql_esc[j] = 0;
    snprintf(cmd, sizeof(cmd),
             "/bin/sqlite3 -separator '\t' '%s' '%s' 2>&1", db, sql_esc);
    FILE *p = popen(cmd, "r");
    if (!p) { duk_push_null(ctx); return 1; }
    char *out = malloc(1);
    size_t out_len = 0;
    out[0] = 0;
    char buf[4096];
    for (;;) {
        size_t got = fread(buf, 1, sizeof(buf), p);
        if (got == 0) break;
        char *nb = realloc(out, out_len + got + 1);
        if (!nb) break;
        out = nb;
        memcpy(out + out_len, buf, got);
        out_len += got;
        out[out_len] = 0;
    }
    pclose(p);
    duk_push_lstring(ctx, out, out_len);
    free(out);
    return 1;
}

/* ox.sqlite.query — like exec but split tab-separated rows into array. */
static duk_ret_t bind_sqlite_query(duk_context *ctx) {
    bind_sqlite_exec(ctx);                /* push result string */
    duk_size_t L;
    const char *raw = duk_safe_to_lstring(ctx, -1, &L);
    duk_push_array(ctx);
    int row = 0;
    size_t line_start = 0;
    for (size_t i = 0; i <= L; i++) {
        if (i == L || raw[i] == '\n') {
            if (i > line_start) {
                duk_push_lstring(ctx, raw + line_start, i - line_start);
                duk_put_prop_index(ctx, -2, row++);
            }
            line_start = i + 1;
        }
    }
    duk_remove(ctx, -2);                  /* drop original string */
    return 1;
}

/* ===================================================================
 * ox.ui — modal helpers (msgbox, prompt). Block until user action.
 * =================================================================== */

static void modal_draw_panel(int x, int y, int w, int h,
                              const char *title, const char *body) {
    /* Backdrop */
    ox_draw_rect(g_win, 0, 0, g_w, g_h, OX_RGB(0x00, 0x00, 0x00));
    /* Panel */
    ox_draw_rect(g_win, x, y, w, h, OX_RGB(0xee, 0xee, 0xee));
    ox_draw_rect(g_win, x, y, w, 18, OX_RGB(0xfc, 0xe0, 0x6d));
    ox_draw_text(g_win, x + 6, y + 5, title ? title : "", OX_RGB(0, 0, 0));
    /* Body */
    ox_draw_text(g_win, x + 12, y + 30, body ? body : "", OX_RGB(0x20, 0x20, 0x20));
    /* OK button */
    int bx = x + (w - 64) / 2, by = y + h - 32;
    ox_draw_rect(g_win, bx, by, 64, 22, OX_RGB(0x53, 0x84, 0xe4));
    ox_draw_text(g_win, bx + 24, by + 7, "OK", OX_RGB(255, 255, 255));
    ox_present(g_win);
}

static duk_ret_t bind_ui_msgbox(duk_context *ctx) {
    if (g_win < 0) return 0;
    const char *title = duk_safe_to_string(ctx, 0);
    const char *msg   = duk_safe_to_string(ctx, 1);
    int pw = 400, ph = 160;
    int px = (g_w - pw) / 2, py = (g_h - ph) / 2;
    modal_draw_panel(px, py, pw, ph, title, msg);
    /* Wait for click on OK or any key. */
    for (;;) {
        ox_event_t ev;
        if (!ox_wait_event(&ev)) continue;
        if (ev.type == OX_EV_CLOSE) { duk_push_boolean(ctx, 0); return 1; }
        if (ev.type == OX_EV_KEY)   { duk_push_boolean(ctx, 1); break; }
        if (ev.type == OX_EV_MOUSE && ev.mouse_kind == OX_MOUSE_DOWN) {
            duk_push_boolean(ctx, 1); break;
        }
    }
    /* Repaint the user's window. */
    call_stashed(g_ctx, CB_PAINT, 0);
    ox_present(g_win);
    return 1;
}

static duk_ret_t bind_ui_prompt(duk_context *ctx) {
    if (g_win < 0) return 0;
    const char *title = duk_safe_to_string(ctx, 0);
    const char *msg   = duk_safe_to_string(ctx, 1);
    char buf[256] = "";
    int len = 0;
    int pw = 400, ph = 180;
    int px = (g_w - pw) / 2, py = (g_h - ph) / 2;
    for (;;) {
        modal_draw_panel(px, py, pw, ph, title, msg);
        /* Input field */
        ox_draw_rect(g_win, px + 12, py + 60, pw - 24, 22, OX_RGB(255, 255, 255));
        ox_draw_text(g_win, px + 18, py + 67, buf, OX_RGB(0x20, 0x20, 0x20));
        ox_present(g_win);
        ox_event_t ev;
        if (!ox_wait_event(&ev)) continue;
        if (ev.type == OX_EV_CLOSE) { duk_push_null(ctx); return 1; }
        if (ev.type == OX_EV_KEY) {
            if (ev.ascii == '\r' || ev.ascii == '\n') break;
            if (ev.ascii == 0x08 && len > 0) { buf[--len] = 0; continue; }
            if (ev.ascii == 0x1b) { duk_push_null(ctx); return 1; }
            if (ev.ascii >= 0x20 && ev.ascii < 0x7f && len + 1 < (int)sizeof(buf)) {
                buf[len++] = ev.ascii;
                buf[len] = 0;
            }
        }
        if (ev.type == OX_EV_MOUSE && ev.mouse_kind == OX_MOUSE_DOWN) {
            int bx = px + (pw - 64) / 2, by = py + ph - 32;
            if (ev.x >= bx && ev.x < bx + 64 && ev.y >= by && ev.y < by + 22) break;
        }
    }
    duk_push_string(ctx, buf);
    call_stashed(g_ctx, CB_PAINT, 0);
    ox_present(g_win);
    return 1;
}

/* ===================================================================
 * runtime setup
 * =================================================================== */

typedef struct { const char *name; duk_c_function fn; int nargs; } binding_t;

static const binding_t ox_bindings[] = {
    {"window",   bind_window,    3},
    {"title",    bind_title,     1},
    {"size",     bind_size,      0},
    {"clear",    bind_clear,     1},
    {"rect",     bind_rect,      5},
    {"text",     bind_text,      4},
    {"pixel",    bind_pixel,     3},
    {"line",     bind_line,      5},
    {"circle",   bind_circle,    4},
    {"frame",    bind_frame,     DUK_VARARGS},
    {"present",  bind_present,   0},
    {"onPaint",  bind_on_paint,  1},
    {"onKey",    bind_on_key,    1},
    {"onClick",  bind_on_click,  1},
    {"onMouse",  bind_on_mouse,  1},
    {"onTick",   bind_on_tick,   1},
    {0, 0, 0}
};
static const binding_t fs_bindings[] = {
    {"readFile",   bind_fs_readFile,   1},
    {"writeFile",  bind_fs_writeFile,  2},
    {"appendFile", bind_fs_appendFile, 2},
    {"listDir",    bind_fs_listDir,    1},
    {"exists",     bind_fs_exists,     1},
    {"stat",       bind_fs_stat,       1},
    {"mkdir",      bind_fs_mkdir,      1},
    {"unlink",     bind_fs_unlink,     1},
    {"rmdir",      bind_fs_rmdir,      1},
    {"rename",     bind_fs_rename,     2},
    {"chdir",      bind_fs_chdir,      1},
    {"cwd",        bind_fs_cwd,        0},
    {0, 0, 0}
};
static const binding_t os_bindings[] = {
    {"exec",     bind_os_exec,     1},
    {"system",   bind_os_system,   1},
    {"exit",     bind_os_exit,     1},
    {"getpid",   bind_os_getpid,   0},
    {"getenv",   bind_os_getenv,   1},
    {"setenv",   bind_os_setenv,   2},
    {"sleep",    bind_os_sleep,    1},
    {"usleep",   bind_os_usleep,   1},
    {"hostname", bind_os_hostname, 0},
    {"argv",     bind_os_argv,     0},
    {0, 0, 0}
};
static const binding_t http_bindings[] = {
    {"get",  bind_http_get,  1},
    {"post", bind_http_post, DUK_VARARGS},
    {0, 0, 0}
};
static const binding_t net_bindings[] = {
    {"tcpConnect", bind_net_tcpConnect, 2},
    {"tcpListen",  bind_net_tcpListen,  1},
    {"accept",     bind_net_accept,     1},
    {"send",       bind_net_send,       2},
    {"recv",       bind_net_recv,       DUK_VARARGS},
    {"close",      bind_net_close,      1},
    {"udpSend",    bind_net_udpSend,    3},
    {0, 0, 0}
};
static const binding_t color_bindings[] = {
    {"rgb", bind_color_rgb, 3},
    {"hex", bind_color_hex, 3},
    {0, 0, 0}
};
static const binding_t sys_bindings[] = {
    {"sysread", bind_sys_sysread, 1},
    {"meminfo", bind_sys_meminfo, 0},
    {"uptime",  bind_sys_uptime,  0},
    {"tasks",   bind_sys_tasks,   0},
    {0, 0, 0}
};
static const binding_t time_bindings[] = {
    {"now",    bind_time_now,    0},
    {"epoch",  bind_time_epoch,  0},
    {"date",   bind_time_date,   0},
    {"format", bind_time_format, 2},
    {0, 0, 0}
};
static const binding_t clipboard_bindings[] = {
    {"set", bind_clipboard_set, 1},
    {"get", bind_clipboard_get, 0},
    {0, 0, 0}
};
static const binding_t log_bindings[] = {
    {"info",  bind_log_info,  DUK_VARARGS},
    {"warn",  bind_log_warn,  DUK_VARARGS},
    {"error", bind_log_error, DUK_VARARGS},
    {0, 0, 0}
};
static const binding_t sqlite_bindings[] = {
    {"exec",  bind_sqlite_exec,  2},
    {"query", bind_sqlite_query, 2},
    {0, 0, 0}
};
static const binding_t ui_bindings[] = {
    {"msgbox", bind_ui_msgbox, 2},
    {"prompt", bind_ui_prompt, 2},
    {0, 0, 0}
};

/* Register `mod[name]` from a binding table. Stack: [...mod] in→[...mod]. */
static void register_table(duk_context *ctx, const binding_t *t) {
    for (; t->name; t++) {
        duk_push_c_function(ctx, t->fn, t->nargs);
        duk_put_prop_string(ctx, -2, t->name);
    }
}

static void setup_runtime(duk_context *ctx, int argc, char **argv) {
    duk_push_global_object(ctx);

    /* console.log/error */
    duk_push_object(ctx);
    duk_push_c_function(ctx, bind_console_log, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "log");
    duk_push_c_function(ctx, bind_console_log, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "error");
    duk_put_prop_string(ctx, -2, "console");

    /* Stash argv for ox.os.argv(). */
    duk_push_array(ctx);
    for (int i = 0; i < argc; i++) {
        duk_push_string(ctx, argv[i] ? argv[i] : "");
        duk_put_prop_index(ctx, -2, i);
    }
    duk_put_prop_string(ctx, -2, "_ox_argv");

    /* ox + sub-modules. */
    duk_push_object(ctx);            /* ox */
    register_table(ctx, ox_bindings);

#define SUBMOD(name, table) do { \
        duk_push_object(ctx); \
        register_table(ctx, table); \
        duk_put_prop_string(ctx, -2, name); \
    } while (0)

    SUBMOD("fs",        fs_bindings);
    SUBMOD("os",        os_bindings);
    SUBMOD("http",      http_bindings);
    SUBMOD("net",       net_bindings);
    SUBMOD("color",     color_bindings);
    SUBMOD("sys",       sys_bindings);
    SUBMOD("time",      time_bindings);
    SUBMOD("clipboard", clipboard_bindings);
    SUBMOD("log",       log_bindings);
    SUBMOD("sqlite",    sqlite_bindings);
    SUBMOD("ui",        ui_bindings);

    /* ox.syscall as a callable function (not a sub-object) plus constants. */
    duk_push_c_function(ctx, bind_syscall, DUK_VARARGS);
    /* Common syscall numbers as properties on the function object. */
#define DEF_SC(name, num) do { duk_push_int(ctx, num); duk_put_prop_string(ctx, -2, name); } while (0)
    DEF_SC("READ", 0);   DEF_SC("WRITE", 1);  DEF_SC("OPEN", 2);  DEF_SC("CLOSE", 3);
    DEF_SC("STAT", 4);   DEF_SC("LSEEK", 8);  DEF_SC("MMAP", 9);  DEF_SC("MUNMAP", 11);
    DEF_SC("BRK", 12);   DEF_SC("IOCTL", 16); DEF_SC("PIPE", 22); DEF_SC("SELECT", 23);
    DEF_SC("DUP", 32);   DEF_SC("NANOSLEEP", 35); DEF_SC("SOCKET", 41);
    DEF_SC("CONNECT", 42); DEF_SC("ACCEPT", 43); DEF_SC("SENDTO", 44);
    DEF_SC("RECVFROM", 45); DEF_SC("BIND", 49); DEF_SC("LISTEN", 50);
    DEF_SC("FORK", 57);  DEF_SC("EXECVE", 59); DEF_SC("EXIT", 60);
    DEF_SC("GETPID", 39); DEF_SC("KILL", 62); DEF_SC("CHDIR", 80);
    DEF_SC("MKDIR", 83); DEF_SC("UNLINK", 87);
#undef DEF_SC
    duk_put_prop_string(ctx, -2, "syscall");

    duk_put_prop_string(ctx, -2, "ox");         /* global.ox = ... */
    duk_pop(ctx);                                /* pop global */

#undef SUBMOD
}

/* ===================================================================
 * file slurp + main
 * =================================================================== */

static char *slurp_file(const char *path, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0 || st.st_size > 4 * 1024 * 1024) {
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

/* Resolve argv[1]:
 *   - "/abs/path"            → that file
 *   - "name.js"              → /home/apps/name.js
 *   - "name"                 → /home/apps/name.js
 *   - integer N              → Nth entry in /home/apps/, sorted
 */
static int resolve_script(const char *arg, char *out, size_t cap) {
    if (!arg || !arg[0]) return -1;
    if (arg[0] == '/') { snprintf(out, cap, "%s", arg); return 0; }
    /* numeric? */
    int all_digits = 1;
    for (const char *p = arg; *p; p++) if (*p < '0' || *p > '9') { all_digits = 0; break; }
    if (all_digits) {
        int target = atoi(arg);
        DIR *d = opendir("/home/apps");
        if (!d) return -1;
        char names[64][64];
        int count = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL && count < 64) {
            size_t L = strlen(e->d_name);
            if (L > 3 && strcmp(e->d_name + L - 3, ".js") == 0) {
                strncpy(names[count], e->d_name, 63);
                names[count][63] = 0;
                count++;
            }
        }
        closedir(d);
        /* Simple sort by name (insertion). */
        for (int i = 1; i < count; i++) {
            for (int j = i; j > 0 && strcmp(names[j-1], names[j]) > 0; j--) {
                char tmp[64];
                strcpy(tmp, names[j-1]); strcpy(names[j-1], names[j]); strcpy(names[j], tmp);
            }
        }
        if (target < 0 || target >= count) return -1;
        snprintf(out, cap, "/home/apps/%s", names[target]);
        return 0;
    }
    /* else: bare name. Try /home/apps/name.js, fallback /home/apps/name. */
    snprintf(out, cap, "/home/apps/%s", arg);
    struct stat st;
    if (stat(out, &st) == 0) return 0;
    snprintf(out, cap, "/home/apps/%s.js", arg);
    return 0;
}

int main(int argc, char **argv) {
    oxlog("oxjs: argc=%d argv[1]=%s\n",
          argc, (argc > 1 && argv[1]) ? argv[1] : "(null)");
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        oxlog("usage: oxjs <name|/path/to/app.js|N>\n");
        return 1;
    }
    char script_path[512];
    if (resolve_script(argv[1], script_path, sizeof(script_path)) < 0) {
        oxlog("oxjs: cannot resolve script '%s'\n", argv[1]);
        return 1;
    }
    oxlog("oxjs: loading %s\n", script_path);
    size_t src_len = 0;
    char *src = slurp_file(script_path, &src_len);
    if (!src) {
        oxlog("oxjs: cannot read %s (errno=%d)\n", script_path, errno);
        return 1;
    }
    oxlog("oxjs: script %lu bytes\n", (unsigned long)src_len);
    if (ox_init() < 0) {
        oxlog("oxjs: ox_init failed (errno=%d)\n", errno);
        free(src);
        return 1;
    }

    g_ctx = duk_create_heap_default();
    if (!g_ctx) {
        oxlog("oxjs: duk_create_heap failed\n");
        free(src);
        return 1;
    }
    setup_runtime(g_ctx, argc, argv);
    oxlog("oxjs: runtime ready, evaluating script...\n");

    if (duk_peval_lstring(g_ctx, src, src_len) != 0) {
        oxlog("oxjs: SCRIPT ERROR: %s\n",
              duk_safe_to_string(g_ctx, -1));
        duk_destroy_heap(g_ctx);
        free(src);
        return 1;
    }
    duk_pop(g_ctx);
    free(src);
    oxlog("oxjs: script eval OK, g_win=%d\n", (int)g_win);

    if (g_win < 0) {
        oxlog("oxjs: no window — exiting (script ran headless)\n");
        duk_destroy_heap(g_ctx);
        return 0;
    }
    call_stashed(g_ctx, CB_PAINT, 0);
    ox_present(g_win);

    while (1) {
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
            } else if (ev.type == OX_EV_MOUSE) {
                /* General onMouse callback gets a rich event. */
                duk_push_object(g_ctx);
                duk_push_int(g_ctx, ev.x);          duk_put_prop_string(g_ctx, -2, "x");
                duk_push_int(g_ctx, ev.y);          duk_put_prop_string(g_ctx, -2, "y");
                duk_push_int(g_ctx, ev.buttons);    duk_put_prop_string(g_ctx, -2, "buttons");
                duk_push_int(g_ctx, ev.mouse_kind); duk_put_prop_string(g_ctx, -2, "kind");
                duk_push_int(g_ctx, ev.wheel_delta);duk_put_prop_string(g_ctx, -2, "wheel");
                call_stashed(g_ctx, CB_MOUSE, 1);
                /* Legacy onClick fires only for left button DOWN. */
                if (ev.mouse_kind == OX_MOUSE_DOWN && (ev.buttons & 0x01)) {
                    duk_push_int(g_ctx, ev.x);
                    duk_push_int(g_ctx, ev.y);
                    call_stashed(g_ctx, CB_CLICK, 2);
                }
                call_stashed(g_ctx, CB_PAINT, 0);
                ox_present(g_win);
            }
            continue;
        }
        struct timespec ts = { 0, 33 * 1000000 };
        nanosleep(&ts, 0);
        duk_push_int(g_ctx, (int)time(NULL));
        call_stashed(g_ctx, CB_TICK, 1);
        call_stashed(g_ctx, CB_PAINT, 0);
        ox_present(g_win);
    }
    ox_window_destroy(g_win);
    duk_destroy_heap(g_ctx);
    return 0;
}
