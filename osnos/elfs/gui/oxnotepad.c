/*
 * /bin/oxnotepad — minimal text editor for the Ox window system.
 *
 * Single-buffer, no undo, no scroll. Loads from $HOME/notepad.txt at
 * startup if present, Ctrl+S writes it back. Other keys insert or
 * delete; Enter inserts '\n'. Backspace removes the prev char.
 */

#include <errno.h>
#include <fcntl.h>
#include <ox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WIN_W 600
#define WIN_H 400
#define MARGIN 8
#define LINE_H 12
#define DEFAULT_PATH "/home/notepad.txt"
#define BUF_MAX 4096

static char g_buf[BUF_MAX];
static int  g_len = 0;
static ox_win_t g_win;
static char g_path[256] = DEFAULT_PATH;

static void render(void) {
    /* Paint background. */
    ox_draw_rect(g_win, 0, 0, WIN_W, WIN_H, OX_RGB(250, 250, 245));
    /* Status bar. */
    ox_draw_rect(g_win, 0, WIN_H - 16, WIN_W, 16, OX_RGB(60, 80, 130));
    char status[64];
    snprintf(status, sizeof(status), " %s  %d bytes  Ctrl+S save", g_path, g_len);
    ox_draw_text(g_win, MARGIN, WIN_H - 12, status, OX_RGB(240, 240, 255));
    /* Body — split into lines, render up to what fits. */
    int x = MARGIN, y = MARGIN;
    int max_y = WIN_H - 22;
    for (int i = 0; i < g_len && y <= max_y; i++) {
        char c = g_buf[i];
        if (c == '\n') {
            x = MARGIN;
            y += LINE_H;
            continue;
        }
        char s[2] = { c, 0 };
        ox_draw_text(g_win, x, y, s, OX_RGB(20, 20, 30));
        x += 8;
        if (x + 8 > WIN_W - MARGIN) {
            x = MARGIN;
            y += LINE_H;
        }
    }
    /* Caret hint (always at end). */
    if (y <= max_y) {
        ox_draw_rect(g_win, x, y, 1, 9, OX_RGB(20, 20, 30));
    }
    ox_present(g_win);
}

static void load_file(void) {
    int fd = open(g_path, O_RDONLY);
    if (fd < 0) return;
    int n = (int)read(fd, g_buf, BUF_MAX - 1);
    close(fd);
    if (n > 0) g_len = n;
}

static void save_file(void) {
    int fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    write(fd, g_buf, g_len);
    close(fd);
}

int main(int argc, char **argv) {
    /* Open the file given in argv[1] (when spawned from oxfiles via
     * osn_spawn(path, "/home/foo.txt", ...)) — falls back to the
     * default scratch file when run with no args. */
    if (argc > 1 && argv[1] && argv[1][0]) {
        size_t L = strlen(argv[1]);
        if (L >= sizeof(g_path)) L = sizeof(g_path) - 1;
        memcpy(g_path, argv[1], L);
        g_path[L] = 0;
    }
    if (ox_init() < 0) return 1;
    /* Window title shows the path so users know which file. */
    char title[80];
    snprintf(title, sizeof(title), "Notepad — %s", g_path);
    g_win = ox_window_create(WIN_W, WIN_H, title);
    if (g_win < 0) return 1;

    load_file();
    render();

    for (;;) {
        ox_event_t ev;
        if (!ox_wait_event(&ev)) continue;
        if (ev.type == OX_EV_CLOSE) {
            save_file();
            break;
        }
        if (ev.type != OX_EV_KEY) continue;
        int ch = ev.ascii;
        if (ch == 0x13 /* Ctrl+S */) {
            save_file();
            render();
            continue;
        }
        if (ch == '\b' || ev.keycode == OX_KEY_BACKSPACE) {
            if (g_len > 0) g_len--;
            render();
            continue;
        }
        if (ch == '\r') ch = '\n';
        if (ch == '\n' ||
            (ch >= 0x20 && ch < 0x7f)) {
            if (g_len < BUF_MAX - 1) {
                g_buf[g_len++] = (char)ch;
                render();
            }
        }
    }
    ox_window_destroy(g_win);
    return 0;
}
