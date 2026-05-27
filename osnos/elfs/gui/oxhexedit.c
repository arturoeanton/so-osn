/*
 * /bin/oxhexedit — minimal hex editor for the Ox window system.
 *
 * Layout (classic hexdump):
 *   00000000  48 65 6c 6c 6f 20 77 6f  72 6c 64 0a               Hello world.
 *   00000010  4c 6f 72 65 6d 20 69 70  73 75 6d 20 64 6f 6c 6f   Lorem ipsum dolo
 *
 * Keys: arrows (1 byte / 16 bytes), PgUp/PgDn, Home/End, Ctrl+S save,
 * Ctrl+Q quit. Typing a hex digit edits the byte under the cursor —
 * two presses for the high then low nibble. Tab toggles between hex
 * column and ASCII column editing.
 *
 * Read from argv[1] (or /home/notepad.txt default). Buffer cap 64 KiB.
 */

#include <fcntl.h>
#include <ox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WIN_W   720
#define WIN_H   460
#define MARGIN  10
#define LINE_H  12
#define CHAR_W   8
#define STATUS_H 16
#define BODY_Y   8
#define BODY_H  (WIN_H - STATUS_H - BODY_Y)
#define VIS_LINES (BODY_H / LINE_H)
#define BUF_MAX (64 * 1024)
#define DEFAULT_PATH "/home/notepad.txt"

#define COL_BG          OX_RGB(245, 245, 240)
#define COL_TEXT        OX_RGB( 20,  20,  25)
#define COL_ADDR_FG     OX_RGB(120, 120, 130)
#define COL_SEL_BG      OX_RGB(180, 210, 255)
#define COL_SEL_FG      OX_RGB( 10,  10,  20)
#define COL_STATUS_BG   OX_RGB( 60,  80, 130)
#define COL_STATUS_FG   OX_RGB(240, 240, 255)
#define COL_STATUS_ERR  OX_RGB(255, 200, 100)
#define COL_GRID_LINE   OX_RGB(220, 220, 220)
#define COL_NIBBLE_HOT  OX_RGB(255, 230, 100)   /* shown when half a hex pair was entered */

static unsigned char g_buf[BUF_MAX];
static int  g_len = 0;
static int  g_cur = 0;
static int  g_scroll = 0;     /* topmost row visible (each row = 16 bytes) */
static int  g_dirty = 0;
static int  g_pending_nibble = -1;   /* -1 if no half-byte pending */
static int  g_in_ascii = 0;          /* 0 = editing hex, 1 = editing ASCII */
static char g_path[256] = DEFAULT_PATH;
static char g_status[160] = "ready";
static int  g_status_err = 0;
static ox_win_t g_win;

/* ---------------- file I/O ---------------------------------------- */

static void load_file(void) {
    int fd = open(g_path, O_RDONLY);
    if (fd < 0) {
        snprintf(g_status, sizeof(g_status),
                 "(new file) %s", g_path);
        return;
    }
    int n = (int)read(fd, g_buf, BUF_MAX);
    close(fd);
    if (n > 0) g_len = n;
    snprintf(g_status, sizeof(g_status),
             "loaded %d bytes from %s", g_len, g_path);
}

static int save_file(void) {
    int fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(g_status, sizeof(g_status), "save: open failed");
        g_status_err = 1;
        return -1;
    }
    int wrote = (int)write(fd, g_buf, (size_t)g_len);
    close(fd);
    if (wrote != g_len) {
        snprintf(g_status, sizeof(g_status), "save: short write");
        g_status_err = 1;
        return -1;
    }
    g_dirty = 0;
    g_status_err = 0;
    snprintf(g_status, sizeof(g_status),
             "saved %d bytes to %s", g_len, g_path);
    return 0;
}

/* ---------------- render ------------------------------------------ */

#define HEX_X        (MARGIN + 10 * CHAR_W)             /* after addr "00000000  " */
#define HEX_W_PER    (3 * CHAR_W)                       /* "xx " */
#define HEX_GAP_X    (HEX_X + 8 * HEX_W_PER + CHAR_W)   /* mid-row gap */
#define HEX_END_X    (HEX_GAP_X + 8 * HEX_W_PER)
#define ASCII_X      (HEX_END_X + CHAR_W)

static int hex_col_x(int col_in_row) {
    int gap = (col_in_row >= 8) ? CHAR_W : 0;
    return HEX_X + col_in_row * HEX_W_PER + gap;
}

static void ensure_cursor_visible(void) {
    int row = g_cur / 16;
    if (row < g_scroll) g_scroll = row;
    if (row >= g_scroll + VIS_LINES) g_scroll = row - VIS_LINES + 1;
    if (g_scroll < 0) g_scroll = 0;
}

static void render(void) {
    ox_draw_rect(g_win, 0, 0, WIN_W, WIN_H - STATUS_H, COL_BG);
    /* Optional thin vertical separator between hex + ASCII. */
    ox_draw_rect(g_win, HEX_END_X - CHAR_W / 2, BODY_Y - 2,
                 1, BODY_H, COL_GRID_LINE);

    for (int r = 0; r < VIS_LINES; r++) {
        int row_idx = g_scroll + r;
        int off = row_idx * 16;
        if (off > g_len) break;
        int y = BODY_Y + r * LINE_H;

        /* Address column. */
        char addr[12];
        snprintf(addr, sizeof(addr), "%08x", off);
        ox_draw_text(g_win, MARGIN, y, addr, COL_ADDR_FG);

        /* Hex pairs + ASCII column for each of the 16 bytes. */
        for (int i = 0; i < 16; i++) {
            int byte_off = off + i;
            int is_cur = (byte_off == g_cur);
            int has_byte = (byte_off < g_len);
            int hx = hex_col_x(i);

            if (has_byte) {
                unsigned char b = g_buf[byte_off];
                char pair[3] = {
                    "0123456789abcdef"[b >> 4],
                    "0123456789abcdef"[b & 0xF],
                    0
                };
                uint32_t bg = COL_BG;
                if (is_cur) {
                    bg = (g_pending_nibble >= 0 && !g_in_ascii)
                        ? COL_NIBBLE_HOT
                        : COL_SEL_BG;
                }
                if (is_cur)
                    ox_draw_rect(g_win, hx - 1, y - 1,
                                 2 * CHAR_W + 2, LINE_H, bg);
                ox_draw_text(g_win, hx, y, pair,
                             is_cur ? COL_SEL_FG : COL_TEXT);
            } else if (is_cur) {
                /* Cursor past EOF (one position after end) for appending. */
                ox_draw_rect(g_win, hx - 1, y - 1,
                             2 * CHAR_W + 2, LINE_H, COL_SEL_BG);
                ox_draw_text(g_win, hx, y, "..", COL_SEL_FG);
            }

            /* ASCII column. */
            int ax = ASCII_X + i * CHAR_W;
            if (has_byte) {
                unsigned char b = g_buf[byte_off];
                char ch[2] = {
                    (b >= 32 && b < 127) ? (char)b : '.', 0
                };
                if (is_cur)
                    ox_draw_rect(g_win, ax, y - 1, CHAR_W, LINE_H,
                                 g_in_ascii ? COL_NIBBLE_HOT : COL_SEL_BG);
                ox_draw_text(g_win, ax, y, ch,
                             is_cur ? COL_SEL_FG : COL_TEXT);
            } else if (is_cur) {
                ox_draw_rect(g_win, ax, y - 1, CHAR_W, LINE_H, COL_SEL_BG);
            }
        }
    }

    /* Status bar. */
    ox_draw_rect(g_win, 0, WIN_H - STATUS_H, WIN_W, STATUS_H, COL_STATUS_BG);
    char status[200];
    snprintf(status, sizeof(status),
             " %s%s  off=0x%x  %d bytes  %s mode  ^S save  ^Q quit  Tab switch",
             g_dirty ? "* " : "  ",
             g_path, g_cur, g_len,
             g_in_ascii ? "ASCII" : "HEX");
    ox_draw_text(g_win, MARGIN, WIN_H - 12, status,
                 g_status_err ? COL_STATUS_ERR : COL_STATUS_FG);
    ox_present(g_win);
}

/* ---------------- editing helpers --------------------------------- */

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

/* Apply a single hex nibble at the cursor. First press = high nibble,
 * second press = low nibble then advance. */
static void edit_hex_nibble(int d) {
    if (g_cur >= BUF_MAX) return;
    if (g_cur > g_len) g_cur = g_len;
    if (g_cur == g_len) {
        /* Append a new byte. */
        if (g_len >= BUF_MAX) return;
        g_buf[g_len++] = 0;
    }
    if (g_pending_nibble < 0) {
        g_pending_nibble = d;
        g_buf[g_cur] = (unsigned char)((d << 4) | (g_buf[g_cur] & 0x0F));
    } else {
        g_buf[g_cur] = (unsigned char)((g_pending_nibble << 4) | d);
        g_pending_nibble = -1;
        if (g_cur < BUF_MAX - 1) g_cur++;
    }
    g_dirty = 1;
}

/* In ASCII mode, typing a printable char replaces the byte under cursor
 * and advances. Append a new byte if past EOF. */
static void edit_ascii_byte(int c) {
    if (c < 0x20 || c > 0x7e) return;
    if (g_cur >= BUF_MAX) return;
    if (g_cur == g_len) {
        if (g_len >= BUF_MAX) return;
        g_buf[g_len++] = (unsigned char)c;
    } else {
        g_buf[g_cur] = (unsigned char)c;
    }
    if (g_cur < BUF_MAX - 1) g_cur++;
    g_dirty = 1;
}

static void cursor_left(void)  { if (g_cur > 0) g_cur--; g_pending_nibble = -1; }
static void cursor_right(void) { if (g_cur < g_len) g_cur++; g_pending_nibble = -1; }
static void cursor_up(void)    { if (g_cur >= 16) g_cur -= 16; g_pending_nibble = -1; }
static void cursor_down(void)  { if (g_cur + 16 <= g_len) g_cur += 16; g_pending_nibble = -1; }
static void cursor_pgup(void)  {
    g_cur -= 16 * (VIS_LINES - 1);
    if (g_cur < 0) g_cur = 0;
    g_pending_nibble = -1;
}
static void cursor_pgdn(void)  {
    g_cur += 16 * (VIS_LINES - 1);
    if (g_cur > g_len) g_cur = g_len;
    g_pending_nibble = -1;
}
static void cursor_home(void) { g_cur = 0; g_pending_nibble = -1; }
static void cursor_end(void)  { g_cur = g_len; g_pending_nibble = -1; }

int main(int argc, char **argv) {
    if (argc > 1 && argv[1] && argv[1][0]) {
        size_t L = strlen(argv[1]);
        if (L >= sizeof(g_path)) L = sizeof(g_path) - 1;
        memcpy(g_path, argv[1], L);
        g_path[L] = 0;
    }
    if (ox_init() < 0) return 1;
    char title[80];
    snprintf(title, sizeof(title), "HexEdit \xb7 %s", g_path);
    g_win = ox_window_create(WIN_W, WIN_H, title);
    if (g_win < 0) return 1;

    load_file();
    render();

    int quit = 0;
    while (!quit) {
        ox_event_t ev;
        if (!ox_wait_event(&ev)) continue;
        if (ev.type == OX_EV_CLOSE) {
            if (g_dirty) save_file();
            break;
        }
        if (ev.type != OX_EV_KEY) continue;

        int a = ev.ascii;
        int kc = ev.keycode;

        /* Ctrl combos via control-char ASCII (matches PS/2 driver
         * cooking — 0x13 = ^S, 0x11 = ^Q etc). */
        if (a == 0x13) { save_file(); render(); continue; }
        if (a == 0x11) { if (g_dirty) save_file(); quit = 1; continue; }

        if (kc == OX_KEY_TAB) {
            g_in_ascii = !g_in_ascii;
            g_pending_nibble = -1;
            render();
            continue;
        }

        if (kc == OX_KEY_LEFT)  { cursor_left();  ensure_cursor_visible(); render(); continue; }
        if (kc == OX_KEY_RIGHT) { cursor_right(); ensure_cursor_visible(); render(); continue; }
        if (kc == OX_KEY_UP)    { cursor_up();    ensure_cursor_visible(); render(); continue; }
        if (kc == OX_KEY_DOWN)  { cursor_down();  ensure_cursor_visible(); render(); continue; }
        if (kc == OX_KEY_PGUP)  { cursor_pgup();  ensure_cursor_visible(); render(); continue; }
        if (kc == OX_KEY_PGDN)  { cursor_pgdn();  ensure_cursor_visible(); render(); continue; }
        if (kc == OX_KEY_HOME)  { cursor_home();  ensure_cursor_visible(); render(); continue; }
        if (kc == OX_KEY_END)   { cursor_end();   ensure_cursor_visible(); render(); continue; }

        if (a == '\b' || kc == OX_KEY_BACKSPACE) {
            /* Remove byte at cursor-1 (shift left). */
            if (g_cur > 0 && g_len > 0) {
                memmove(g_buf + g_cur - 1, g_buf + g_cur,
                        (size_t)(g_len - g_cur));
                g_len--;
                g_cur--;
                g_dirty = 1;
                g_pending_nibble = -1;
            }
            ensure_cursor_visible();
            render();
            continue;
        }
        if (kc == OX_KEY_DELETE) {
            if (g_cur < g_len) {
                memmove(g_buf + g_cur, g_buf + g_cur + 1,
                        (size_t)(g_len - g_cur - 1));
                g_len--;
                g_dirty = 1;
                g_pending_nibble = -1;
            }
            render();
            continue;
        }

        /* Typed character → edit. */
        if (g_in_ascii) {
            if (a >= 0x20 && a < 0x7f) {
                edit_ascii_byte(a);
                ensure_cursor_visible();
                render();
            }
        } else {
            int d = hex_digit(a);
            if (d >= 0) {
                edit_hex_nibble(d);
                ensure_cursor_visible();
                render();
            }
        }
    }
    ox_window_destroy(g_win);
    return 0;
}
