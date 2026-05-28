/*
 * /bin/oxterm — windowed terminal: PTY + minishell rendered in an Ox
 * window. Reuses the patterns from /bin/term but draws into a grid
 * instead of /dev/fb0.
 *
 * Resizable since FASE 12: opts into the Ox real-resize protocol
 * (ox_window_create_resizable). On maximize/unmaximize the server
 * reallocates the SHM at the new dims and sends OX_EV_RESIZE; we
 * recompute rows/cols at the same cell size (CELL_W × CELL_H), realloc
 * the grid + scrollback, and call TIOCSWINSZ on the master PTY so the
 * kernel delivers SIGWINCH to the shell's process group — the shell
 * re-fetches winsize via TIOCGWINSZ and reflows its prompt. Result:
 * maximizing oxterm grows the usable text area, NOT the font.
 */

#include <alloca.h>
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
#include <sys/ioctl.h>
#include <unistd.h>

extern char **environ;

#define INIT_COLS 80
#define INIT_ROWS 25
#define CELL_W    8
#define CELL_H   14            /* extra 2px line-height for breathing room */
#define MARGIN    8
#define STATUS_H 20            /* bottom hint strip */
#define MIN_COLS  20           /* clamp so the window stays usable */
#define MIN_ROWS   3
#define SCROLLBACK_ROWS 256

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

/* Current grid + scrollback are now dynamically sized so the terminal
 * can grow to whatever the window dims allow. Stored as flat
 * row-major arrays; row r col c lives at g_grid[r*g_cols + c]. */
static int     g_cols = INIT_COLS;
static int     g_rows = INIT_ROWS;
static int     g_win_w = 0;
static int     g_win_h = 0;
static cell_t *g_grid  = 0;          /* g_rows * g_cols cells */
static cell_t *g_sb    = 0;          /* SCROLLBACK_ROWS * g_cols cells */
static int     g_sb_head  = 0;
static int     g_sb_count = 0;
static int     g_scroll_off = 0;
static int     g_cx = 0, g_cy = 0;
static int     g_dirty = 1;

/* Ghostty-inspired "Adwaita Dark" palette. */
#define COL_TERM_BG     OX_RGB( 30,  30,  46)
#define COL_TERM_FG     OX_RGB(224, 222, 244)
#define COL_TERM_CURSOR OX_RGB(245, 194, 231)

static uint32_t g_pen_fg = COL_TERM_FG;
static uint32_t g_pen_bg = COL_TERM_BG;
static int      g_reverse = 0;

enum { ST_NORMAL, ST_ESC, ST_CSI };
static int  g_pstate = ST_NORMAL;
static int  g_params[8];
static int  g_n_params = 0;
static int  g_cur_param = 0;
static int  g_have_cur_param = 0;

/* Catppuccin Mocha palette — modern Ghostty-style ANSI 16. */
static const uint32_t g_palette[16] = {
    OX_RGB( 69, 71,  90), OX_RGB(243,139,168), OX_RGB(166,227,161), OX_RGB(249,226,175),
    OX_RGB(137,180,250), OX_RGB(245,194,231), OX_RGB(148,226,213), OX_RGB(186,194,222),
    OX_RGB(108,112,134), OX_RGB(243,139,168), OX_RGB(166,227,161), OX_RGB(249,226,175),
    OX_RGB(137,180,250), OX_RGB(245,194,231), OX_RGB(148,226,213), OX_RGB(205,214,244),
};

/* Compute window pixel size for a given grid (cols × rows). */
static int grid_pixel_w(int cols) { return cols * CELL_W + 2 * MARGIN; }
static int grid_pixel_h(int rows) { return rows * CELL_H + 2 * MARGIN + STATUS_H; }

static cell_t *grid_at(int r, int c)   { return &g_grid[r * g_cols + c]; }
static cell_t *sb_at  (int slot, int c){ return &g_sb  [slot * g_cols + c]; }

static void grid_clear_cell(cell_t *c) {
    c->ch = ' ';
    c->fg = g_pen_fg;
    c->bg = g_pen_bg;
}

static void grid_clear_all(void) {
    for (int r = 0; r < g_rows; r++)
        for (int c = 0; c < g_cols; c++)
            grid_clear_cell(grid_at(r, c));
    g_cx = 0; g_cy = 0;
}

static void grid_scroll(void) {
    /* Push the row about to scroll off into the scrollback ring. */
    int slot = (g_sb_head + g_sb_count) % SCROLLBACK_ROWS;
    if (g_sb_count == SCROLLBACK_ROWS) {
        slot = g_sb_head;
        g_sb_head = (g_sb_head + 1) % SCROLLBACK_ROWS;
    } else {
        g_sb_count++;
    }
    memcpy(sb_at(slot, 0), grid_at(0, 0), sizeof(cell_t) * g_cols);
    for (int r = 1; r < g_rows; r++)
        memcpy(grid_at(r - 1, 0), grid_at(r, 0), sizeof(cell_t) * g_cols);
    for (int c = 0; c < g_cols; c++) grid_clear_cell(grid_at(g_rows - 1, c));
    if (g_cy > 0) g_cy--;
    g_scroll_off = 0;
}

/* Pointer to the cells of the r-th visible row, accounting for
 * scrollback offset. Returns at least 1 row of cells. */
static const cell_t *visible_row(int r) {
    int off = g_scroll_off;
    if (off <= 0) return grid_at(r, 0);
    if (r < off) {
        if (r >= g_sb_count) return grid_at(0, 0);
        int sb_idx = g_sb_count - off + r;
        int slot = (g_sb_head + sb_idx) % SCROLLBACK_ROWS;
        return sb_at(slot, 0);
    }
    return grid_at(r - off, 0);
}

static void grid_putc(char c) {
    if (c == '\r') { g_cx = 0; return; }
    if (c == '\n') {
        g_cx = 0; g_cy++;
        if (g_cy >= g_rows) grid_scroll();
        return;
    }
    if (c == '\b') {
        if (g_cx > 0) g_cx--;
        else if (g_cy > 0) { g_cy--; g_cx = g_cols - 1; }
        return;
    }
    if (c == 0x07) return;
    if (c == '\t') {
        do { grid_putc(' '); } while (g_cx & 7);
        return;
    }
    if (c < 0x20 || c >= 0x7f) return;
    if (g_cx >= g_cols) {
        g_cx = 0; g_cy++;
        if (g_cy >= g_rows) grid_scroll();
    }
    cell_t *cell = grid_at(g_cy, g_cx);
    cell->ch = c;
    cell->fg = g_reverse ? g_pen_bg : g_pen_fg;
    cell->bg = g_reverse ? g_pen_fg : g_pen_bg;
    g_cx++;
}

static void csi_dispatch(char final) {
    int p0 = g_n_params > 0 ? g_params[0] : 0;
    int p1 = g_n_params > 1 ? g_params[1] : 0;
    switch (final) {
    case 'H': case 'f': {
        int row = g_n_params > 0 ? p0 - 1 : 0;
        int col = g_n_params > 1 ? p1 - 1 : 0;
        if (row < 0) row = 0;
        if (col < 0) col = 0;
        if (row >= g_rows) row = g_rows - 1;
        if (col >= g_cols) col = g_cols - 1;
        g_cy = row; g_cx = col;
        break;
    }
    case 'A': g_cy -= (p0 ? p0 : 1); if (g_cy < 0) g_cy = 0; break;
    case 'B': g_cy += (p0 ? p0 : 1); if (g_cy >= g_rows) g_cy = g_rows - 1; break;
    case 'C': g_cx += (p0 ? p0 : 1); if (g_cx >= g_cols) g_cx = g_cols - 1; break;
    case 'D': g_cx -= (p0 ? p0 : 1); if (g_cx < 0) g_cx = 0; break;
    case 'J': {
        int mode = (g_n_params > 0) ? p0 : 0;
        if (mode == 0) {
            for (int c = g_cx; c < g_cols; c++)
                grid_clear_cell(grid_at(g_cy, c));
            for (int r = g_cy + 1; r < g_rows; r++)
                for (int c = 0; c < g_cols; c++)
                    grid_clear_cell(grid_at(r, c));
        } else if (mode == 1) {
            for (int r = 0; r < g_cy; r++)
                for (int c = 0; c < g_cols; c++)
                    grid_clear_cell(grid_at(r, c));
            for (int c = 0; c <= g_cx && c < g_cols; c++)
                grid_clear_cell(grid_at(g_cy, c));
        } else if (mode == 2) {
            for (int r = 0; r < g_rows; r++)
                for (int c = 0; c < g_cols; c++)
                    grid_clear_cell(grid_at(r, c));
            g_cx = 0; g_cy = 0;
        }
        break;
    }
    case 'K': {
        int mode = (g_n_params > 0) ? p0 : 0;
        int from_c = 0, to_c = g_cols;
        if (mode == 0) from_c = g_cx;
        else if (mode == 1) to_c = g_cx + 1;
        for (int c = from_c; c < to_c; c++)
            grid_clear_cell(grid_at(g_cy, c));
        break;
    }
    case 'm': {
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
                /* bold */
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
        if (b == 0x1b) { g_pstate = ST_ESC; return; }
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
        if (b == '?') return;
        if (g_have_cur_param || g_n_params == 0) {
            if (g_n_params < 8) g_params[g_n_params++] = g_cur_param;
        }
        csi_dispatch((char)b);
        g_pstate = ST_NORMAL;
        return;
    }
}

/* Allocate / reallocate the grid + scrollback for new (cols, rows).
 * Preserves content where possible by copying the overlap region from
 * the old grid into the top-left of the new one. Scrollback is reset
 * because its rows were sized to the OLD g_cols and don't trivially
 * re-pack (real terminals reflow lines; we punt for v1). */
static int grid_resize(int new_cols, int new_rows) {
    if (new_cols < MIN_COLS) new_cols = MIN_COLS;
    if (new_rows < MIN_ROWS) new_rows = MIN_ROWS;
    if (new_cols == g_cols && new_rows == g_rows && g_grid) return 0;

    cell_t *new_grid = (cell_t *)malloc((size_t)new_cols * new_rows *
                                         sizeof(cell_t));
    cell_t *new_sb   = (cell_t *)malloc((size_t)new_cols * SCROLLBACK_ROWS *
                                         sizeof(cell_t));
    if (!new_grid || !new_sb) {
        free(new_grid); free(new_sb);
        return -1;
    }
    /* Seed with cleared cells using current pen colours. */
    for (int r = 0; r < new_rows; r++)
        for (int c = 0; c < new_cols; c++) {
            new_grid[r * new_cols + c].ch = ' ';
            new_grid[r * new_cols + c].fg = g_pen_fg;
            new_grid[r * new_cols + c].bg = g_pen_bg;
        }
    for (size_t i = 0; i < (size_t)new_cols * SCROLLBACK_ROWS; i++) {
        new_sb[i].ch = ' ';
        new_sb[i].fg = g_pen_fg;
        new_sb[i].bg = g_pen_bg;
    }

    /* Copy overlap region top-left. If new_rows < g_rows, we keep the
     * BOTTOM old_rows worth (where the cursor lives) so the prompt
     * doesn't scroll off — same trick xterm uses on shrink. */
    if (g_grid) {
        int copy_rows = new_rows < g_rows ? new_rows : g_rows;
        int copy_cols = new_cols < g_cols ? new_cols : g_cols;
        int src_row_base = (new_rows < g_rows) ? (g_rows - new_rows) : 0;
        for (int r = 0; r < copy_rows; r++) {
            for (int c = 0; c < copy_cols; c++) {
                new_grid[r * new_cols + c] =
                    g_grid[(src_row_base + r) * g_cols + c];
            }
        }
        g_cy -= src_row_base;
        if (g_cy < 0) g_cy = 0;
    }

    free(g_grid);
    free(g_sb);
    g_grid = new_grid;
    g_sb   = new_sb;
    g_cols = new_cols;
    g_rows = new_rows;
    if (g_cx >= g_cols) g_cx = g_cols - 1;
    if (g_cy >= g_rows) g_cy = g_rows - 1;
    /* Drop scrollback view — old offsets refer to a buffer that's gone. */
    g_sb_head = 0; g_sb_count = 0; g_scroll_off = 0;
    return 0;
}

/* Tell the kernel about the new winsize so the shell gets SIGWINCH
 * via TIOCSWINSZ → sys_kill(-fg_pgid, SIGWINCH). Silently ignored if
 * the master fd isn't open yet. */
static void notify_pty_winsize(void) {
    if (g_master < 0) return;
    struct winsize ws;
    ws.ws_row    = (unsigned short)g_rows;
    ws.ws_col    = (unsigned short)g_cols;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    ioctl(g_master, TIOCSWINSZ, &ws);
}

static void render(void) {
    /* Outer frame: full window background. */
    ox_draw_rect(g_win, 0, 0, g_win_w, g_win_h, COL_TERM_BG);

    /* Background runs first. */
    for (int r = 0; r < g_rows; r++) {
        const cell_t *row = visible_row(r);
        int run_start = 0;
        for (int c = 1; c <= g_cols; c++) {
            if (c < g_cols && row[c].bg == row[run_start].bg) continue;
            int run_len = c - run_start;
            ox_draw_rect(g_win,
                          MARGIN + run_start * CELL_W,
                          MARGIN + r * CELL_H,
                          run_len * CELL_W, CELL_H,
                          row[run_start].bg);
            run_start = c;
        }
    }
    /* Glyphs. */
    char *rowbuf = (char *)alloca((size_t)g_cols + 1);
    for (int r = 0; r < g_rows; r++) {
        const cell_t *row = visible_row(r);
        int run_start = 0;
        while (run_start < g_cols) {
            uint32_t fg = row[run_start].fg;
            int run_len = 1;
            while (run_start + run_len < g_cols &&
                   row[run_start + run_len].fg == fg) {
                run_len++;
            }
            for (int i = 0; i < run_len; i++)
                rowbuf[i] = row[run_start + i].ch;
            rowbuf[run_len] = 0;
            ox_draw_text(g_win,
                          MARGIN + run_start * CELL_W,
                          MARGIN + r * CELL_H + (CELL_H - 8) / 2,
                          rowbuf, fg);
            run_start += run_len;
        }
    }
    /* Cursor. */
    if (g_scroll_off == 0) {
        ox_draw_rect(g_win,
                      MARGIN + g_cx * CELL_W,
                      MARGIN + g_cy * CELL_H,
                      CELL_W, CELL_H,
                      COL_TERM_CURSOR);
        char under = grid_at(g_cy, g_cx)->ch;
        char ch[2] = { under ? under : ' ', 0 };
        ox_draw_text(g_win,
                      MARGIN + g_cx * CELL_W,
                      MARGIN + g_cy * CELL_H + (CELL_H - 8) / 2,
                      ch, COL_TERM_BG);
    } else {
        char hint[64];
        snprintf(hint, sizeof(hint),
                 "SCROLLBACK  -%d / %d   (Esc / End to return)",
                 g_scroll_off, g_sb_count);
        ox_draw_rect(g_win, 0, g_win_h - STATUS_H, g_win_w, STATUS_H,
                      OX_RGB(45, 45, 60));
        ox_draw_text(g_win, MARGIN, g_win_h - STATUS_H + 6, hint, COL_TERM_CURSOR);
    }
    ox_present(g_win);
    g_dirty = 0;
}

static void spawn_child(void) {
    g_master = posix_openpt(O_RDWR);
    if (g_master < 0) return;
    unlockpt(g_master);
    int fl = fcntl(g_master, F_GETFL, 0);
    fcntl(g_master, F_SETFL, fl | O_NONBLOCK);

    /* Seed the PTY's winsize so the very first TIOCGWINSZ the shell
     * issues already sees our current grid. */
    notify_pty_winsize();

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
        int pgid = (int)getpid();
        ioctl(0, 0x5410 /* TIOCSPGRP */, &pgid);
        char *envp[] = {
            "PATH=/bin",
            "HOME=/home",
            "SHELL=/bin/sh",
            "TERM=xterm",
            "ENV=/home/.ashrc",
            0
        };
        char *argv_sh[] = { "sh", "-l", "-i", 0 };
        execve("/bin/sh", argv_sh, envp);
        char *argv_uxsh[] = { "uxsh", 0 };
        execve("/bin/uxsh", argv_uxsh, environ);
        execve("/bin/minishell", argv_uxsh, environ);
        _exit(127);
    }
    g_child_pid = pid;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    ox_log("oxterm: starting\n");
    if (ox_init() < 0) { ox_log("oxterm: ox_init failed\n"); return 1; }

    g_win_w = grid_pixel_w(INIT_COLS);
    g_win_h = grid_pixel_h(INIT_ROWS);
    /* Opt into the real-resize protocol: when the user zooms the
     * window, oxsrv reallocates the SHM and sends OX_EV_RESIZE
     * instead of doing the legacy 2x scaled blit. */
    g_win = ox_window_create_resizable(g_win_w, g_win_h, "Terminal");
    if (g_win < 0) { ox_log("oxterm: window_create failed\n"); return 1; }
    ox_log("oxterm: win=%d %dx%d\n", (int)g_win, g_win_w, g_win_h);

    if (grid_resize(INIT_COLS, INIT_ROWS) < 0) {
        ox_log("oxterm: grid alloc failed\n");
        return 1;
    }
    grid_clear_all();
    spawn_child();
    ox_log("oxterm: spawned child pid=%d shell=/bin/sh (ash)\n",
           g_child_pid);
    render();

    int running = 1;
    while (running) {
        /* Drain PTY master output. */
        char buf[1024];
        ssize_t n;
        while ((n = read(g_master, buf, sizeof(buf))) > 0) {
            char dbg[64];
            int show = n < 60 ? (int)n : 60;
            for (int j = 0; j < show; j++)
                dbg[j] = (buf[j] >= 0x20 && buf[j] < 0x7f) ? buf[j] : '.';
            dbg[show] = 0;
            ox_log("oxterm: pty→app %zd bytes: \"%s\"\n", n, dbg);
            for (ssize_t i = 0; i < n; i++) feed_byte((unsigned char)buf[i]);
            g_dirty = 1;
        }
        /* Drain Ox events. */
        ox_event_t ev;
        while (ox_poll_event(&ev)) {
            if (ev.type == OX_EV_CLOSE) { running = 0; break; }
            if (ev.type == OX_EV_RESIZE) {
                /* lib/libc/ox.c already remapped the SHM and updated
                 * its local w/h. We just need to recompute the grid
                 * shape for the new pixel area and tell the shell. */
                g_win_w = ev.new_w;
                g_win_h = ev.new_h;
                int new_cols = (g_win_w - 2 * MARGIN) / CELL_W;
                int new_rows = (g_win_h - 2 * MARGIN - STATUS_H) / CELL_H;
                grid_resize(new_cols, new_rows);
                notify_pty_winsize();
                ox_log("oxterm: resize → %dx%d (%dx%d cells)\n",
                       g_win_w, g_win_h, g_cols, g_rows);
                g_dirty = 1;
                continue;
            }
            if (ev.type == OX_EV_MOUSE && ev.mouse_kind == OX_MOUSE_WHEEL) {
                int prev = g_scroll_off;
                g_scroll_off += ev.wheel_delta * 3;
                if (g_scroll_off < 0)            g_scroll_off = 0;
                if (g_scroll_off > g_sb_count)   g_scroll_off = g_sb_count;
                if (g_scroll_off != prev) g_dirty = 1;
                continue;
            }
            if (ev.type == OX_EV_KEY) {
                if (ev.keycode == OX_KEY_PGUP) {
                    g_scroll_off += g_rows / 2;
                    if (g_scroll_off > g_sb_count) g_scroll_off = g_sb_count;
                    g_dirty = 1;
                    continue;
                }
                if (ev.keycode == OX_KEY_PGDN) {
                    g_scroll_off -= g_rows / 2;
                    if (g_scroll_off < 0) g_scroll_off = 0;
                    g_dirty = 1;
                    continue;
                }
                if (g_scroll_off > 0 &&
                    (ev.ascii || ev.keycode == OX_KEY_ENTER ||
                     ev.keycode == OX_KEY_BACKSPACE ||
                     ev.keycode == OX_KEY_UP ||
                     ev.keycode == OX_KEY_DOWN ||
                     ev.keycode == OX_KEY_LEFT ||
                     ev.keycode == OX_KEY_RIGHT)) {
                    g_scroll_off = 0; g_dirty = 1;
                }
                if (ev.keycode == OX_KEY_BACKSPACE ||
                    ev.ascii == 0x08 || ev.ascii == 0x7f) {
                    char c = 0x7f;
                    write(g_master, &c, 1);
                    continue;
                }
                if (ev.keycode == OX_KEY_UP)    { write(g_master, "\033[A", 3); continue; }
                if (ev.keycode == OX_KEY_DOWN)  { write(g_master, "\033[B", 3); continue; }
                if (ev.keycode == OX_KEY_RIGHT) { write(g_master, "\033[C", 3); continue; }
                if (ev.keycode == OX_KEY_LEFT)  { write(g_master, "\033[D", 3); continue; }
                if (ev.keycode == OX_KEY_HOME)  { write(g_master, "\033[H", 3); continue; }
                if (ev.keycode == OX_KEY_END)   { write(g_master, "\033[F", 3); continue; }
                if (ev.keycode == OX_KEY_DELETE){ write(g_master, "\033[3~",4); continue; }
                if (ev.ascii) {
                    char c = (char)ev.ascii;
                    if (c == '\r') c = '\n';
                    ox_log("oxterm: app→pty 0x%02x ('%c')\n",
                           (unsigned)(unsigned char)c,
                           (c >= 0x20 && c < 0x7f) ? c : '.');
                    write(g_master, &c, 1);
                } else if (ev.keycode == OX_KEY_ENTER) {
                    char c = '\n';
                    ox_log("oxterm: app→pty ENTER\n");
                    write(g_master, &c, 1);
                } else if (ev.keycode == OX_KEY_TAB) {
                    char c = '\t';
                    ox_log("oxterm: app→pty TAB\n");
                    write(g_master, &c, 1);
                } else if (ev.keycode == OX_KEY_ESC) {
                    char c = 0x1b;
                    ox_log("oxterm: app→pty ESC\n");
                    write(g_master, &c, 1);
                } else {
                    ox_log("oxterm: ignored key ascii=0 code=%d\n",
                           ev.keycode);
                }
            }
        }
        if (g_dirty) render();
        struct timespec ts = { 0, 16 * 1000000 };
        nanosleep(&ts, 0);
        int st;
        if (waitpid(g_child_pid, &st, WNOHANG) > 0) {
            running = 0;
        }
    }
    if (g_child_pid > 0) kill(g_child_pid, 15);
    if (g_master >= 0) close(g_master);
    ox_window_destroy(g_win);
    free(g_grid); free(g_sb);
    return 0;
}
