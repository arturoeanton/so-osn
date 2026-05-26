/*
 * /bin/oxtop — graphical process viewer + killer for Ox.
 *
 * Adwaita-dark window listing osnos tasks via SYS_TASKINFO. Shows pid,
 * name, state, and a monotonic dispatches counter. Click a row to
 * select; "Kill" sends SIGKILL to the selected pid. Auto-refresh every
 * second.
 */

#include <errno.h>
#include <osnos_ipc.h>
#include <ox.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../src/include/osnos_taskinfo.h"

/* ---------------- syscall raw wrapper ----------------------------- */
extern long osnos_syscall2(long n, long a1, long a2);

/* ---------------- geometry ---------------------------------------- */
#define WIN_W        640
#define WIN_H        500
#define HEADER_H      38
#define FOOTER_H      56
#define ROW_H         22
#define MAX_TASKS_UI  32      /* kernel MAX_TASKS=16, room to spare */

/* ---------------- palette (Adwaita dark) -------------------------- */
#define COL_HEADER     OX_RGB( 30, 30, 30)
#define COL_BODY       OX_RGB( 36, 36, 36)
#define COL_FOOTER     OX_RGB( 30, 30, 30)
#define COL_DIVIDER    OX_RGB( 18, 18, 18)
#define COL_TEXT       OX_RGB(255,255,255)
#define COL_TEXT_DIM   OX_RGB(154,153,150)
#define COL_TEXT_KERN  OX_RGB(166,148,196)   /* kernel tasks lavender */
#define COL_ACCENT     OX_RGB( 53,132,228)
#define COL_HOVER      OX_RGB( 48, 48, 48)
#define COL_BTN_OK     OX_RGB( 53,132,228)
#define COL_BTN_OK_H   OX_RGB( 99,164,235)
#define COL_BTN_DANGER OX_RGB(192, 28, 40)
#define COL_BTN_DANGER_H OX_RGB(229, 60, 73)
#define COL_BTN_DIS    OX_RGB( 60, 60, 60)
#define COL_BTN_FG     OX_RGB(255,255,255)
#define COL_BTN_FG_DIS OX_RGB(120,120,120)
#define COL_STATE_RUN  OX_RGB(163,190,140)   /* green */
#define COL_STATE_RDY  OX_RGB(235,203,139)   /* yellow */
#define COL_STATE_BLK  OX_RGB(154,153,150)   /* dim */
#define COL_STATE_DEAD OX_RGB(191, 97,106)   /* red */

/* ---------------- state ------------------------------------------- */
static ox_win_t g_win;
static osnos_taskinfo_t g_tasks[MAX_TASKS_UI];
static int      g_n_tasks = 0;
static int      g_selected = -1;
static int      g_hover = -1;
static int      g_kill_hover = 0;
static int      g_refresh_hover = 0;

/* ---------------- helpers ----------------------------------------- */
static const char *state_label(uint8_t s) {
    switch (s) {
    case OSNOS_TASK_READY:   return "READY";
    case OSNOS_TASK_RUNNING: return "RUN";
    case OSNOS_TASK_BLOCKED: return "BLOCK";
    case OSNOS_TASK_STOPPED: return "STOP";
    case OSNOS_TASK_DEAD:    return "DEAD";
    case OSNOS_TASK_ZOMBIE:  return "ZOMB";
    default:                 return "?";
    }
}

static uint32_t state_color(uint8_t s) {
    switch (s) {
    case OSNOS_TASK_RUNNING: return COL_STATE_RUN;
    case OSNOS_TASK_READY:   return COL_STATE_RDY;
    case OSNOS_TASK_BLOCKED: return COL_STATE_BLK;
    case OSNOS_TASK_STOPPED: return COL_STATE_BLK;
    case OSNOS_TASK_DEAD:    return COL_STATE_DEAD;
    case OSNOS_TASK_ZOMBIE:  return COL_STATE_DEAD;
    default:                 return COL_TEXT_DIM;
    }
}

#define SYS_TASKINFO 515

/* Enumerate all task slots, store the ones that aren't UNUSED. */
static void refresh_tasks(void) {
    g_n_tasks = 0;
    /* Kernel MAX_TASKS is 16 but the syscall handles out-of-range
     * with ENOENT — iterate generously and stop on first miss past
     * a populated slot? No, just iterate to MAX_TASKS_UI and skip. */
    for (int i = 0; i < MAX_TASKS_UI && g_n_tasks < MAX_TASKS_UI; i++) {
        osnos_taskinfo_t info;
        long r = osnos_syscall2(SYS_TASKINFO, i, (long)&info);
        if (r < 0) continue;
        if (info.state == OSNOS_TASK_UNUSED) continue;
        g_tasks[g_n_tasks++] = info;
    }
    /* Clamp selection to current task list. */
    if (g_selected >= g_n_tasks) g_selected = -1;
}

/* ---------------- layout ------------------------------------------ */
static int list_top(void)    { return HEADER_H + 6; }
static int list_bottom(void) { return WIN_H - FOOTER_H; }
static int rows_visible(void) { return (list_bottom() - list_top()) / ROW_H; }

static int kill_btn_x(void) { return WIN_W - 110 - 16; }
static int refresh_btn_x(void) { return kill_btn_x() - 110 - 8; }
static int btn_y(void) { return WIN_H - FOOTER_H + (FOOTER_H - 32) / 2; }

/* Column positions (px from window left). Each column is 8px-aligned
 * since the font is 8x8 monospace. */
#define COL_PID_X    12
#define COL_NAME_X   72
#define COL_STATE_X  260
#define COL_DISP_X   340     /* right-aligned */

/* ---------------- render ------------------------------------------ */
static void draw_btn(int x, int w, const char *label, uint32_t bg,
                     uint32_t bg_hover, int hovered, int enabled) {
    uint32_t col = enabled ? (hovered ? bg_hover : bg) : COL_BTN_DIS;
    uint32_t fg  = enabled ? COL_BTN_FG : COL_BTN_FG_DIS;
    ox_draw_rect(g_win, x, btn_y(), w, 32, col);
    int lx = x + (w - (int)strlen(label) * 8) / 2;
    int ly = btn_y() + (32 - 8) / 2;
    ox_draw_text(g_win, lx, ly, label, fg);
}

static void render_chrome(void) {
    ox_draw_rect(g_win, 0, 0, WIN_W, HEADER_H, COL_HEADER);
    ox_draw_rect(g_win, 0, WIN_H - FOOTER_H, WIN_W, FOOTER_H, COL_FOOTER);
    ox_draw_rect(g_win, 0, HEADER_H - 1, WIN_W, 1, COL_DIVIDER);
    ox_draw_rect(g_win, 0, WIN_H - FOOTER_H, WIN_W, 1, COL_DIVIDER);
    /* Title left + task count right. */
    ox_draw_text(g_win, 18, (HEADER_H - 8) / 2, "Processes", COL_TEXT);
    char count[40];
    snprintf(count, sizeof(count), "%d tasks", g_n_tasks);
    int cx = WIN_W - 18 - (int)strlen(count) * 8;
    ox_draw_text(g_win, cx, (HEADER_H - 8) / 2, count, COL_TEXT_DIM);
    /* Column headings on row 0 of the list zone. */
    int hy = HEADER_H + 8;
    ox_draw_rect(g_win, 0, HEADER_H, WIN_W, ROW_H, COL_BODY);
    ox_draw_text(g_win, COL_PID_X,   hy, "PID",        COL_TEXT_DIM);
    ox_draw_text(g_win, COL_NAME_X,  hy, "NAME",       COL_TEXT_DIM);
    ox_draw_text(g_win, COL_STATE_X, hy, "STATE",      COL_TEXT_DIM);
    ox_draw_text(g_win, COL_DISP_X,  hy, "DISPATCHES", COL_TEXT_DIM);
    /* Buttons */
    draw_btn(refresh_btn_x(), 100, "Refresh",
             COL_BTN_OK, COL_BTN_OK_H, g_refresh_hover, 1);
    draw_btn(kill_btn_x(), 100, "Kill",
             COL_BTN_DANGER, COL_BTN_DANGER_H, g_kill_hover, g_selected >= 0);
}

static void render_row(int i) {
    int y = list_top() + ROW_H + i * ROW_H;   /* +ROW_H for header row */
    int is_sel = (i == g_selected);
    int is_hov = (i == g_hover);
    uint32_t bg = COL_BODY;
    if (is_sel)      bg = COL_ACCENT;
    else if (is_hov) bg = COL_HOVER;
    ox_draw_rect(g_win, 4, y, WIN_W - 8, ROW_H - 2, bg);

    char buf[40];
    int ty = y + (ROW_H - 8) / 2;

    /* PID */
    snprintf(buf, sizeof(buf), "%lu",
             (unsigned long)g_tasks[i].pid);
    ox_draw_text(g_win, COL_PID_X, ty, buf, COL_TEXT);

    /* Name — dim if kernel task (is_user=0). */
    ox_draw_text(g_win, COL_NAME_X, ty, g_tasks[i].name,
                 g_tasks[i].is_user ? COL_TEXT : COL_TEXT_KERN);

    /* State — colored. */
    ox_draw_text(g_win, COL_STATE_X, ty,
                 state_label(g_tasks[i].state),
                 is_sel ? COL_TEXT : state_color(g_tasks[i].state));

    /* Dispatches */
    snprintf(buf, sizeof(buf), "%lu",
             (unsigned long)g_tasks[i].dispatches);
    ox_draw_text(g_win, COL_DISP_X, ty, buf,
                 is_sel ? COL_TEXT : COL_TEXT_DIM);
}

static void render(void) {
    /* List zone body. */
    ox_draw_rect(g_win, 0, HEADER_H + ROW_H, WIN_W,
                 list_bottom() - HEADER_H - ROW_H, COL_BODY);
    render_chrome();
    int max_rows = rows_visible() - 1;   /* minus header row */
    for (int i = 0; i < g_n_tasks && i < max_rows; i++) {
        render_row(i);
    }
    if (g_n_tasks == 0) {
        const char *msg = "no tasks found (SYS_TASKINFO failed?)";
        int len = (int)strlen(msg) * 8;
        ox_draw_text(g_win, (WIN_W - len) / 2, HEADER_H + 40,
                     msg, COL_TEXT_DIM);
    }
    ox_present(g_win);
}

/* ---------------- hit tests --------------------------------------- */
static int hit_row(int mx, int my) {
    if (my < list_top() + ROW_H) return -1;   /* header row */
    if (my >= list_bottom()) return -1;
    int row = (my - list_top() - ROW_H) / ROW_H;
    if (mx < 4 || mx >= WIN_W - 4) return -1;
    if (row >= g_n_tasks) return -1;
    return row;
}
static int hit_kill_btn(int mx, int my) {
    return (mx >= kill_btn_x() && mx < kill_btn_x() + 100 &&
            my >= btn_y() && my < btn_y() + 32);
}
static int hit_refresh_btn(int mx, int my) {
    return (mx >= refresh_btn_x() && mx < refresh_btn_x() + 100 &&
            my >= btn_y() && my < btn_y() + 32);
}

/* ---------------- main -------------------------------------------- */
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (ox_init() < 0) return 1;
    g_win = ox_window_create(WIN_W, WIN_H, "Processes");
    if (g_win < 0) return 1;
    refresh_tasks();
    render();
    /* Auto-refresh every ~1s via timeout-based polling. */
    int frame = 0;
    for (;;) {
        ox_event_t ev;
        int got = ox_poll_event(&ev);
        if (got) {
            if (ev.type == OX_EV_CLOSE) break;
            if (ev.type == OX_EV_MOUSE) {
                int row = hit_row(ev.x, ev.y);
                int kh  = hit_kill_btn(ev.x, ev.y);
                int rh  = hit_refresh_btn(ev.x, ev.y);
                if (ev.mouse_kind == OX_MOUSE_MOVE) {
                    int dirty = 0;
                    if (row != g_hover) { g_hover = row; dirty = 1; }
                    if (kh != g_kill_hover) { g_kill_hover = kh; dirty = 1; }
                    if (rh != g_refresh_hover) { g_refresh_hover = rh; dirty = 1; }
                    if (dirty) render();
                } else if (ev.mouse_kind == OX_MOUSE_DOWN) {
                    if (rh) {
                        refresh_tasks();
                        render();
                    } else if (kh && g_selected >= 0) {
                        kill((int)g_tasks[g_selected].pid, 9 /* SIGKILL */);
                        /* Give the scheduler a moment to reap, then refresh. */
                        struct timespec ts = { 0, 100 * 1000000 };
                        nanosleep(&ts, 0);
                        refresh_tasks();
                        render();
                    } else if (row >= 0) {
                        g_selected = row;
                        render();
                    }
                }
            }
        } else {
            /* Idle: short sleep + auto-refresh every ~30 idle ticks. */
            struct timespec ts = { 0, 33 * 1000000 };
            nanosleep(&ts, 0);
            frame++;
            if (frame >= 30) {           /* ~1 second */
                frame = 0;
                refresh_tasks();
                render();
            }
        }
    }
    ox_window_destroy(g_win);
    return 0;
}
