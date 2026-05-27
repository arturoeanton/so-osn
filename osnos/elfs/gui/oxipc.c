/*
 * oxipc — live IPC + services inspector.
 *
 *   - top:    /sys/services   (server registry)
 *   - bottom: /sys/tasks      (used as proxy for "who's listening")
 *
 * Mostly the same as oxmem; different sources. Refreshes every 1 s
 * so you can watch services come and go.
 */

#include <errno.h>
#include <fcntl.h>
#include <ox.h>
#include <ox_ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WIN_W   720
#define WIN_H   540
#define HDR_H    28
#define LINE_H   12
#define BUF_MAX  8192

static ox_win_t g_win;
static int g_w = WIN_W, g_h = WIN_H;

typedef struct {
    const char     *path;
    const char     *title;
    char            buf[BUF_MAX];
    int             nbytes;
    const char     *lines[256];
    int             nlines;
    ox_scrollview_t sv;
} panel_t;

static panel_t g_panels[2] = {
    { .path = "/sys/services", .title = "Services (SERVER_*)" },
    { .path = "/sys/tasks",    .title = "Tasks"               },
};

#define COL_FG       OX_RGB( 20,  20,  20)
#define COL_HDR_BG   OX_RGB(232, 232, 232)
#define COL_BORDER   OX_RGB( 80,  80,  80)
#define COL_PANEL_HDR    OX_RGB( 80, 130, 180)
#define COL_PANEL_HDR_FG OX_RGB(255, 255, 255)

static void slurp(panel_t *p) {
    int fd = open(p->path, O_RDONLY);
    p->nbytes = 0;
    p->nlines = 0;
    if (fd < 0) {
        snprintf(p->buf, sizeof(p->buf),
                 "cannot open %s (errno=%d)\n", p->path, errno);
        p->nbytes = (int)strlen(p->buf);
    } else {
        int total = 0;
        for (;;) {
            int n = (int)read(fd, p->buf + total, BUF_MAX - 1 - total);
            if (n <= 0) break;
            total += n;
            if (total >= BUF_MAX - 1) break;
        }
        close(fd);
        p->buf[total] = 0;
        p->nbytes = total;
    }
    if (p->nbytes == 0) return;
    p->lines[p->nlines++] = p->buf;
    for (int i = 0; i < p->nbytes && p->nlines < (int)(sizeof(p->lines)/sizeof(p->lines[0])); i++) {
        if (p->buf[i] == '\n') {
            p->buf[i] = 0;
            if (i + 1 < p->nbytes) p->lines[p->nlines++] = p->buf + i + 1;
        }
    }
    if (p->nlines > 0 && p->lines[p->nlines - 1][0] == 0) p->nlines--;
    p->sv.content_h = p->nlines * LINE_H + 8;
    ox_scrollview_clamp(&p->sv);
}

static void layout(void) {
    int panel_h = (g_h - HDR_H) / 2;
    for (int i = 0; i < 2; i++) {
        g_panels[i].sv.x = 0;
        g_panels[i].sv.y = HDR_H + i * panel_h + 14;
        g_panels[i].sv.w = g_w;
        g_panels[i].sv.h = panel_h - 14;
        g_panels[i].sv.wheel_step = 30;
        g_panels[i].sv.bar_w = 12;
        ox_scrollview_clamp(&g_panels[i].sv);
    }
}

static void draw_panel(panel_t *p) {
    ox_draw_rect(g_win, 0, p->sv.y - 14, g_w, 14, COL_PANEL_HDR);
    ox_draw_text(g_win, 8, p->sv.y - 11, p->title, COL_PANEL_HDR_FG);
    ox_scrollview_draw_bg(g_win, &p->sv);
    int first = p->sv.scroll_y / LINE_H;
    int last  = first + (p->sv.h / LINE_H) + 1;
    if (last > p->nlines) last = p->nlines;
    int top_off = p->sv.scroll_y % LINE_H;
    for (int i = first; i < last; i++) {
        int y = p->sv.y + (i - first) * LINE_H - top_off;
        if (y + LINE_H < p->sv.y || y > p->sv.y + p->sv.h) continue;
        ox_draw_text(g_win, 6, y + 2, p->lines[i], COL_FG);
    }
    ox_scrollview_draw_bar(g_win, &p->sv);
}

static void render(void) {
    ox_draw_rect(g_win, 0, 0, g_w, HDR_H, COL_HDR_BG);
    ox_draw_rect(g_win, 0, HDR_H - 1, g_w, 1, COL_BORDER);
    ox_draw_text(g_win, 10, 10, "IPC & Services  —  refresh every 1s",
                 OX_RGB(30, 30, 30));
    for (int i = 0; i < 2; i++) draw_panel(&g_panels[i]);
    ox_present(g_win);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (ox_init() < 0) return 1;
    g_win = ox_window_create(g_w, g_h, "IPC & Services");
    if (g_win < 0) return 1;
    layout();
    slurp(&g_panels[0]);
    slurp(&g_panels[1]);
    render();
    int frame = 0;
    for (;;) {
        ox_event_t ev;
        int got = ox_poll_event(&ev);
        if (got) {
            if (ev.type == OX_EV_CLOSE) break;
            if (ev.type == OX_EV_RESIZE) { g_w = ev.new_w; g_h = ev.new_h; layout(); render(); continue; }
            int dirty = 0;
            for (int i = 0; i < 2; i++) {
                if (ox_scrollview_event(&g_panels[i].sv, &ev)) dirty = 1;
            }
            if (dirty) render();
            continue;
        }
        struct timespec ts = { 0, 33 * 1000000 };
        nanosleep(&ts, 0);
        if (++frame >= 30) {
            frame = 0;
            slurp(&g_panels[0]);
            slurp(&g_panels[1]);
            render();
        }
    }
    ox_window_destroy(g_win);
    return 0;
}
