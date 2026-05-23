/*
 * /bin/oxcalc — basic 4-function calculator.
 *
 * 4x5 button grid, single-line display. Integer arithmetic stored
 * in long long; +, -, *, /  chain via accumulator + pending-op
 * pattern. Click buttons; keyboard digits + ops also accepted.
 */

#include <ox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN_W 240
#define WIN_H 320
#define DISP_H 48
#define BTN_COLS 4
#define BTN_ROWS 5

static ox_win_t g_win;
static char     g_display[32] = "0";
static long long g_acc = 0;
static char     g_pending = 0;       /* 0 none, else '+', '-', '*', '/' */
static int      g_just_evaluated = 0;

typedef struct {
    const char *label;
    uint32_t    color;
    uint32_t    fg;
} btn_t;

#define COL_NUM  OX_RGB( 70,  70,  70)
#define COL_OP   OX_RGB(180,  90,  30)
#define COL_ACT  OX_RGB( 30, 100, 160)
#define COL_EQ   OX_RGB( 30, 140,  60)
#define FG       OX_RGB(255, 255, 255)

static const btn_t g_btns[BTN_ROWS][BTN_COLS] = {
  { {"C", COL_ACT, FG}, {"CE", COL_ACT, FG}, {"/", COL_OP, FG}, {"*", COL_OP, FG} },
  { {"7", COL_NUM, FG}, {"8",  COL_NUM, FG}, {"9", COL_NUM, FG}, {"-", COL_OP, FG} },
  { {"4", COL_NUM, FG}, {"5",  COL_NUM, FG}, {"6", COL_NUM, FG}, {"+", COL_OP, FG} },
  { {"1", COL_NUM, FG}, {"2",  COL_NUM, FG}, {"3", COL_NUM, FG}, {"=", COL_EQ, FG} },
  { {"0", COL_NUM, FG}, {"00", COL_NUM, FG}, {".", COL_NUM, FG}, {"+/-", COL_NUM, FG} },
};

static int btn_w(void) { return WIN_W / BTN_COLS; }
static int btn_h(void) { return (WIN_H - DISP_H) / BTN_ROWS; }

static void apply_op(void) {
    long long n = atoll(g_display);
    if (g_pending == 0) {
        g_acc = n;
    } else if (g_pending == '+') g_acc += n;
    else if (g_pending == '-') g_acc -= n;
    else if (g_pending == '*') g_acc *= n;
    else if (g_pending == '/') {
        if (n == 0) { strcpy(g_display, "ERR"); g_acc = 0; g_pending = 0; return; }
        g_acc /= n;
    }
    snprintf(g_display, sizeof(g_display), "%lld", g_acc);
}

static void on_btn(const char *label) {
    if (label[0] >= '0' && label[0] <= '9') {
        if (g_just_evaluated) { strcpy(g_display, "0"); g_just_evaluated = 0; }
        if (strcmp(g_display, "0") == 0 && label[0] != '.') {
            strcpy(g_display, label);
        } else if (strlen(g_display) < sizeof(g_display) - 3) {
            strcat(g_display, label);
        }
    } else if (strcmp(label, ".") == 0) {
        /* Integer-only; ignore for now. */
    } else if (strcmp(label, "+/-") == 0) {
        if (g_display[0] == '-') {
            memmove(g_display, g_display + 1, strlen(g_display));
        } else if (strcmp(g_display, "0") != 0) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "-%s", g_display);
            strcpy(g_display, tmp);
        }
    } else if (strcmp(label, "C") == 0) {
        g_acc = 0; g_pending = 0; strcpy(g_display, "0"); g_just_evaluated = 0;
    } else if (strcmp(label, "CE") == 0) {
        strcpy(g_display, "0"); g_just_evaluated = 0;
    } else if (strcmp(label, "=") == 0) {
        apply_op();
        g_pending = 0;
        g_just_evaluated = 1;
    } else if (label[1] == 0 && (label[0] == '+' || label[0] == '-' ||
                                   label[0] == '*' || label[0] == '/')) {
        apply_op();
        g_pending = label[0];
        g_just_evaluated = 1;
    }
}

static void render(void) {
    ox_draw_rect(g_win, 0, 0, WIN_W, WIN_H, OX_RGB(40, 40, 50));
    /* Display. */
    ox_draw_rect(g_win, 4, 4, WIN_W - 8, DISP_H - 8, OX_RGB(20, 20, 25));
    int dl = (int)strlen(g_display);
    int tx = WIN_W - 12 - dl * 8;
    if (tx < 8) tx = 8;
    ox_draw_text(g_win, tx, 18, g_display, OX_RGB(200, 255, 200));
    /* Buttons. */
    int bw = btn_w(), bh = btn_h();
    for (int r = 0; r < BTN_ROWS; r++) {
        for (int c = 0; c < BTN_COLS; c++) {
            int x = c * bw;
            int y = DISP_H + r * bh;
            ox_draw_rect(g_win, x + 2, y + 2, bw - 4, bh - 4, g_btns[r][c].color);
            int ll = (int)strlen(g_btns[r][c].label);
            int lx = x + (bw - ll * 8) / 2;
            int ly = y + (bh - 8) / 2;
            ox_draw_text(g_win, lx, ly, g_btns[r][c].label, g_btns[r][c].fg);
        }
    }
    ox_present(g_win);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (ox_init() < 0) return 1;
    g_win = ox_window_create(WIN_W, WIN_H, "Calc");
    if (g_win < 0) return 1;
    render();
    for (;;) {
        ox_event_t ev;
        if (!ox_wait_event(&ev)) continue;
        if (ev.type == OX_EV_CLOSE) break;
        if (ev.type == OX_EV_MOUSE && ev.mouse_kind == OX_MOUSE_DOWN) {
            int bw = btn_w(), bh = btn_h();
            int c = ev.x / bw;
            int r = (ev.y - DISP_H) / bh;
            if (ev.y >= DISP_H && r >= 0 && r < BTN_ROWS &&
                c >= 0 && c < BTN_COLS) {
                on_btn(g_btns[r][c].label);
                render();
            }
        } else if (ev.type == OX_EV_KEY) {
            char k = (char)ev.ascii;
            if (k >= '0' && k <= '9') {
                char s[2] = { k, 0 };
                on_btn(s);
            } else if (k == '+' || k == '-' || k == '*' || k == '/') {
                char s[2] = { k, 0 };
                on_btn(s);
            } else if (k == '=' || k == '\r' || k == '\n') {
                on_btn("=");
            } else if (k == 'c' || k == 'C') {
                on_btn("C");
            } else continue;
            render();
        }
    }
    ox_window_destroy(g_win);
    return 0;
}
