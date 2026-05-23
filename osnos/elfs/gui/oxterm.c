/*
 * /bin/oxterm — windowed terminal: PTY + minishell rendered in an Ox
 * window. Reuses the patterns from /bin/term but draws into a grid
 * instead of /dev/fb0.
 */

#include <errno.h>
#include <fcntl.h>
#include <ox.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define COLS 80
#define ROWS 25
#define CELL_W 8
#define CELL_H 12
#define WIN_W (COLS * CELL_W + 8)
#define WIN_H (ROWS * CELL_H + 8)
#define MARGIN 4

static ox_win_t g_win;
static int      g_master = -1;
static int      g_child_pid = -1;

/* Per-cell color so SGR escapes work. fg is packed RGB; bg too.
 * Stored alongside the character for one-pass render. */
typedef struct {
    char     ch;
    uint32_t fg;
    uint32_t bg;
} cell_t;

static cell_t g_grid[ROWS][COLS];
static int      g_cx = 0, g_cy = 0;
static int      g_dirty = 1;

/* Current SGR pen state — applied to every printed cell. */
static uint32_t g_pen_fg = OX_RGB(200, 230, 200);
static uint32_t g_pen_bg = OX_RGB( 15,  15,  25);
static int      g_reverse = 0;

/* ANSI parser state. */
enum { ST_NORMAL, ST_ESC, ST_CSI };
static int  g_pstate = ST_NORMAL;
static int  g_params[8];
static int  g_n_params = 0;
static int  g_cur_param = 0;
static int  g_have_cur_param = 0;

/* xterm-256 inspired basic palette (8 base + 8 bright). */
static const uint32_t g_palette[16] = {
    OX_RGB(  0,  0,  0), OX_RGB(178,24,44), OX_RGB( 64,160,43), OX_RGB(178,148,21),
    OX_RGB( 41, 79,184), OX_RGB(157,52,158), OX_RGB( 36,165,165), OX_RGB(200,200,200),
    OX_RGB(120,120,120), OX_RGB(255,90,90), OX_RGB(128,255,128), OX_RGB(255,255,80),
    OX_RGB(100,140,255), OX_RGB(255,120,255), OX_RGB(120,255,255), OX_RGB(255,255,255),
};

static void grid_clear_cell(cell_t *c) {
    c->ch = ' ';
    c->fg = g_pen_fg;
    c->bg = g_pen_bg;
}

static void grid_clear_all(void) {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            grid_clear_cell(&g_grid[r][c]);
    g_cx = 0; g_cy = 0;
}

static void grid_scroll(void) {
    for (int r = 1; r < ROWS; r++)
        memcpy(g_grid[r - 1], g_grid[r], sizeof(g_grid[0]));
    for (int c = 0; c < COLS; c++) grid_clear_cell(&g_grid[ROWS - 1][c]);
    if (g_cy > 0) g_cy--;
}

static void grid_putc(char c) {
    if (c == '\r') { g_cx = 0; return; }
    if (c == '\n') {
        g_cx = 0; g_cy++;
        if (g_cy >= ROWS) grid_scroll();
        return;
    }
    if (c == '\b') {
        if (g_cx > 0) g_cx--;
        else if (g_cy > 0) { g_cy--; g_cx = COLS - 1; }
        grid_clear_cell(&g_grid[g_cy][g_cx]);
        return;
    }
    if (c == 0x07) return;                /* bell */
    if (c == '\t') {
        do { grid_putc(' '); } while (g_cx & 7);
        return;
    }
    if (c < 0x20 || c >= 0x7f) return;
    if (g_cx >= COLS) {
        g_cx = 0; g_cy++;
        if (g_cy >= ROWS) grid_scroll();
    }
    g_grid[g_cy][g_cx].ch = c;
    g_grid[g_cy][g_cx].fg = g_reverse ? g_pen_bg : g_pen_fg;
    g_grid[g_cy][g_cx].bg = g_reverse ? g_pen_fg : g_pen_bg;
    g_cx++;
}

/* CSI dispatch: invoked when we hit the final byte of an
 * ESC [ ... <final> sequence. Implements the SGR + cursor +
 * erase subset that 99% of TTY programs rely on. */
static void csi_dispatch(char final) {
    int p0 = g_n_params > 0 ? g_params[0] : 0;
    int p1 = g_n_params > 1 ? g_params[1] : 0;
    switch (final) {
    case 'H': case 'f': {
        int row = g_n_params > 0 ? p0 - 1 : 0;
        int col = g_n_params > 1 ? p1 - 1 : 0;
        if (row < 0) row = 0;
        if (col < 0) col = 0;
        if (row >= ROWS) row = ROWS - 1;
        if (col >= COLS) col = COLS - 1;
        g_cy = row; g_cx = col;
        break;
    }
    case 'A': g_cy -= (p0 ? p0 : 1); if (g_cy < 0) g_cy = 0; break;
    case 'B': g_cy += (p0 ? p0 : 1); if (g_cy >= ROWS) g_cy = ROWS - 1; break;
    case 'C': g_cx += (p0 ? p0 : 1); if (g_cx >= COLS) g_cx = COLS - 1; break;
    case 'D': g_cx -= (p0 ? p0 : 1); if (g_cx < 0) g_cx = 0; break;
    case 'J': {
        /* Erase in display. 0/none = below, 1 = above, 2 = all. */
        int mode = (g_n_params > 0) ? p0 : 0;
        int from_r = 0, to_r = ROWS;
        if (mode == 0) { from_r = g_cy; }
        else if (mode == 1) { to_r = g_cy + 1; }
        for (int r = from_r; r < to_r; r++)
            for (int c = 0; c < COLS; c++)
                grid_clear_cell(&g_grid[r][c]);
        if (mode == 2) { g_cx = 0; g_cy = 0; }
        break;
    }
    case 'K': {
        /* Erase in line. */
        int mode = (g_n_params > 0) ? p0 : 0;
        int from_c = 0, to_c = COLS;
        if (mode == 0) from_c = g_cx;
        else if (mode == 1) to_c = g_cx + 1;
        for (int c = from_c; c < to_c; c++)
            grid_clear_cell(&g_grid[g_cy][c]);
        break;
    }
    case 'm': {
        /* SGR — colours / attributes. */
        if (g_n_params == 0) {
            g_pen_fg = OX_RGB(200, 230, 200);
            g_pen_bg = OX_RGB(15, 15, 25);
            g_reverse = 0;
            break;
        }
        for (int i = 0; i < g_n_params; i++) {
            int p = g_params[i];
            if (p == 0) {
                g_pen_fg = OX_RGB(200, 230, 200);
                g_pen_bg = OX_RGB(15, 15, 25);
                g_reverse = 0;
            } else if (p == 1) {
                /* bold: approximate by promoting to bright base. */
            } else if (p == 7) {
                g_reverse = 1;
            } else if (p == 22 || p == 27) {
                g_reverse = 0;
            } else if (p >= 30 && p <= 37) {
                g_pen_fg = g_palette[p - 30];
            } else if (p == 39) {
                g_pen_fg = OX_RGB(200, 230, 200);
            } else if (p >= 40 && p <= 47) {
                g_pen_bg = g_palette[p - 40];
            } else if (p == 49) {
                g_pen_bg = OX_RGB(15, 15, 25);
            } else if (p >= 90 && p <= 97) {
                g_pen_fg = g_palette[8 + (p - 90)];
            } else if (p >= 100 && p <= 107) {
                g_pen_bg = g_palette[8 + (p - 100)];
            } else if (p == 38 && i + 4 < g_n_params && g_params[i+1] == 2) {
                /* 38;2;R;G;B truecolor */
                g_pen_fg = OX_RGB(g_params[i+2] & 0xff,
                                   g_params[i+3] & 0xff,
                                   g_params[i+4] & 0xff);
                i += 4;
            } else if (p == 48 && i + 4 < g_n_params && g_params[i+1] == 2) {
                g_pen_bg = OX_RGB(g_params[i+2] & 0xff,
                                   g_params[i+3] & 0xff,
                                   g_params[i+4] & 0xff);
                i += 4;
            }
        }
        break;
    }
    default: break;
    }
}

static void feed_byte(unsigned char b) {
    switch (g_pstate) {
    case ST_NORMAL:
        if (b == 0x1b) {
            g_pstate = ST_ESC;
            return;
        }
        grid_putc((char)b);
        return;
    case ST_ESC:
        if (b == '[') {
            g_pstate = ST_CSI;
            g_n_params = 0;
            g_cur_param = 0;
            g_have_cur_param = 0;
            return;
        }
        /* Unsupported ESC <X> — drop and resume. */
        g_pstate = ST_NORMAL;
        return;
    case ST_CSI:
        if (b >= '0' && b <= '9') {
            g_cur_param = g_cur_param * 10 + (b - '0');
            g_have_cur_param = 1;
            return;
        }
        if (b == ';') {
            if (g_n_params < 8) g_params[g_n_params++] = g_have_cur_param ? g_cur_param : 0;
            g_cur_param = 0;
            g_have_cur_param = 0;
            return;
        }
        if (b == '?') return;  /* private-mode marker — ignore */
        /* Final byte. */
        if (g_have_cur_param || g_n_params == 0) {
            if (g_n_params < 8) g_params[g_n_params++] = g_cur_param;
        }
        csi_dispatch((char)b);
        g_pstate = ST_NORMAL;
        return;
    }
}

static void render(void) {
    /* Group consecutive cells with identical bg into rect runs; emit
     * one draw_rect per run + one draw_text per row. Much cheaper
     * than per-cell rendering. */
    for (int r = 0; r < ROWS; r++) {
        int run_start = 0;
        for (int c = 1; c <= COLS; c++) {
            if (c < COLS && g_grid[r][c].bg == g_grid[r][run_start].bg) continue;
            int run_len = c - run_start;
            ox_draw_rect(g_win,
                          MARGIN + run_start * CELL_W,
                          MARGIN + r * CELL_H,
                          run_len * CELL_W, CELL_H,
                          g_grid[r][run_start].bg);
            run_start = c;
        }
    }
    /* Now glyphs — group consecutive cells with same fg into runs. */
    char rowbuf[COLS + 1];
    for (int r = 0; r < ROWS; r++) {
        int run_start = 0;
        while (run_start < COLS) {
            uint32_t fg = g_grid[r][run_start].fg;
            int run_len = 1;
            while (run_start + run_len < COLS &&
                   g_grid[r][run_start + run_len].fg == fg) {
                run_len++;
            }
            for (int i = 0; i < run_len; i++)
                rowbuf[i] = g_grid[r][run_start + i].ch;
            rowbuf[run_len] = 0;
            ox_draw_text(g_win,
                          MARGIN + run_start * CELL_W,
                          MARGIN + r * CELL_H + 2,
                          rowbuf, fg);
            run_start += run_len;
        }
    }
    /* Cursor block on top. */
    ox_draw_rect(g_win,
                  MARGIN + g_cx * CELL_W,
                  MARGIN + g_cy * CELL_H + CELL_H - 2,
                  CELL_W, 2,
                  OX_RGB(255, 255, 100));
    ox_present(g_win);
    g_dirty = 0;
}

static void spawn_child(void) {
    g_master = posix_openpt(O_RDWR);
    if (g_master < 0) return;
    unlockpt(g_master);
    /* Non-blocking master so we can drain in the event loop. */
    int fl = fcntl(g_master, F_GETFL, 0);
    fcntl(g_master, F_SETFL, fl | O_NONBLOCK);
    char slave[32];
    if (ptsname_r(g_master, slave, sizeof(slave)) < 0) return;
    int pid = fork();
    if (pid == 0) {
        int s = open(slave, O_RDWR);
        if (s < 0) _exit(127);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        if (s > 2) close(s);
        close(g_master);
        setsid();
        char *argv[] = { "uxsh", 0 };
        execve("/bin/uxsh", argv, environ);
        /* Fallback if uxsh missing — minishell at least echoes. */
        execve("/bin/minishell", argv, environ);
        _exit(127);
    }
    g_child_pid = pid;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (ox_init() < 0) return 1;
    g_win = ox_window_create(WIN_W, WIN_H, "Terminal");
    if (g_win < 0) return 1;
    grid_clear_all();
    spawn_child();
    render();

    int running = 1;
    while (running) {
        /* Drain PTY master output. */
        char buf[1024];
        ssize_t n;
        while ((n = read(g_master, buf, sizeof(buf))) > 0) {
            for (ssize_t i = 0; i < n; i++) feed_byte((unsigned char)buf[i]);
            g_dirty = 1;
        }
        /* Drain Ox events. */
        ox_event_t ev;
        while (ox_poll_event(&ev)) {
            if (ev.type == OX_EV_CLOSE) { running = 0; break; }
            if (ev.type == OX_EV_KEY) {
                /* Map ascii to bytes for the shell. */
                if (ev.ascii) {
                    char c = (char)ev.ascii;
                    if (c == '\r') c = '\n';
                    write(g_master, &c, 1);
                } else if (ev.keycode == OX_KEY_ENTER) {
                    char c = '\n';
                    write(g_master, &c, 1);
                } else if (ev.keycode == OX_KEY_BACKSPACE) {
                    char c = 0x7f;
                    write(g_master, &c, 1);
                }
            }
        }
        if (g_dirty) render();
        struct timespec ts = { 0, 16 * 1000000 };
        nanosleep(&ts, 0);
        /* Reap child if it exited. */
        int st;
        if (waitpid(g_child_pid, &st, WNOHANG) > 0) {
            /* Respawn — the user might "exit" minishell and want a
             * fresh one in the same window, but for V1 we just close. */
            running = 0;
        }
    }
    if (g_child_pid > 0) kill(g_child_pid, 15);
    if (g_master >= 0) close(g_master);
    ox_window_destroy(g_win);
    return 0;
}
