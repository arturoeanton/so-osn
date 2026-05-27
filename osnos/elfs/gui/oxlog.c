/*
 * oxlog — scrollable log / text viewer.
 *
 * Slurps a file once at startup (cap 256 KiB), splits it into lines,
 * and renders inside an ox_scrollview_t so users can wheel / drag /
 * arrow through it. F5 re-reads from disk for tailing.
 *
 * Default path is /sys/tasks (always present, multi-line, refreshes
 * with kernel state). Pass any path as argv[1] to view other files.
 */

#include <errno.h>
#include <fcntl.h>
#include <ox.h>
#include <ox_ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WIN_W   720
#define WIN_H   520
#define HDR_H    32
#define LINE_H   12
#define BUF_MAX  (256 * 1024)
#define MAX_LINES 8192

static ox_win_t g_win;
static int      g_w = WIN_W, g_h = WIN_H;
static char     g_path[256] = "/sys/tasks";
static char    *g_buf = NULL;
static int      g_buf_len = 0;
static const char *g_lines[MAX_LINES];
static int      g_nlines = 0;
static ox_scrollview_t g_sv;
static ox_button_t g_btn_reload = { 0,0,0,0, "Reload", 0, 0 };
static int  g_status_err = 0;
static char g_status[160] = "";

#define COL_BG     OX_RGB(255, 255, 255)
#define COL_FG     OX_RGB( 20,  20,  20)
#define COL_HDR_BG OX_RGB(232, 232, 232)
#define COL_HDR_FG OX_RGB( 30,  30,  30)
#define COL_BORDER OX_RGB( 80,  80,  80)
#define COL_STATUS OX_RGB(110, 110, 110)
#define COL_ERROR  OX_RGB(190,  60,  60)

static void split_lines(void) {
    g_nlines = 0;
    if (!g_buf || g_buf_len == 0) return;
    g_lines[g_nlines++] = g_buf;
    for (int i = 0; i < g_buf_len && g_nlines < MAX_LINES; i++) {
        if (g_buf[i] == '\n') {
            g_buf[i] = 0;
            if (i + 1 < g_buf_len)
                g_lines[g_nlines++] = g_buf + i + 1;
        }
    }
    /* Trim trailing empty if file ends in '\n'. */
    if (g_nlines > 0 && g_lines[g_nlines - 1][0] == 0) g_nlines--;
}

static void load_file(void) {
    if (g_buf) { free(g_buf); g_buf = NULL; g_buf_len = 0; }
    g_nlines = 0;
    int fd = open(g_path, O_RDONLY);
    if (fd < 0) {
        snprintf(g_status, sizeof(g_status),
                 "cannot open %s (errno=%d)", g_path, errno);
        g_status_err = 1;
        return;
    }
    g_buf = malloc(BUF_MAX);
    if (!g_buf) { close(fd); return; }
    int total = 0;
    for (;;) {
        int n = (int)read(fd, g_buf + total, BUF_MAX - 1 - total);
        if (n <= 0) break;
        total += n;
        if (total >= BUF_MAX - 1) break;
    }
    close(fd);
    g_buf[total] = 0;
    g_buf_len = total;
    split_lines();
    g_sv.content_h = g_nlines * LINE_H + 16;
    ox_scrollview_clamp(&g_sv);
    snprintf(g_status, sizeof(g_status),
             "%s  %d line%s  %d byte%s",
             g_path, g_nlines, g_nlines == 1 ? "" : "s",
             g_buf_len, g_buf_len == 1 ? "" : "s");
    g_status_err = 0;
}

static void layout(void) {
    g_sv.x = 0;
    g_sv.y = HDR_H;
    g_sv.w = g_w;
    g_sv.h = g_h - HDR_H - 16;
    g_sv.wheel_step = 30;
    g_sv.bar_w = 12;
    g_btn_reload.x = g_w - 90;
    g_btn_reload.y = 4;
    g_btn_reload.w = 84;
    g_btn_reload.h = 24;
    ox_scrollview_clamp(&g_sv);
}

static void render(void) {
    /* Header bar. */
    ox_draw_rect(g_win, 0, 0, g_w, HDR_H, COL_HDR_BG);
    ox_draw_rect(g_win, 0, HDR_H - 1, g_w, 1, COL_BORDER);
    {
        char ttl[300];
        snprintf(ttl, sizeof(ttl), "Log: %s", g_path);
        ox_draw_text(g_win, 10, 12, ttl, COL_HDR_FG);
    }
    ox_button_draw(g_win, &g_btn_reload);

    /* Body. */
    ox_scrollview_draw_bg(g_win, &g_sv);
    int first = g_sv.scroll_y / LINE_H;
    int last  = first + (g_sv.h / LINE_H) + 1;
    if (last > g_nlines) last = g_nlines;
    int top_offset = g_sv.scroll_y % LINE_H;
    for (int i = first; i < last; i++) {
        int y = g_sv.y + (i - first) * LINE_H - top_offset;
        if (y + LINE_H < g_sv.y || y > g_sv.y + g_sv.h) continue;
        ox_draw_text(g_win, 6, y + 2, g_lines[i], COL_FG);
    }
    ox_scrollview_draw_bar(g_win, &g_sv);

    /* Status. */
    ox_draw_rect(g_win, 0, g_h - 16, g_w, 16, COL_HDR_BG);
    ox_draw_text(g_win, 8, g_h - 12, g_status,
                 g_status_err ? COL_ERROR : COL_STATUS);

    ox_present(g_win);
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1] && argv[1][0]) {
        size_t L = strlen(argv[1]);
        if (L >= sizeof(g_path)) L = sizeof(g_path) - 1;
        memcpy(g_path, argv[1], L);
        g_path[L] = 0;
    }
    ox_log("oxlog: starting\n");
    if (ox_init() < 0) return 1;
    char title[300];
    snprintf(title, sizeof(title), "Log — %s", g_path);
    g_win = ox_window_create(g_w, g_h, title);
    if (g_win < 0) return 1;
    layout();
    load_file();
    render();

    int quit = 0;
    while (!quit) {
        ox_event_t ev;
        if (!ox_wait_event(&ev)) continue;
        if (ev.type == OX_EV_CLOSE) break;
        if (ev.type == OX_EV_RESIZE) {
            g_w = ev.new_w;
            g_h = ev.new_h;
            layout();
            render();
            continue;
        }
        if (ev.type == OX_EV_MOUSE) {
            int prev_hover = g_btn_reload.hover;
            g_btn_reload.hover = ox_button_hit(&g_btn_reload, ev.x, ev.y);
            if (ev.mouse_kind == OX_MOUSE_DOWN && (ev.buttons & 0x01)) {
                if (g_btn_reload.hover) {
                    load_file();
                    render();
                    continue;
                }
            }
            if (ox_scrollview_event(&g_sv, &ev)) { render(); continue; }
            if (prev_hover != g_btn_reload.hover) render();
            continue;
        }
        if (ev.type == OX_EV_KEY) {
            /* F5 = reload. */
            if (ev.keycode == 63 /* F5 */) {
                load_file();
                render();
                continue;
            }
            if (ev.ascii == 'q' || ev.ascii == 'Q') break;
            if (ox_scrollview_event(&g_sv, &ev)) { render(); continue; }
            continue;
        }
    }

    ox_window_destroy(g_win);
    if (g_buf) free(g_buf);
    return 0;
}
