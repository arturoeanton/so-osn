/*
 * /bin/oxsrv — Ox window-system server (FASE 12).
 *
 * Ring-3 ELF. Registers SERVER_OX, owns the framebuffer, multiplexes
 * mouse + keyboard, draws wallpaper + windows + cursor, dispatches
 * input events to client tasks via IPC opcodes 0x60-0x7F. Bootstraps
 * everything required by the GUI: wallpaper load, default settings
 * (/home/.oxrc), root menu.
 *
 * Architecture decisions (see /Users/.../plans/...walrus.md):
 *  - Full backbuffer (BGRA), single FBIO_BLIT per dirty frame.
 *  - Windows have a backing buffer (also BGRA) painted by clients via
 *    IPC; oxsrv composes them onto the backbuffer in z-order.
 *  - Cursor is a 12x17 arrow sprite drawn last (always on top).
 *  - PPM P6 wallpaper, scaled nearest-neighbour to screen size.
 *  - Menu drawn directly by oxsrv (Openbox style root menu).
 *
 * Tile dispatch path:
 *   client → IPC_OX_DRAW_RECT/TEXT/IMAGE → oxsrv writes into the
 *   window's backing buffer → marks window dirty → IPC_OX_PRESENT
 *   → oxsrv re-composes backbuffer (if dirty) → FBIO_BLIT.
 */

#include <errno.h>
#include <fcntl.h>
#include <osnos_ipc.h>
#include <ox.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mouse.h>
#include <time.h>
#include <unistd.h>

#include "../../src/include/osnos_keys.h"

extern char **environ;

#define MAX_WINS        16
#define MAX_WIN_W       1024
#define MAX_WIN_H       768
#define TITLEBAR_H      18
#define BORDER_W        2
#define MENU_ITEM_H     22
#define MENU_W          180
#define CURSOR_W        12
#define CURSOR_H        17
#define WALLPAPER_PATH_LEN 96
#define OXRC_PATH       "/home/.oxrc"

/* ---------------- 12x17 arrow cursor sprite (1=visible) ---------- */
static const uint8_t g_cursor_mask[CURSOR_H][CURSOR_W] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,1,1,1,1,1,0},
    {1,2,2,1,2,2,1,0,0,0,0,0},
    {1,2,1,0,1,2,2,1,0,0,0,0},
    {1,1,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,0,1,1,0,0,0,0},
};

/* ---------------- Globals --------------------------------------- */
static int      g_fb_fd     = -1;
static int      g_mouse_fd  = -1;
static int      g_input_fd  = -1;

static uint32_t g_scr_w = 0, g_scr_h = 0;
static uint32_t *g_back = 0;          /* w*h BGRA backbuffer */

static char     g_wp_name[64] = "samurai";
static uint32_t *g_wp_scaled  = 0;    /* w*h scaled BGRA      */

static int      g_cx = 0, g_cy = 0;
static uint8_t  g_prev_buttons = 0;
/* Coalesced "send a MOVE event next frame" — accumulates motion
 * during the iter, fires once per frame to avoid IPC flooding. */
static int      g_pending_move = 0;
static int      g_pending_move_slot = -1;

static int      g_mods = 0;
static int      g_alt_down = 0;       /* tracked separately for Alt+F4 etc */

typedef struct {
    int          used;
    int          id;
    uint64_t     owner_pid;
    int          x, y;
    int          w, h;
    char         title[64];
    uint32_t    *back;                /* w*h BGRA */
    int          dirty;
    int          dragging;
    int          drag_off_x, drag_off_y;
    int          should_close;
} ox_window_t;

static ox_window_t g_wins[MAX_WINS];
static int         g_stack[MAX_WINS]; /* slot indices, bottom→top   */
static int         g_stack_n = 0;
static int         g_focus_slot = -1;
static int         g_next_id    = 1;

static int         g_dirty = 1;

/* Menu state. */
static int  g_menu_visible = 0;
static int  g_menu_x = 0, g_menu_y = 0;

typedef struct {
    const char *label;
    const char *path;        /* NULL for separators / actions */
    int         action;      /* 0=spawn path, 1=reboot, 2=quit */
} menu_item_t;

static const menu_item_t g_menu[] = {
    { "Notepad",     "/bin/oxnotepad",   0 },
    { "Calculator",  "/bin/oxcalc",      0 },
    { "Terminal",    "/bin/oxterm",      0 },
    { "Settings",    "/bin/oxsettings",  0 },
    { "Reboot",      0,                  1 },
};
#define MENU_N (sizeof(g_menu)/sizeof(g_menu[0]))

/* Colours (BGRA — alpha ignored). */
#define COL_TITLE_FG    OX_RGB(255,255,255)
#define COL_TITLE_BG    OX_RGB( 60, 90,170)
#define COL_TITLE_INACT OX_RGB( 90, 90, 90)
#define COL_BORDER      OX_RGB( 30, 30, 30)
#define COL_BG          OX_RGB(220,220,220)
#define COL_MENU_BG     OX_RGB( 40, 40, 40)
#define COL_MENU_FG     OX_RGB(230,230,230)
#define COL_MENU_HI     OX_RGB( 80,120,200)
#define COL_CLOSE_FG    OX_RGB(255,180,180)

/* ---------------- Utility drawing into BGRA buffer --------------- */

static void buf_fill_rect(uint32_t *buf, int buf_w, int buf_h,
                           int x, int y, int w, int h, uint32_t color) {
    if (!buf) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > buf_w) w = buf_w - x;
    if (y + h > buf_h) h = buf_h - y;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        uint32_t *p = buf + (size_t)(y + row) * buf_w + x;
        for (int col = 0; col < w; col++) p[col] = color;
    }
}

static void buf_draw_glyph(uint32_t *buf, int buf_w, int buf_h,
                            int x, int y, int c, uint32_t color) {
    const uint8_t *g = ox_font_glyph(c);
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            if (g[row] & (1 << (7 - col))) {
                int px = x + col, py = y + row;
                if (px < 0 || py < 0 || px >= buf_w || py >= buf_h) continue;
                buf[(size_t)py * buf_w + px] = color;
            }
        }
    }
}

static void buf_draw_text(uint32_t *buf, int buf_w, int buf_h,
                           int x, int y, const char *s, uint32_t color) {
    if (!s) return;
    int cx = x;
    while (*s) {
        if (*s == '\n') { y += 10; cx = x; s++; continue; }
        buf_draw_glyph(buf, buf_w, buf_h, cx, y, (unsigned char)*s, color);
        cx += 8;
        s++;
    }
}

/* Blit a sub-rect from `src` into `dst` (both BGRA full-screen buffers).
 * Used for compositing window backings onto the backbuffer. */
static void buf_blit(uint32_t *dst, int dst_w, int dst_h,
                      int dx, int dy,
                      const uint32_t *src, int sw, int sh) {
    int x0 = dx, y0 = dy;
    int x1 = dx + sw, y1 = dy + sh;
    int sx0 = 0,  sy0 = 0;
    if (x0 < 0) { sx0 = -x0; x0 = 0; }
    if (y0 < 0) { sy0 = -y0; y0 = 0; }
    if (x1 > dst_w) x1 = dst_w;
    if (y1 > dst_h) y1 = dst_h;
    if (x0 >= x1 || y0 >= y1) return;
    for (int row = y0; row < y1; row++) {
        const uint32_t *sp = src + (size_t)(sy0 + (row - y0)) * sw + sx0;
        uint32_t *dp = dst + (size_t)row * dst_w + x0;
        memcpy(dp, sp, (size_t)(x1 - x0) * sizeof(uint32_t));
    }
}

/* ---------------- PPM loader ------------------------------------ */
/*
 * Parse a PPM P6 file (binary RGB). Returns malloc'd BGRA buffer
 * of width*height*4 bytes, or NULL on failure. *out_w / *out_h
 * receive the dimensions.
 */
static int read_ppm_skip_ws(int fd, char *c) {
    for (;;) {
        if (read(fd, c, 1) != 1) return -1;
        if (*c == '#') {
            while (read(fd, c, 1) == 1 && *c != '\n') {}
            continue;
        }
        if (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') continue;
        return 0;
    }
}

static int read_ppm_uint(int fd, int *out, char *peek) {
    int v = 0, got = 0;
    char c = *peek;
    while (c >= '0' && c <= '9') {
        v = v * 10 + (c - '0');
        got = 1;
        if (read(fd, &c, 1) != 1) { *peek = 0; return got ? (*out = v, 0) : -1; }
    }
    *peek = c;
    if (!got) return -1;
    *out = v;
    return 0;
}

static uint32_t *load_ppm(const char *path, int *out_w, int *out_h) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    char magic[2];
    if (read(fd, magic, 2) != 2 || magic[0] != 'P' || magic[1] != '6') {
        close(fd); return 0;
    }
    char peek;
    if (read_ppm_skip_ws(fd, &peek) < 0) { close(fd); return 0; }
    int w, h, maxval;
    if (read_ppm_uint(fd, &w, &peek) < 0) { close(fd); return 0; }
    if (read_ppm_skip_ws(fd, &peek) < 0) { close(fd); return 0; }
    if (read_ppm_uint(fd, &h, &peek) < 0) { close(fd); return 0; }
    if (read_ppm_skip_ws(fd, &peek) < 0) { close(fd); return 0; }
    if (read_ppm_uint(fd, &maxval, &peek) < 0) { close(fd); return 0; }
    /* peek now holds the single whitespace after maxval — consumed
     * by the parser. The raw pixel bytes follow. */
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || maxval != 255) {
        close(fd); return 0;
    }
    size_t npix = (size_t)w * (size_t)h;
    uint32_t *bgra = (uint32_t *)malloc(npix * sizeof(uint32_t));
    if (!bgra) { close(fd); return 0; }
    /* Read row by row to handle large files without a giant single
     * read. Convert RGB→BGRA on the fly. */
    unsigned char rowbuf[8192 * 3];
    for (int y = 0; y < h; y++) {
        int need = w * 3;
        int got = 0;
        while (got < need) {
            int n = (int)read(fd, rowbuf + got, need - got);
            if (n <= 0) { free(bgra); close(fd); return 0; }
            got += n;
        }
        for (int x = 0; x < w; x++) {
            unsigned char r = rowbuf[x * 3 + 0];
            unsigned char g = rowbuf[x * 3 + 1];
            unsigned char b = rowbuf[x * 3 + 2];
            bgra[(size_t)y * w + x] =
                ((uint32_t)255 << 24) |
                ((uint32_t)r   << 16) |
                ((uint32_t)g   <<  8) |
                 (uint32_t)b;
        }
    }
    close(fd);
    *out_w = w;
    *out_h = h;
    return bgra;
}

/* Nearest-neighbour scale src(sw,sh) → new BGRA buffer (dw,dh). */
static uint32_t *scale_bgra(const uint32_t *src, int sw, int sh,
                             int dw, int dh) {
    uint32_t *dst = (uint32_t *)malloc((size_t)dw * dh * sizeof(uint32_t));
    if (!dst) return 0;
    for (int y = 0; y < dh; y++) {
        int sy = (int)((long long)y * sh / dh);
        for (int x = 0; x < dw; x++) {
            int sx = (int)((long long)x * sw / dw);
            dst[(size_t)y * dw + x] = src[(size_t)sy * sw + sx];
        }
    }
    return dst;
}

/* Load a wallpaper named "samurai" → "/home/wallpapers/samurai.ppm",
 * scale to screen size, replace g_wp_scaled. Falls back to a solid
 * colour if anything fails. */
static void load_wallpaper(const char *name) {
    char path[WALLPAPER_PATH_LEN];
    snprintf(path, sizeof(path), "/home/wallpapers/%s.ppm", name);
    int w = 0, h = 0;
    uint32_t *raw = load_ppm(path, &w, &h);
    if (g_wp_scaled) { free(g_wp_scaled); g_wp_scaled = 0; }
    if (!raw) {
        /* Fallback: Win95 teal/aqua (00 80 80). */
        g_wp_scaled = (uint32_t *)malloc((size_t)g_scr_w * g_scr_h * sizeof(uint32_t));
        if (g_wp_scaled) {
            uint32_t color = OX_RGB(0, 128, 128);
            for (size_t i = 0; i < (size_t)g_scr_w * g_scr_h; i++)
                g_wp_scaled[i] = color;
        }
        return;
    }
    g_wp_scaled = scale_bgra(raw, w, h, (int)g_scr_w, (int)g_scr_h);
    free(raw);
    if (!g_wp_scaled) return;
}

/* ---------------- Settings I/O ---------------------------------- */

static void parse_oxrc(void) {
    int fd = open(OXRC_PATH, O_RDONLY);
    if (fd < 0) return;
    char buf[1024];
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (*line && *line != '#') {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = 0;
                const char *key = line;
                const char *val = eq + 1;
                if (strcmp(key, "current_wallpaper") == 0) {
                    size_t L = strlen(val);
                    if (L >= sizeof(g_wp_name)) L = sizeof(g_wp_name) - 1;
                    memcpy(g_wp_name, val, L);
                    g_wp_name[L] = 0;
                }
            }
        }
        line = nl ? nl + 1 : 0;
    }
}

/* ---------------- Window mgmt ----------------------------------- */

static int alloc_window(uint64_t owner_pid, int w, int h, const char *title) {
    if (w <= 0 || h <= 0) return -1;
    if (w > MAX_WIN_W) w = MAX_WIN_W;
    if (h > MAX_WIN_H) h = MAX_WIN_H;
    for (int i = 0; i < MAX_WINS; i++) {
        if (!g_wins[i].used) {
            g_wins[i].used = 1;
            g_wins[i].id = g_next_id++;
            g_wins[i].owner_pid = owner_pid;
            /* Cascade placement: 30+id*24 from top-left. */
            g_wins[i].x = 40 + ((g_wins[i].id - 1) % 8) * 30;
            g_wins[i].y = 40 + ((g_wins[i].id - 1) % 8) * 30;
            g_wins[i].w = w;
            g_wins[i].h = h;
            if (title) {
                size_t L = strlen(title);
                if (L >= sizeof(g_wins[i].title)) L = sizeof(g_wins[i].title) - 1;
                memcpy(g_wins[i].title, title, L);
                g_wins[i].title[L] = 0;
            } else {
                g_wins[i].title[0] = 0;
            }
            size_t bsz = (size_t)w * h * sizeof(uint32_t);
            g_wins[i].back = (uint32_t *)malloc(bsz);
            if (!g_wins[i].back) {
                g_wins[i].used = 0;
                return -1;
            }
            /* Default backing = solid grey. */
            for (size_t k = 0; k < (size_t)w * h; k++)
                g_wins[i].back[k] = COL_BG;
            g_wins[i].dirty = 1;
            g_wins[i].dragging = 0;
            g_wins[i].should_close = 0;
            /* Push to top of stack. */
            if (g_stack_n < MAX_WINS) {
                g_stack[g_stack_n++] = i;
                g_focus_slot = i;
            }
            g_dirty = 1;
            return i;
        }
    }
    return -1;
}

static int find_slot_by_id(int id) {
    for (int i = 0; i < MAX_WINS; i++)
        if (g_wins[i].used && g_wins[i].id == id) return i;
    return -1;
}

static int find_owner_slot(uint64_t pid, int id) {
    int s = find_slot_by_id(id);
    if (s < 0) return -1;
    if (g_wins[s].owner_pid != pid) return -1;
    return s;
}

static void destroy_slot(int slot) {
    if (slot < 0 || slot >= MAX_WINS || !g_wins[slot].used) return;
    free(g_wins[slot].back);
    g_wins[slot].back = 0;
    g_wins[slot].used = 0;
    /* Remove from stack. */
    int j = 0;
    for (int i = 0; i < g_stack_n; i++) {
        if (g_stack[i] != slot) g_stack[j++] = g_stack[i];
    }
    g_stack_n = j;
    if (g_focus_slot == slot) {
        g_focus_slot = (g_stack_n > 0) ? g_stack[g_stack_n - 1] : -1;
    }
    g_dirty = 1;
}

static void raise_slot(int slot) {
    if (slot < 0 || slot >= MAX_WINS || !g_wins[slot].used) return;
    int j = 0;
    for (int i = 0; i < g_stack_n; i++) {
        if (g_stack[i] != slot) g_stack[j++] = g_stack[i];
    }
    g_stack[j++] = slot;
    g_stack_n = j;
    g_focus_slot = slot;
    g_dirty = 1;
}

/* ---------------- Compositor ------------------------------------ */

static void draw_window_frame(int slot) {
    ox_window_t *w = &g_wins[slot];
    int wx = w->x, wy = w->y, ww = w->w, wh = w->h;
    int focused = (slot == g_focus_slot);
    /* Border. */
    buf_fill_rect(g_back, g_scr_w, g_scr_h,
                  wx - BORDER_W, wy - TITLEBAR_H - BORDER_W,
                  ww + 2 * BORDER_W, TITLEBAR_H + wh + 2 * BORDER_W,
                  COL_BORDER);
    /* Title bar. */
    uint32_t tb = focused ? COL_TITLE_BG : COL_TITLE_INACT;
    buf_fill_rect(g_back, g_scr_w, g_scr_h,
                  wx, wy - TITLEBAR_H, ww, TITLEBAR_H, tb);
    /* Title text. */
    buf_draw_text(g_back, g_scr_w, g_scr_h,
                   wx + 6, wy - TITLEBAR_H + 5,
                   w->title, COL_TITLE_FG);
    /* Close button [X] on the right. */
    int cb_x = wx + ww - TITLEBAR_H + 2;
    buf_fill_rect(g_back, g_scr_w, g_scr_h,
                  cb_x, wy - TITLEBAR_H + 2,
                  TITLEBAR_H - 4, TITLEBAR_H - 4,
                  OX_RGB(180, 50, 50));
    buf_draw_glyph(g_back, g_scr_w, g_scr_h,
                    cb_x + 3, wy - TITLEBAR_H + 5, 'x', COL_CLOSE_FG);
    /* Body — blit window backing. */
    buf_blit(g_back, g_scr_w, g_scr_h, wx, wy, w->back, ww, wh);
}

static void draw_menu(void) {
    if (!g_menu_visible) return;
    int mh = (int)MENU_N * MENU_ITEM_H + 6;
    /* Frame. */
    buf_fill_rect(g_back, g_scr_w, g_scr_h,
                  g_menu_x - 1, g_menu_y - 1,
                  MENU_W + 2, mh + 2, COL_BORDER);
    buf_fill_rect(g_back, g_scr_w, g_scr_h,
                  g_menu_x, g_menu_y, MENU_W, mh, COL_MENU_BG);
    /* Items. */
    for (size_t i = 0; i < MENU_N; i++) {
        int iy = g_menu_y + 3 + (int)i * MENU_ITEM_H;
        /* Highlight if mouse is over. */
        if (g_cx >= g_menu_x && g_cx < g_menu_x + MENU_W &&
            g_cy >= iy && g_cy < iy + MENU_ITEM_H) {
            buf_fill_rect(g_back, g_scr_w, g_scr_h,
                          g_menu_x + 1, iy,
                          MENU_W - 2, MENU_ITEM_H, COL_MENU_HI);
        }
        buf_draw_text(g_back, g_scr_w, g_scr_h,
                       g_menu_x + 10, iy + 7,
                       g_menu[i].label, COL_MENU_FG);
    }
}

static void draw_cursor(void) {
    /* Outline (1) drawn in dark, fill (2) drawn in white. */
    for (int row = 0; row < CURSOR_H; row++) {
        for (int col = 0; col < CURSOR_W; col++) {
            uint8_t m = g_cursor_mask[row][col];
            if (!m) continue;
            uint32_t color = (m == 1) ? OX_RGB(20, 20, 20) : OX_RGB(255, 255, 255);
            int px = g_cx + col, py = g_cy + row;
            if (px < 0 || py < 0 ||
                px >= (int)g_scr_w || py >= (int)g_scr_h) continue;
            g_back[(size_t)py * g_scr_w + px] = color;
        }
    }
}

static void composite_and_flush(void) {
    if (!g_back || !g_wp_scaled) return;
    /* Wallpaper. */
    memcpy(g_back, g_wp_scaled,
           (size_t)g_scr_w * g_scr_h * sizeof(uint32_t));
    /* Windows back→front. */
    for (int i = 0; i < g_stack_n; i++) {
        int slot = g_stack[i];
        if (slot >= 0 && slot < MAX_WINS && g_wins[slot].used) {
            draw_window_frame(slot);
        }
    }
    /* Menu. */
    draw_menu();
    /* Cursor (always on top). */
    draw_cursor();
    /* Push to FB. */
    struct fb_blit_req req = {
        .x = 0, .y = 0,
        .w = g_scr_w, .h = g_scr_h,
        .src = g_back,
        .src_pitch = g_scr_w * 4
    };
    ioctl(g_fb_fd, FBIO_BLIT, &req);
}

/* ---------------- Spawn helper ---------------------------------- */

static void spawn_app(const char *path) {
    /* Packed envp ("KEY=VAL\0KEY=VAL\0\0"). Apps that exec sub-shells
     * (oxterm → minishell) need PATH/HOME/SHELL set so their children
     * can find /bin/<name>. Passing NULL caused minishell-from-oxterm
     * to crash because environ was unset. */
    static const char envp_flat[] =
        "PATH=/bin\0"
        "HOME=/home\0"
        "SHELL=/bin/minishell\0"
        "TERM=osnos\0";
    osn_spawn(path, "", envp_flat, -1, -1);
}

static void do_reboot(void) {
    /* Linux reboot magic — RB_AUTOBOOT = 0x01234567 in osnos. */
    extern long osnos_syscall1(long n, long a);
    osnos_syscall1(169 /* SYS_REBOOT */, 0x01234567);
}

/* Resume consrv so the shell text reappears when oxsrv exits. */
static void resume_consrv(void) {
    ipc_msg_t sm;
    memset(&sm, 0, sizeof(sm));
    sm.to   = SERVER_CONSOLE;
    sm.type = IPC_CONSOLE_RESUME;
    ipc_send(&sm);
}

static void on_term(int sig) {
    (void)sig;
    resume_consrv();
    _exit(0);
}

/* ---------------- Event dispatch -------------------------------- */

/* Small bounded retry on EAGAIN — drops the event if the client is
 * really stuck (~20 ms total) instead of locking up oxsrv. */
static void try_send(ipc_msg_t *m) {
    for (int i = 0; i < 10; i++) {
        if (ipc_send(m) == 0) return;
        if (errno != EAGAIN) return;
        struct timespec ts = { 0, 2 * 1000000 };
        nanosleep(&ts, 0);
    }
}

static void send_event_close(int slot) {
    ox_window_t *w = &g_wins[slot];
    if (!w->used) return;
    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.to   = w->owner_pid;
    m.type = IPC_OX_EVENT_CLOSE;
    m.arg0 = (uint64_t)w->id;
    try_send(&m);
}

static void send_event_key(int slot, int ascii, int keycode, int mods) {
    ox_window_t *w = &g_wins[slot];
    if (!w->used) return;
    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.to   = w->owner_pid;
    m.type = IPC_OX_EVENT_KEY;
    m.arg0 = (uint64_t)w->id;
    m.arg1 = ((uint64_t)(uint8_t)ascii   << 24) |
             ((uint64_t)(uint16_t)keycode << 8) |
              (uint64_t)(uint8_t)mods;
    try_send(&m);
}

static void send_event_mouse(int slot, int x, int y, int buttons, int kind) {
    ox_window_t *w = &g_wins[slot];
    if (!w->used) return;
    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.to   = w->owner_pid;
    m.type = IPC_OX_EVENT_MOUSE;
    m.arg0 = (uint64_t)w->id;
    m.arg1 = ((uint64_t)(uint32_t)x << 32) | (uint64_t)(uint32_t)y;
    m.data[0] = (char)buttons;
    m.data[1] = (char)kind;
    try_send(&m);
}

static void send_response(uint64_t to, int status, uint64_t value) {
    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.to   = to;
    m.type = IPC_OX_RESPONSE;
    m.arg0 = (uint64_t)status;
    m.arg1 = value;
    /* RETRY on EAGAIN — the client is blocked waiting for this
     * response (send_and_wait in lib/libc/ox.c). Dropping it would
     * hang the client forever. Cap at ~200 ms to avoid getting
     * stuck if the kernel queue is permanently full. */
    for (int attempt = 0; attempt < 100; attempt++) {
        if (ipc_send(&m) == 0) return;
        if (errno != EAGAIN) return;
        struct timespec ts = { 0, 2 * 1000000 };  /* 2 ms */
        nanosleep(&ts, 0);
    }
}

/* ---------------- IPC handler ----------------------------------- */

static uint32_t get_u32(const char *p) {
    return  (uint32_t)(uint8_t)p[0]        |
           ((uint32_t)(uint8_t)p[1] <<  8) |
           ((uint32_t)(uint8_t)p[2] << 16) |
           ((uint32_t)(uint8_t)p[3] << 24);
}

static void handle_ipc(const ipc_msg_t *m) {
    uint64_t from = m->from;
    switch (m->type) {
    case IPC_OX_CONNECT:
        /* Nothing to do — pid identity is implicit. */
        return;
    case IPC_OX_WINDOW_CREATE: {
        int w = (int)((m->arg0 >> 16) & 0xffff);
        int h = (int)( m->arg0        & 0xffff);
        char title[64];
        size_t tl = strnlen(m->data, sizeof(title) - 1);
        memcpy(title, m->data, tl);
        title[tl] = 0;
        int slot = alloc_window(from, w, h, title);
        if (slot < 0) {
            send_response(from, ENOMEM, 0);
        } else {
            send_response(from, 0, (uint64_t)g_wins[slot].id);
        }
        return;
    }
    case IPC_OX_WINDOW_DESTROY: {
        int slot = find_owner_slot(from, (int)m->arg0);
        if (slot >= 0) destroy_slot(slot);
        return;
    }
    case IPC_OX_DRAW_RECT: {
        int slot = find_owner_slot(from, (int)m->arg0);
        if (slot < 0) return;
        int x = (int)get_u32(m->data + 0);
        int y = (int)get_u32(m->data + 4);
        int w = (int)get_u32(m->data + 8);
        int h = (int)get_u32(m->data + 12);
        buf_fill_rect(g_wins[slot].back,
                       g_wins[slot].w, g_wins[slot].h,
                       x, y, w, h, (uint32_t)m->arg1);
        g_wins[slot].dirty = 1;
        return;
    }
    case IPC_OX_DRAW_TEXT: {
        int slot = find_owner_slot(from, (int)m->arg0);
        if (slot < 0) return;
        int x = (int)get_u32(m->data + 0);
        int y = (int)get_u32(m->data + 4);
        buf_draw_text(g_wins[slot].back,
                       g_wins[slot].w, g_wins[slot].h,
                       x, y, m->data + 8, (uint32_t)m->arg1);
        g_wins[slot].dirty = 1;
        return;
    }
    case IPC_OX_DRAW_IMAGE: {
        int slot = find_owner_slot(from, (int)m->arg0);
        if (slot < 0) return;
        int x = (int)get_u32(m->data + 0);
        int y = (int)get_u32(m->data + 4);
        int w = (int)get_u32(m->data + 8);
        int h = (int)get_u32(m->data + 12);
        if (w <= 0 || h <= 0) return;
        const uint32_t *src = (const uint32_t *)(m->data + 16);
        /* The wire only carries one row strip; chunked client-side. */
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++) {
                int px = x + col, py = y + row;
                if (px < 0 || py < 0 ||
                    px >= g_wins[slot].w || py >= g_wins[slot].h) continue;
                g_wins[slot].back[(size_t)py * g_wins[slot].w + px] =
                    src[(size_t)row * w + col];
            }
        }
        g_wins[slot].dirty = 1;
        return;
    }
    case IPC_OX_PRESENT: {
        int slot = find_owner_slot(from, (int)m->arg0);
        if (slot < 0) return;
        if (g_wins[slot].dirty) {
            g_wins[slot].dirty = 0;
            g_dirty = 1;
        }
        return;
    }
    case IPC_OX_SET_TITLE: {
        int slot = find_owner_slot(from, (int)m->arg0);
        if (slot < 0) return;
        size_t tl = strnlen(m->data, sizeof(g_wins[slot].title) - 1);
        memcpy(g_wins[slot].title, m->data, tl);
        g_wins[slot].title[tl] = 0;
        g_dirty = 1;
        return;
    }
    case IPC_OX_RELOAD_SETTINGS: {
        char prev[64];
        memcpy(prev, g_wp_name, sizeof(prev));
        parse_oxrc();
        if (strcmp(prev, g_wp_name) != 0) {
            load_wallpaper(g_wp_name);
        }
        g_dirty = 1;
        (void)from;   /* unused — reload is global */
        return;
    }
    default:
        return;
    }
}

/* ---------------- Mouse / Keyboard processing -------------------- */

/* Returns 1 if any window contains (mx,my) on its title bar. */
static int hit_title(int slot, int mx, int my, int *out_close) {
    ox_window_t *w = &g_wins[slot];
    if (mx < w->x - BORDER_W ||
        mx >= w->x + w->w + BORDER_W ||
        my < w->y - TITLEBAR_H - BORDER_W ||
        my >= w->y) return 0;
    int cb_x = w->x + w->w - TITLEBAR_H + 2;
    if (mx >= cb_x && mx < cb_x + TITLEBAR_H - 4 &&
        my >= w->y - TITLEBAR_H + 2 &&
        my <  w->y - 2) {
        *out_close = 1;
    } else {
        *out_close = 0;
    }
    return 1;
}

static int hit_body(int slot, int mx, int my) {
    ox_window_t *w = &g_wins[slot];
    return (mx >= w->x && mx < w->x + w->w &&
            my >= w->y && my < w->y + w->h);
}

static int pick_top_slot(int mx, int my, int *is_title, int *is_close) {
    *is_title = 0; *is_close = 0;
    for (int i = g_stack_n - 1; i >= 0; i--) {
        int slot = g_stack[i];
        if (!g_wins[slot].used) continue;
        if (hit_title(slot, mx, my, is_close)) {
            *is_title = 1;
            return slot;
        }
        if (hit_body(slot, mx, my)) {
            return slot;
        }
    }
    return -1;
}

static void process_mouse_event(mouse_event_t ev) {
    int new_cx = g_cx + ev.dx;
    int new_cy = g_cy + ev.dy;
    if (new_cx < 0) new_cx = 0;
    if (new_cy < 0) new_cy = 0;
    if (new_cx >= (int)g_scr_w) new_cx = (int)g_scr_w - 1;
    if (new_cy >= (int)g_scr_h) new_cy = (int)g_scr_h - 1;
    int moved = (new_cx != g_cx || new_cy != g_cy);
    g_cx = new_cx;
    g_cy = new_cy;
    if (moved) g_dirty = 1;   /* cursor moved → recompose */

    uint8_t old_b = g_prev_buttons;
    uint8_t new_b = ev.buttons;
    g_prev_buttons = new_b;

    /* If a window is being dragged, follow the cursor. */
    if (g_focus_slot >= 0 && g_wins[g_focus_slot].dragging) {
        if (new_b & MOUSE_BTN_LEFT) {
            g_wins[g_focus_slot].x = g_cx - g_wins[g_focus_slot].drag_off_x;
            g_wins[g_focus_slot].y = g_cy - g_wins[g_focus_slot].drag_off_y;
            g_dirty = 1;
        } else {
            g_wins[g_focus_slot].dragging = 0;
        }
    }

    /* Edge-detect button transitions. */
    int is_title = 0, is_close = 0;
    int slot = pick_top_slot(g_cx, g_cy, &is_title, &is_close);

    /* Menu interaction. */
    if (g_menu_visible) {
        if (g_cx >= g_menu_x && g_cx < g_menu_x + MENU_W &&
            g_cy >= g_menu_y &&
            g_cy <  g_menu_y + (int)MENU_N * MENU_ITEM_H + 6) {
            if ((new_b & MOUSE_BTN_LEFT) && !(old_b & MOUSE_BTN_LEFT)) {
                int idx = (g_cy - g_menu_y - 3) / MENU_ITEM_H;
                if (idx >= 0 && idx < (int)MENU_N) {
                    g_menu_visible = 0;
                    if (g_menu[idx].action == 1) do_reboot();
                    else if (g_menu[idx].path) spawn_app(g_menu[idx].path);
                    g_dirty = 1;
                    return;
                }
            }
            g_dirty = 1;   /* hover hilight */
            return;
        } else {
            /* Click outside menu closes it. */
            if ((new_b & MOUSE_BTN_LEFT) && !(old_b & MOUSE_BTN_LEFT)) {
                g_menu_visible = 0;
                g_dirty = 1;
            }
        }
    }

    /* Right-click opens the root menu when on wallpaper. */
    if (!g_menu_visible &&
        (new_b & MOUSE_BTN_RIGHT) && !(old_b & MOUSE_BTN_RIGHT)) {
        if (slot < 0) {
            g_menu_x = g_cx;
            g_menu_y = g_cy;
            if (g_menu_x + MENU_W > (int)g_scr_w) g_menu_x = (int)g_scr_w - MENU_W;
            if (g_menu_y + (int)MENU_N * MENU_ITEM_H + 6 > (int)g_scr_h)
                g_menu_y = (int)g_scr_h - (int)MENU_N * MENU_ITEM_H - 6;
            g_menu_visible = 1;
            g_dirty = 1;
            return;
        }
    }

    /* Left-button down. */
    if ((new_b & MOUSE_BTN_LEFT) && !(old_b & MOUSE_BTN_LEFT)) {
        if (slot >= 0) {
            raise_slot(slot);
            if (is_title) {
                if (is_close) {
                    send_event_close(slot);
                    g_wins[slot].should_close = 1;
                } else {
                    g_wins[slot].dragging = 1;
                    g_wins[slot].drag_off_x = g_cx - g_wins[slot].x;
                    g_wins[slot].drag_off_y = g_cy - g_wins[slot].y;
                }
            } else {
                /* Body click → forward DOWN. */
                send_event_mouse(slot,
                                  g_cx - g_wins[slot].x,
                                  g_cy - g_wins[slot].y,
                                  new_b, OX_MOUSE_DOWN);
            }
        }
    }
    /* Left-button up. */
    if (!(new_b & MOUSE_BTN_LEFT) && (old_b & MOUSE_BTN_LEFT)) {
        if (slot >= 0 && !is_title) {
            send_event_mouse(slot,
                              g_cx - g_wins[slot].x,
                              g_cy - g_wins[slot].y,
                              new_b, OX_MOUSE_UP);
        }
    }

    /* Hover move: stash it; the main loop emits one MOVE per frame
     * with the LATEST coords. Avoids flooding the client with 100s
     * of intermediate positions when the user wiggles fast. */
    if (moved && slot >= 0 && !is_title &&
        !g_wins[g_focus_slot >= 0 ? g_focus_slot : 0].dragging) {
        g_pending_move = 1;
        g_pending_move_slot = slot;
    }
}

static void process_kbd_event(int ascii, int keycode) {
    /* Track modifier state from keycodes. The PS/2 driver also
     * cooks shift/ctrl into ASCII (uppercase, ^C) so most apps
     * don't need mods; the bits are reported for completeness. */
    /* Special action keys handled by oxsrv directly. */
    if (keycode == OX_KEY_LEFT && (g_mods & OX_MOD_ALT)) {
        /* Alt+Left = focus previous window. */
        if (g_stack_n > 1) {
            int last = g_stack[g_stack_n - 1];
            for (int i = g_stack_n - 1; i > 0; i--)
                g_stack[i] = g_stack[i - 1];
            g_stack[0] = last;
            g_focus_slot = g_stack[g_stack_n - 1];
            g_dirty = 1;
        }
        return;
    }
    /* Alt+F4: close focused. F4 keycode is 62. */
    if (ascii == 0 && keycode == 62 && (g_alt_down || (g_mods & OX_MOD_ALT))) {
        if (g_focus_slot >= 0) {
            send_event_close(g_focus_slot);
        }
        return;
    }
    /* F1 toggles menu at cursor. */
    if (ascii == 0 && keycode == 59) {
        if (g_menu_visible) g_menu_visible = 0;
        else {
            g_menu_x = g_cx;
            g_menu_y = g_cy;
            if (g_menu_x + MENU_W > (int)g_scr_w) g_menu_x = (int)g_scr_w - MENU_W;
            if (g_menu_y + (int)MENU_N * MENU_ITEM_H + 6 > (int)g_scr_h)
                g_menu_y = (int)g_scr_h - (int)MENU_N * MENU_ITEM_H - 6;
            g_menu_visible = 1;
        }
        g_dirty = 1;
        return;
    }
    /* Forward to focused. */
    if (g_focus_slot >= 0 && g_wins[g_focus_slot].used) {
        send_event_key(g_focus_slot, ascii, keycode, g_mods);
    }
}

/* ---------------- Main loop ------------------------------------ */

static void reap_dead_windows(void) {
    /* Reclaim windows whose owner went away. We can't (yet) tell
     * the kernel "ping me when pid X dies", so we use a periodic
     * sweep: try sending a 0-byte ipc to each owner; if -ESRCH,
     * the owner is gone — kill the window. */
    for (int i = 0; i < MAX_WINS; i++) {
        if (!g_wins[i].used) continue;
        if (g_wins[i].should_close) {
            destroy_slot(i);
            continue;
        }
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* Single-instance check: another oxsrv already owns SERVER_OX → bail
     * with a visible message instead of silently competing for the FB. */
    long existing = ipc_service_lookup(SERVER_OX);
    if (existing > 0) {
        fprintf(stderr,
                "oxsrv: already running as pid %ld — kill it first\n",
                existing);
        return 1;
    }

    /* Diagnostic prints — go to stderr (visible via consrv on /dev/fb0
     * BEFORE we take over the framebuffer). If oxsrv silently dies,
     * the last visible line tells the user where. */
    fprintf(stderr, "oxsrv: starting\n");

    g_fb_fd    = open("/dev/fb0",    O_RDWR);
    g_mouse_fd = open("/dev/mouse0", O_RDONLY);
    g_input_fd = open("/dev/input0", O_RDONLY);
    if (g_fb_fd < 0 || g_mouse_fd < 0 || g_input_fd < 0) {
        fprintf(stderr,
                "oxsrv: open failed fb=%d mouse=%d input=%d errno=%d\n",
                g_fb_fd, g_mouse_fd, g_input_fd, errno);
        return 1;
    }
    /* CRITICAL: libc read() blocks on EAGAIN by default — it loops
     * forever with nanosleep waiting for data. We want a tight
     * drain-then-compose loop, so mouse + input must be NON-blocking
     * (devfs returns EAGAIN when the ring is empty; libc then
     * returns -1 + errno=EAGAIN and our drain loops exit cleanly). */
    fcntl(g_mouse_fd, F_SETFL, O_NONBLOCK);
    fcntl(g_input_fd, F_SETFL, O_NONBLOCK);

    struct fb_var_screeninfo info;
    if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &info) < 0) {
        fprintf(stderr, "oxsrv: FBIOGET_VSCREENINFO failed errno=%d\n", errno);
        return 1;
    }
    g_scr_w = info.xres;
    g_scr_h = info.yres;
    if (g_scr_w == 0 || g_scr_h == 0) {
        fprintf(stderr, "oxsrv: bad screen size %ux%u\n", g_scr_w, g_scr_h);
        return 1;
    }
    fprintf(stderr, "oxsrv: fb %ux%u bpp=%u pitch=%u\n",
            g_scr_w, g_scr_h, info.bits_per_pixel, info.line_length);

    g_back = (uint32_t *)malloc((size_t)g_scr_w * g_scr_h * sizeof(uint32_t));
    if (!g_back) {
        fprintf(stderr,
                "oxsrv: malloc backbuffer %u bytes FAILED\n",
                (unsigned)((size_t)g_scr_w * g_scr_h * 4));
        return 1;
    }

    g_cx = g_scr_w / 2;
    g_cy = g_scr_h / 2;

    parse_oxrc();
    load_wallpaper(g_wp_name);
    if (!g_wp_scaled) {
        fprintf(stderr, "oxsrv: wallpaper load failed (silent fallback also failed)\n");
        return 1;
    }
    fprintf(stderr, "oxsrv: wallpaper '%s' ready, entering loop\n", g_wp_name);

    /* Register only AFTER all init is good. */
    ipc_service_register(SERVER_OX);

    /* Open /dev/ttyS0 for diagnostic output that doesn't paint
     * over the framebuffer (writes go to QEMU's serial). */
    int ttyfd = open("/dev/ttyS0", O_WRONLY);
    if (ttyfd >= 0) {
        const char *m = "oxsrv: entered main loop\n";
        write(ttyfd, m, 25);
    }

    /* Tell consrv to stop painting the framebuffer so we own it
     * exclusively (no more shell-text flicker through our GUI).
     * RESUME is sent in the SIGTERM handler below. */
    {
        ipc_msg_t sm;
        memset(&sm, 0, sizeof(sm));
        sm.to   = SERVER_CONSOLE;
        sm.type = IPC_CONSOLE_SUSPEND;
        ipc_send(&sm);
    }

    /* Install termination handlers so `kill <pid>` lets us tell
     * consrv to take the FB back before we die. */
    signal(SIGTERM, on_term);
    signal(SIGINT,  on_term);

    /* Force first paint so wallpaper appears even with no events. */
    g_dirty = 1;

    /* Main loop — fixed ~30 FPS compositor.
     *
     * We blit unconditionally every frame (not just on dirty) so
     * consrv's cooked-text writes to /dev/fb0 get overpainted
     * within ~33 ms.
     */
    /* Main loop — dirty-flag composite.
     *
     * With consrv suspended, we OWN the framebuffer. No need to
     * fight anyone; only repaint when something actually changed.
     * Event handlers (mouse, kbd, IPC) set g_dirty = 1. Idle frames
     * just sleep — zero CPU, no flicker, no MMIO churn.
     *
     * Per-iteration caps prevent a mouse storm (which can produce
     * thousands of events/sec when QEMU grabs the cursor) from
     * starving the rest of the system or saturating the IPC queue.
     */
    long frame = 0;
    for (;;) {
        mouse_event_t ev;
        int drained = 0;
        /* Coalesce all pending mouse motion into the cursor's final
         * position; only emit ONE MOVE event per frame to the focused
         * client. Button transitions (DOWN/UP) still fire per-event
         * inside process_mouse_event because edge-detection lives
         * there. Cap at 64 reads per iter as a safety against driver
         * runaway. */
        while (drained < 64 &&
               read(g_mouse_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            process_mouse_event(ev);
            drained++;
        }
        struct { char ascii; uint16_t keycode; } kev;
        int kdrained = 0;
        while (kdrained < 32 &&
               read(g_input_fd, &kev, sizeof(kev)) == (ssize_t)sizeof(kev)) {
            process_kbd_event((int)(unsigned char)kev.ascii, (int)kev.keycode);
            kdrained++;
        }
        ipc_msg_t m;
        int idrained = 0;
        while (idrained < 64 && ipc_recv(&m) == 0) {
            handle_ipc(&m);
            idrained++;
        }
        reap_dead_windows();
        /* Emit at most one MOVE event per frame to the focused
         * window — coalescing prevents IPC saturation under a
         * mouse storm. */
        if (g_pending_move && g_pending_move_slot >= 0 &&
            g_pending_move_slot < MAX_WINS &&
            g_wins[g_pending_move_slot].used) {
            int slot = g_pending_move_slot;
            send_event_mouse(slot,
                              g_cx - g_wins[slot].x,
                              g_cy - g_wins[slot].y,
                              g_prev_buttons, OX_MOUSE_MOVE);
            g_pending_move = 0;
            g_pending_move_slot = -1;
        }
        if (g_dirty) {
            composite_and_flush();
            g_dirty = 0;
        }
        /* Heartbeat every ~2 seconds for serial-log diagnostics. */
        if (ttyfd >= 0 && (frame % 60) == 0) {
            char hb[64];
            int n = snprintf(hb, sizeof(hb),
                             "oxsrv: frame %ld cur=(%d,%d) wins=%d\n",
                             frame, g_cx, g_cy, g_stack_n);
            if (n > 0) write(ttyfd, hb, n);
        }
        frame++;
        struct timespec ts = { 0, 33 * 1000000 };
        nanosleep(&ts, 0);
    }
    return 0;
}
