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

static char     g_grid[ROWS][COLS];
static int      g_cx = 0, g_cy = 0;
static int      g_dirty = 1;

static void grid_clear(void) {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            g_grid[r][c] = ' ';
    g_cx = 0; g_cy = 0;
}

static void grid_scroll(void) {
    for (int r = 1; r < ROWS; r++)
        memcpy(g_grid[r - 1], g_grid[r], COLS);
    for (int c = 0; c < COLS; c++) g_grid[ROWS - 1][c] = ' ';
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
        g_grid[g_cy][g_cx] = ' ';
        return;
    }
    if (c == 0x07) return;                /* bell */
    if (c == 0x1b) return;                /* drop ESC; ANSI not interpreted */
    if (c < 0x20 || c >= 0x7f) return;
    if (g_cx >= COLS) {
        g_cx = 0; g_cy++;
        if (g_cy >= ROWS) grid_scroll();
    }
    g_grid[g_cy][g_cx++] = c;
}

static void render(void) {
    /* Background. */
    ox_draw_rect(g_win, 0, 0, WIN_W, WIN_H, OX_RGB(15, 15, 25));
    /* Cursor block (drawn first so text overlays). */
    ox_draw_rect(g_win,
                  MARGIN + g_cx * CELL_W,
                  MARGIN + g_cy * CELL_H,
                  CELL_W, CELL_H,
                  OX_RGB(80, 120, 80));
    /* Render each row as a single text run to minimise IPC. */
    char rowbuf[COLS + 1];
    for (int r = 0; r < ROWS; r++) {
        memcpy(rowbuf, g_grid[r], COLS);
        rowbuf[COLS] = 0;
        ox_draw_text(g_win,
                      MARGIN,
                      MARGIN + r * CELL_H + 2,
                      rowbuf,
                      OX_RGB(200, 230, 200));
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
        char *argv[] = { "minishell", 0 };
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
    grid_clear();
    spawn_child();
    render();

    int running = 1;
    while (running) {
        /* Drain PTY master output. */
        char buf[256];
        ssize_t n;
        while ((n = read(g_master, buf, sizeof(buf))) > 0) {
            for (ssize_t i = 0; i < n; i++) grid_putc(buf[i]);
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
