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
#define CELL_W  8
#define CELL_H  14            /* extra 2px line-height for breathing room */
#define MARGIN  8
#define WIN_W (COLS * CELL_W + 2 * MARGIN)
#define WIN_H (ROWS * CELL_H + 2 * MARGIN + 20)  /* +20 = bottom status hint */

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

/* Scrollback ring buffer. Each row that falls off the top of the
 * live grid via grid_scroll() is pushed here. g_sb_count rises until
 * SCROLLBACK_ROWS, then we keep overwriting g_sb_head (oldest entry).
 * g_scroll_off counts how many rows up from the live top the user
 * has scrolled (0 = live, SCROLLBACK_ROWS = oldest). */
#define SCROLLBACK_ROWS 256
static cell_t g_sb[SCROLLBACK_ROWS][COLS];
static int    g_sb_head  = 0;          /* index of oldest entry */
static int    g_sb_count = 0;          /* 0..SCROLLBACK_ROWS    */
static int    g_scroll_off = 0;        /* rows above live top */

/* Ghostty-inspired "Adwaita Dark" palette — comfortable cream-on-
 * charcoal foreground/background and the standard ANSI 16 below. */
#define COL_TERM_BG     OX_RGB( 30,  30,  46)
#define COL_TERM_FG     OX_RGB(224, 222, 244)
#define COL_TERM_CURSOR OX_RGB(245, 194, 231)

/* Current SGR pen state — applied to every printed cell. */
static uint32_t g_pen_fg = COL_TERM_FG;
static uint32_t g_pen_bg = COL_TERM_BG;
static int      g_reverse = 0;

/* ANSI parser state. */
enum { ST_NORMAL, ST_ESC, ST_CSI };
static int  g_pstate = ST_NORMAL;
static int  g_params[8];
static int  g_n_params = 0;
static int  g_cur_param = 0;
static int  g_have_cur_param = 0;

/* Catppuccin Mocha palette — modern Ghostty-style ANSI 16. Reads
 * comfortably on the dark Adwaita-style background and matches the
 * pink cursor we set above. */
static const uint32_t g_palette[16] = {
    OX_RGB( 69, 71,  90), OX_RGB(243,139,168), OX_RGB(166,227,161), OX_RGB(249,226,175),
    OX_RGB(137,180,250), OX_RGB(245,194,231), OX_RGB(148,226,213), OX_RGB(186,194,222),
    OX_RGB(108,112,134), OX_RGB(243,139,168), OX_RGB(166,227,161), OX_RGB(249,226,175),
    OX_RGB(137,180,250), OX_RGB(245,194,231), OX_RGB(148,226,213), OX_RGB(205,214,244),
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
    /* Push the row about to scroll off into the scrollback ring. */
    int slot = (g_sb_head + g_sb_count) % SCROLLBACK_ROWS;
    if (g_sb_count == SCROLLBACK_ROWS) {
        /* Ring full — overwrite oldest, advance head. */
        slot = g_sb_head;
        g_sb_head = (g_sb_head + 1) % SCROLLBACK_ROWS;
    } else {
        g_sb_count++;
    }
    memcpy(g_sb[slot], g_grid[0], sizeof(g_grid[0]));
    for (int r = 1; r < ROWS; r++)
        memcpy(g_grid[r - 1], g_grid[r], sizeof(g_grid[0]));
    for (int c = 0; c < COLS; c++) grid_clear_cell(&g_grid[ROWS - 1][c]);
    if (g_cy > 0) g_cy--;
    /* Live activity always returns the user to the bottom. */
    g_scroll_off = 0;
}

/* Look up the (row r in [0..ROWS-1])-th visible row, accounting for
 * scrollback offset. Returns the cell array for that row. */
static const cell_t *visible_row(int r) {
    int off = g_scroll_off;
    if (off <= 0) return g_grid[r];
    /* When off > 0, the top `off` rows of the screen show the
     * latest `off` rows of the scrollback ring; below that comes
     * the live grid. */
    if (r < off) {
        if (r >= g_sb_count) return g_grid[0]; /* shouldn't happen */
        int sb_idx = g_sb_count - off + r;
        int slot = (g_sb_head + sb_idx) % SCROLLBACK_ROWS;
        return g_sb[slot];
    }
    return g_grid[r - off];
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
    /* Outer frame: full window background. */
    ox_draw_rect(g_win, 0, 0, WIN_W, WIN_H, COL_TERM_BG);

    /* Background runs first — group consecutive cells with same bg
     * and emit one rect per run. */
    for (int r = 0; r < ROWS; r++) {
        const cell_t *row = visible_row(r);
        int run_start = 0;
        for (int c = 1; c <= COLS; c++) {
            if (c < COLS && row[c].bg == row[run_start].bg) continue;
            int run_len = c - run_start;
            ox_draw_rect(g_win,
                          MARGIN + run_start * CELL_W,
                          MARGIN + r * CELL_H,
                          run_len * CELL_W, CELL_H,
                          row[run_start].bg);
            run_start = c;
        }
    }
    /* Glyphs — group consecutive cells with same fg into runs. */
    char rowbuf[COLS + 1];
    for (int r = 0; r < ROWS; r++) {
        const cell_t *row = visible_row(r);
        int run_start = 0;
        while (run_start < COLS) {
            uint32_t fg = row[run_start].fg;
            int run_len = 1;
            while (run_start + run_len < COLS &&
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
    /* Cursor: full Ghostty-style block. Only when at the live view —
     * scrollback view shows just a thin outline so the user knows
     * they're looking at history. */
    if (g_scroll_off == 0) {
        ox_draw_rect(g_win,
                      MARGIN + g_cx * CELL_W,
                      MARGIN + g_cy * CELL_H,
                      CELL_W, CELL_H,
                      COL_TERM_CURSOR);
        /* Redraw the glyph under the cursor in the bg color so it
         * reads as inverse — true Ghostty look. */
        char ch[2] = { g_grid[g_cy][g_cx].ch ? g_grid[g_cy][g_cx].ch : ' ', 0 };
        ox_draw_text(g_win,
                      MARGIN + g_cx * CELL_W,
                      MARGIN + g_cy * CELL_H + (CELL_H - 8) / 2,
                      ch, COL_TERM_BG);
    } else {
        /* Indicate scrollback view via a status strip at the bottom. */
        char hint[64];
        snprintf(hint, sizeof(hint),
                 "SCROLLBACK  -%d / %d   (Esc / End to return)",
                 g_scroll_off, g_sb_count);
        ox_draw_rect(g_win, 0, WIN_H - 20, WIN_W, 20,
                      OX_RGB(45, 45, 60));
        ox_draw_text(g_win, MARGIN, WIN_H - 14, hint, COL_TERM_CURSOR);
    }
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

    /* Put the PTY in raw mode so the kernel line discipline doesn't
     * echo arrow keys / control sequences back to us — without this,
     * sending "\033[A" to the slave bounces straight back via ECHO
     * and our CSI parser interprets it as "cursor up" and visibly
     * moves the grid cursor instead of letting the shell handle it
     * as history navigation. The shell can re-cook the termios as
     * needed; we just want the BASELINE to be raw + noecho. */
    struct termios t;
    if (tcgetattr(g_master, &t) == 0) {
        t.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHONL | ISIG);
        t.c_iflag &= ~(INLCR | ICRNL | IXON);
        t.c_oflag &= ~OPOST;
        tcsetattr(g_master, TCSANOW, &t);
    }

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
        /* Prefer busybox sh (full line editor with history support);
         * fall back to uxsh / minishell if anything is missing. The
         * shell name in argv[0] tells busybox to dispatch to ash. */
        char *envp[] = {
            "PATH=/bin",
            "HOME=/home",
            "SHELL=/bin/sh",
            /* TERM=xterm tells busybox line editor to enable arrow-
             * key history nav (the default unknown TERM disables it). */
            "TERM=xterm",
            "PS1=osnos:/\\w$ ",
            0
        };
        char *argv_sh[] = { "sh", "-i", 0 };
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
    g_win = ox_window_create(WIN_W, WIN_H, "Terminal");
    if (g_win < 0) { ox_log("oxterm: window_create failed\n"); return 1; }
    ox_log("oxterm: win=%d %dx%d\n", (int)g_win, WIN_W, WIN_H);
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
            for (ssize_t i = 0; i < n; i++) feed_byte((unsigned char)buf[i]);
            g_dirty = 1;
        }
        /* Drain Ox events. */
        ox_event_t ev;
        while (ox_poll_event(&ev)) {
            if (ev.type == OX_EV_CLOSE) { running = 0; break; }
            if (ev.type == OX_EV_MOUSE && ev.mouse_kind == OX_MOUSE_WHEEL) {
                /* Wheel scrolls the scrollback when up, or jumps
                 * back to live when scrolling down at the bottom. */
                int prev = g_scroll_off;
                g_scroll_off += ev.wheel_delta * 3;
                if (g_scroll_off < 0)            g_scroll_off = 0;
                if (g_scroll_off > g_sb_count)   g_scroll_off = g_sb_count;
                if (g_scroll_off != prev) g_dirty = 1;
                continue;
            }
            if (ev.type == OX_EV_KEY) {
                /* PageUp/PageDown navigate the scrollback. */
                if (ev.keycode == OX_KEY_PGUP) {
                    g_scroll_off += ROWS / 2;
                    if (g_scroll_off > g_sb_count) g_scroll_off = g_sb_count;
                    g_dirty = 1;
                    continue;
                }
                if (ev.keycode == OX_KEY_PGDN) {
                    g_scroll_off -= ROWS / 2;
                    if (g_scroll_off < 0) g_scroll_off = 0;
                    g_dirty = 1;
                    continue;
                }
                /* If user types while scrolled up, snap back to live. */
                if (g_scroll_off > 0 &&
                    (ev.ascii || ev.keycode == OX_KEY_ENTER ||
                     ev.keycode == OX_KEY_BACKSPACE ||
                     ev.keycode == OX_KEY_UP ||
                     ev.keycode == OX_KEY_DOWN ||
                     ev.keycode == OX_KEY_LEFT ||
                     ev.keycode == OX_KEY_RIGHT)) {
                    g_scroll_off = 0; g_dirty = 1;
                }
                /* Backspace — the line discipline expects 0x7F (DEL) as
                 * its ERASE character. Some keyboards emit ascii=0x08
                 * for the Backspace key; remap so the shell actually
                 * erases instead of leaving "BS, space, BS" echoed
                 * with the original chars still in the read buffer. */
                if (ev.keycode == OX_KEY_BACKSPACE ||
                    ev.ascii == 0x08 || ev.ascii == 0x7f) {
                    char c = 0x7f;
                    write(g_master, &c, 1);
                    continue;
                }
                /* Arrow keys, Home/End, Delete — emit the standard
                 * xterm-style ESC sequences so ash's line editor can
                 * navigate history (Up/Down) and move within the
                 * current line (Left/Right/Home/End/Delete). */
                if (ev.keycode == OX_KEY_UP)    { write(g_master, "\033[A", 3); continue; }
                if (ev.keycode == OX_KEY_DOWN)  { write(g_master, "\033[B", 3); continue; }
                if (ev.keycode == OX_KEY_RIGHT) { write(g_master, "\033[C", 3); continue; }
                if (ev.keycode == OX_KEY_LEFT)  { write(g_master, "\033[D", 3); continue; }
                if (ev.keycode == OX_KEY_HOME)  { write(g_master, "\033[H", 3); continue; }
                if (ev.keycode == OX_KEY_END)   { write(g_master, "\033[F", 3); continue; }
                if (ev.keycode == OX_KEY_DELETE){ write(g_master, "\033[3~",4); continue; }
                /* Map ascii to bytes for the shell. Includes Tab (0x09),
                 * Enter (0x0a after the \r → \n translation below), and
                 * every Ctrl+letter (control chars 0x01..0x1f). */
                if (ev.ascii) {
                    char c = (char)ev.ascii;
                    if (c == '\r') c = '\n';
                    write(g_master, &c, 1);
                } else if (ev.keycode == OX_KEY_ENTER) {
                    char c = '\n';
                    write(g_master, &c, 1);
                } else if (ev.keycode == OX_KEY_TAB) {
                    char c = '\t';
                    write(g_master, &c, 1);
                } else if (ev.keycode == OX_KEY_ESC) {
                    char c = 0x1b;
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
