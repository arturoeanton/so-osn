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
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mouse.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../src/include/osnos_keys.h"

extern char **environ;

#define MAX_WINS        16
#define MAX_WIN_W       1024
#define MAX_WIN_H       768
#define TITLEBAR_H      22         /* Adwaita-style headerbar */
#define BORDER_W        0          /* no visible border; shadow does the work */
#define CORNER_RADIUS   8
#define SHADOW_DEPTH    5
#define SHADOW_OFFSET   3          /* downward bias (light source from top) */
#define CLOSE_BTN_SIZE  14
#define CLOSE_BTN_PAD   ((TITLEBAR_H - CLOSE_BTN_SIZE) / 2)
#define MENU_ITEM_H     26
#define MENU_W          200
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
/* Composed snapshot WITHOUT the cursor. After every composite we
 * copy g_back → g_back_clean before drawing the cursor. Cursor-only
 * frames then restore the dirty rect from g_back_clean instead of
 * recomposing wallpaper + windows from scratch. Cuts cursor-frame
 * cost from O(window draws) to a single memcpy of the cursor bbox. */
static uint32_t *g_back_clean = 0;

static char     g_wp_name[64] = "wallpaper1";
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
    uint32_t    *back;                /* w*h BGRA — mmap-SHARED with client */
    char         shm_name[32];        /* "/oxw_<id>" — used for shm_unlink */
    size_t       back_bytes;          /* page-rounded mmap length */
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

/* Dirty-rect tracking — when only the cursor moved (most common case),
 * we know exactly what changed visually: old cursor area + new cursor
 * area. Re-composite only that region and blit only that region to FB.
 * Drops per-frame work from 1280×800 (1M pixels memcpy + blit) to
 * ~30×30 (~900 pixels) when cursor moves over wallpaper.
 * g_dirty_full=1 forces the legacy full-screen path. */
static int g_dirty_full = 1;
static int g_dirty_x0 = 0, g_dirty_y0 = 0, g_dirty_x1 = 0, g_dirty_y1 = 0;

/* Forward refs — counters defined later in the diagnostics block. */
static const char *g_last_full_reason = "init";
static uint64_t g_full_alloc, g_full_destroy, g_full_raise,
                g_full_reload, g_full_other;
static uint64_t g_destroy_us_total, g_destroy_us_max, g_destroy_count;
static uint64_t g_loop_iters;     /* main-loop iterations since boot */
/* Event counts — tell us how many mouse/kbd/ipc events actually
 * arrived. If iters is low and events are also low, the user just
 * isn't generating input. If iters is low but events are high, our
 * wake/drain isn't keeping up. */
static uint64_t g_mouse_events;
static uint64_t g_kbd_events;
static uint64_t g_ipc_events;
static uint64_t now_us(void);

static void mark_dirty_full_reason(const char *r) {
    g_dirty_full = 1;
    g_last_full_reason = r;
    if      (r[0]=='a') g_full_alloc++;
    else if (r[0]=='d') g_full_destroy++;
    else if (r[0]=='r' && r[1]=='a') g_full_raise++;
    else if (r[0]=='r' && r[1]=='e') g_full_reload++;
    else                g_full_other++;
}

static void mark_dirty(int x, int y, int w, int h) {
    if (g_dirty_full) return;
    int x1 = x + w, y1 = y + h;
    if (x  < 0)               x  = 0;
    if (y  < 0)               y  = 0;
    if (x1 > (int)g_scr_w)    x1 = (int)g_scr_w;
    if (y1 > (int)g_scr_h)    y1 = (int)g_scr_h;
    if (x >= x1 || y >= y1)   return;
    if (g_dirty_x0 >= g_dirty_x1) {            /* first dirty mark this frame */
        g_dirty_x0 = x;  g_dirty_y0 = y;
        g_dirty_x1 = x1; g_dirty_y1 = y1;
    } else {
        if (x  < g_dirty_x0) g_dirty_x0 = x;
        if (y  < g_dirty_y0) g_dirty_y0 = y;
        if (x1 > g_dirty_x1) g_dirty_x1 = x1;
        if (y1 > g_dirty_y1) g_dirty_y1 = y1;
    }
}

static int rect_intersects(int ax, int ay, int aw, int ah,
                            int bx0, int by0, int bx1, int by1) {
    int ax1 = ax + aw, ay1 = ay + ah;
    return ax < bx1 && ax1 > bx0 && ay < by1 && ay1 > by0;
}

/* Menu state. */
static int  g_menu_visible = 0;
static int  g_menu_x = 0, g_menu_y = 0;

typedef struct {
    const char *label;
    const char *path;        /* NULL for separators / actions */
    int         action;      /* 0=spawn path, 1=reboot, 2=quit */
} menu_item_t;

static const menu_item_t g_menu[] = {
    { "Files",       "/bin/oxfiles",     0 },
    { "Notepad",     "/bin/oxnotepad",   0 },
    { "Browser",     "/bin/oxbrowser",   0 },
    { "SQLite",      "/bin/oxsqliteview", 0 },
    { "Calculator",  "/bin/oxcalc",      0 },
    { "Terminal",    "/bin/oxterm",      0 },
    { "Processes",   "/bin/oxtop",       0 },
    { "Settings",    "/bin/oxsettings",  0 },
    { "Exit Ox",     0,                  2 },
    { "Reboot",      0,                  1 },
};

/* Set when "Exit Ox" is chosen — main loop breaks on next iteration. */
static int g_quit = 0;

/* System clipboard — single buffer shared across all Ox apps. Cap is
 * 1023 bytes so a single IPC roundtrip covers a SET/GET. Apps that
 * need larger clips will eventually move to an SHM-backed channel. */
static char g_clipboard[1024];
static int  g_clip_len = 0;
#define MENU_N (sizeof(g_menu)/sizeof(g_menu[0]))

/* Mark only the menu's bounding box as dirty. Every menu interaction
 * (open / close / hover / item-pick) touches at most this rect, so a
 * dirty-rect composite suffices — no full-screen repaint needed.
 * Sin esto, cada hover sobre un item del menu = 4 MB memcpy + 4 MB blit. */
static void mark_menu_dirty(void) {
    int mh = (int)MENU_N * MENU_ITEM_H + 8;
    mark_dirty(g_menu_x, g_menu_y, MENU_W, mh);
}

/* Adwaita dark palette (GNOME 40+). */
#define COL_HDR_BG      OX_RGB( 30, 30, 30)   /* #1e1e1e headerbar */
#define COL_HDR_INA     OX_RGB( 23, 23, 23)   /* darker when unfocused */
#define COL_BODY_BG     OX_RGB( 36, 36, 36)   /* #242424 window body */
#define COL_DIVIDER     OX_RGB( 18, 18, 18)   /* below headerbar */
#define COL_TITLE_FG    OX_RGB(255,255,255)
#define COL_TITLE_DIM   OX_RGB(154,153,150)   /* #9a9996 unfocused */
#define COL_BG          OX_RGB(220,220,220)   /* legacy */
#define COL_MENU_BG     OX_RGB( 36, 36, 36)   /* same as body */
#define COL_MENU_FG     OX_RGB(238,238,236)
#define COL_MENU_FG_HI  OX_RGB(255,255,255)
#define COL_MENU_HI     OX_RGB( 53,132,228)   /* #3584e4 Adwaita blue */
#define COL_MENU_SEP    OX_RGB( 56, 56, 56)
#define COL_CLOSE_BG    OX_RGB( 60, 60, 60)   /* subtle gray pill */
#define COL_CLOSE_BG_F  OX_RGB(192, 28, 40)   /* #c01c28 red on hover */
#define COL_CLOSE_FG    OX_RGB(255,255,255)

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

/* ---------------- Modern drawing primitives (Adwaita dark) ------- */

/* Fast alpha-blend using >>8 instead of /255. Slightly biased
 * (alpha=255 produces 254 instead of 255) but visually identical
 * and ~3x faster than the exact integer path. */
static inline uint32_t blend_fast(uint32_t dst, uint32_t color, uint32_t alpha) {
    uint32_t inv = 256u - alpha;
    uint32_t sr = (color >> 16) & 0xFF, sg = (color >> 8) & 0xFF, sb = color & 0xFF;
    uint32_t dr = (dst   >> 16) & 0xFF, dg = (dst   >> 8) & 0xFF, db = dst   & 0xFF;
    uint32_t r = (sr * alpha + dr * inv) >> 8;
    uint32_t g = (sg * alpha + dg * inv) >> 8;
    uint32_t b = (sb * alpha + db * inv) >> 8;
    return (r << 16) | (g << 8) | b;
}

/* Bulk-blend a solid color over a rectangular region. Inner loop is
 * inlined (no per-pixel function call), bounds are clipped once. */
static void buf_blend_rect(uint32_t *buf, int bw, int bh,
                            int x, int y, int w, int h,
                            uint32_t color, uint8_t alpha) {
    if (alpha == 0 || !buf) return;
    if (alpha >= 255) { buf_fill_rect(buf, bw, bh, x, y, w, h, color); return; }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > bw) w = bw - x;
    if (y + h > bh) h = bh - y;
    if (w <= 0 || h <= 0) return;
    uint32_t a = (uint32_t)alpha;
    for (int row = 0; row < h; row++) {
        uint32_t *p = buf + (size_t)(y + row) * bw + x;
        for (int col = 0; col < w; col++) {
            p[col] = blend_fast(p[col], color, a);
        }
    }
}

/* Filled rounded rectangle — corner pixels outside the inscribed
 * circles are skipped (left as wallpaper). r=0 falls through to a
 * plain rect. */
static void buf_fill_rounded(uint32_t *buf, int bw, int bh,
                              int x, int y, int w, int h, int r,
                              uint32_t color) {
    if (r <= 0) { buf_fill_rect(buf, bw, bh, x, y, w, h, color); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    buf_fill_rect(buf, bw, bh, x, y + r, w, h - 2 * r, color);
    int r2 = r * r;
    for (int dy = 0; dy < r; dy++) {
        int span = 0;
        for (int dx = 0; dx < r; dx++) {
            int ex = r - 1 - dx, ey = r - 1 - dy;
            if (ex * ex + ey * ey <= r2) { span = r - dx; break; }
        }
        int row_w = w - 2 * (r - span);
        int row_x = x + (r - span);
        buf_fill_rect(buf, bw, bh, row_x, y + dy,         row_w, 1, color);
        buf_fill_rect(buf, bw, bh, row_x, y + h - 1 - dy, row_w, 1, color);
    }
}

/* buf_blit variant that masks the bottom-N rows by an inscribed circle
 * radius r — so the bottom corners stay un-touched (preserving the
 * rounded background they're sitting on). Used for the body blit so
 * client rectangular content doesn't square off the rounded window. */
static void buf_blit_rounded_bot(uint32_t *dst, int dw, int dh,
                                  int dx, int dy,
                                  const uint32_t *src, int sw, int sh,
                                  int r) {
    if (r <= 0) {
        buf_blit(dst, dw, dh, dx, dy, src, sw, sh);
        return;
    }
    if (r > sw / 2) r = sw / 2;
    if (r > sh / 2) r = sh / 2;
    int r2 = r * r;
    int x0 = dx, y0 = dy;
    int x1 = dx + sw, y1 = dy + sh;
    int sx0 = 0, sy0 = 0;
    if (x0 < 0) { sx0 = -x0; x0 = 0; }
    if (y0 < 0) { sy0 = -y0; y0 = 0; }
    if (x1 > dw) x1 = dw;
    if (y1 > dh) y1 = dh;
    if (x0 >= x1 || y0 >= y1) return;
    int sh_top = sh - r;            /* rows above the rounded region */
    for (int row = y0; row < y1; row++) {
        int s_row = sy0 + (row - y0);
        const uint32_t *sp = src + (size_t)s_row * sw + sx0;
        uint32_t *dp = dst + (size_t)row * dw + x0;
        if (s_row < sh_top) {
            /* Above corner region — full-width copy. */
            memcpy(dp, sp, (size_t)(x1 - x0) * sizeof(uint32_t));
        } else {
            /* In the bottom corner region. Compute the inscribed span
             * for this row and skip pixels outside it. */
            int dy_corner = s_row - sh_top;            /* 0..r-1 */
            int span = 0;
            for (int dx2 = 0; dx2 < r; dx2++) {
                int ex = r - 1 - dx2, ey = dy_corner;
                if (ex * ex + ey * ey <= r2) { span = r - dx2; break; }
            }
            int row_inset = r - span;                  /* px skipped each side */
            int row_w = sw - 2 * row_inset;
            int copy_x0 = x0 + row_inset, copy_x1 = x1 - row_inset;
            if (copy_x0 < copy_x1) {
                memcpy(dst + (size_t)row * dw + copy_x0,
                       src + (size_t)s_row * sw + sx0 + row_inset,
                       (size_t)(copy_x1 - copy_x0) * sizeof(uint32_t));
            }
            (void)row_w;
        }
    }
}

/* Soft drop shadow — paints each layer's EXPOSED WINGS only (the
 * layer's rect minus the frame area). Adjacent layers' wings overlap
 * for pixels closer to the frame; each overlap stacks alpha-over,
 * giving correct gradient falloff identical to the old solid-nested-
 * rect approach but at ~25x less work (skips the interior that the
 * frame would overwrite anyway). */
static void buf_draw_shadow(uint32_t *buf, int bw, int bh,
                             int x, int y, int w, int h) {
    /* Raw per-layer alphas (outer → inner). Painted outer-first so
     * the inner-frame area accumulates darker on top. */
    static const uint8_t raw_alpha[SHADOW_DEPTH] = { 15, 25, 40, 60, 90 };
    uint32_t color = OX_RGB(0, 0, 0);
    int oy = SHADOW_OFFSET;
    for (int i = 0; i < SHADOW_DEPTH; i++) {
        int m = SHADOW_DEPTH - i;                /* margin: 5 → 1 (outer → inner) */
        uint8_t a = raw_alpha[i];
        int sx = x - m;
        int sy = y - m + oy;
        int sw = w + 2 * m;
        int sh = h + 2 * m;
        int sx2 = sx + sw;     /* exclusive */
        int sy2 = sy + sh;
        int fx2 = x + w;
        int fy2 = y + h;
        /* Top wing — above frame top, full strip width. */
        if (sy < y) {
            buf_blend_rect(buf, bw, bh,
                           sx, sy, sw, y - sy, color, a);
        }
        /* Bottom wing — below frame bottom. */
        if (sy2 > fy2) {
            buf_blend_rect(buf, bw, bh,
                           sx, fy2, sw, sy2 - fy2, color, a);
        }
        /* Left wing — left of frame, restricted to frame's Y range. */
        int wy0 = sy > y      ? sy  : y;
        int wy1 = sy2 < fy2   ? sy2 : fy2;
        if (sx < x && wy0 < wy1) {
            buf_blend_rect(buf, bw, bh,
                           sx, wy0, x - sx, wy1 - wy0, color, a);
        }
        /* Right wing. */
        if (sx2 > fx2 && wy0 < wy1) {
            buf_blend_rect(buf, bw, bh,
                           fx2, wy0, sx2 - fx2, wy1 - wy0, color, a);
        }
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
            /* Window backing via SHARED MEMORY (shm_open + mmap MAP_SHARED).
             * Both oxsrv AND the client mmap the same underlying pages,
             * so the client can draw locally with no IPC traffic — only
             * ox_present (1 IPC) tells oxsrv "frame ready". Wallpapers
             * + thumbnails that used to cost 1200 IPCs per render now
             * cost 1. */
            size_t bsz = (size_t)w * h * sizeof(uint32_t);
            size_t mmap_bsz = (bsz + 4095) & ~(size_t)4095;
            snprintf(g_wins[i].shm_name, sizeof(g_wins[i].shm_name),
                     "/oxw_%d", g_wins[i].id);
            int shmfd = shm_open(g_wins[i].shm_name,
                                 O_CREAT | O_RDWR, 0600);
            if (shmfd < 0) { g_wins[i].used = 0; return -1; }
            if (ftruncate(shmfd, (off_t)mmap_bsz) < 0) {
                close(shmfd); shm_unlink(g_wins[i].shm_name);
                g_wins[i].used = 0; return -1;
            }
            void *p = mmap(0, mmap_bsz,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED, shmfd, 0);
            close(shmfd);  /* mmap holds the page references */
            if (!p || p == (void *)-1) {
                shm_unlink(g_wins[i].shm_name);
                g_wins[i].used = 0; return -1;
            }
            g_wins[i].back = (uint32_t *)p;
            g_wins[i].back_bytes = mmap_bsz;
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
            mark_dirty_full_reason("alloc");
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
    uint64_t t_destroy0 = now_us();
    if (g_wins[slot].back && g_wins[slot].back_bytes) {
        munmap(g_wins[slot].back, g_wins[slot].back_bytes);
    }
    /* shm_unlink so the name slot can be reused. Refcount of the
     * underlying object drops here; the client also munmap'd on its
     * side (or will when it dies), and the last unref frees the
     * physical pages back to the PMM. */
    if (g_wins[slot].shm_name[0]) {
        shm_unlink(g_wins[slot].shm_name);
        g_wins[slot].shm_name[0] = 0;
    }
    uint64_t dt = now_us() - t_destroy0;
    g_destroy_us_total += dt;
    if (dt > g_destroy_us_max) g_destroy_us_max = dt;
    g_destroy_count++;
    g_wins[slot].back = 0;
    g_wins[slot].back_bytes = 0;
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
    mark_dirty_full_reason("destroy");
}

static void raise_slot(int slot) {
    if (slot < 0 || slot >= MAX_WINS || !g_wins[slot].used) return;
    /* No-op if already on top — saves a full repaint on every click
     * within the same focused window. Big win for oxsettings/oxfiles
     * where each tile click hits raise_slot. */
    if (g_stack_n > 0 && g_stack[g_stack_n - 1] == slot &&
        g_focus_slot == slot) {
        return;
    }
    int j = 0;
    for (int i = 0; i < g_stack_n; i++) {
        if (g_stack[i] != slot) g_stack[j++] = g_stack[i];
    }
    g_stack[j++] = slot;
    g_stack_n = j;
    g_focus_slot = slot;
    g_dirty = 1;
    mark_dirty_full_reason("raise");
}

/* ---------------- Compositor ------------------------------------ */

static void draw_window_frame(int slot) {
    ox_window_t *w = &g_wins[slot];
    int wx = w->x, wy = w->y, ww = w->w, wh = w->h;
    int focused = (slot == g_focus_slot);

    /* Frame bounds = headerbar + body (no extra border — shadow alone
     * separates the window from the wallpaper). */
    int fx = wx;
    int fy = wy - TITLEBAR_H;
    int fw = ww;
    int fh = TITLEBAR_H + wh;

    /* 1. Soft drop shadow. */
    buf_draw_shadow(g_back, g_scr_w, g_scr_h, fx, fy, fw, fh);

    /* 2. Rounded outer shape — unified body color first. The headerbar
     * overpaints the top region; bottom corners stay body-colored. */
    buf_fill_rounded(g_back, g_scr_w, g_scr_h,
                     fx, fy, fw, fh, CORNER_RADIUS, COL_BODY_BG);

    /* 3. Headerbar — flat color, only top corners rounded. We paint the
     * full-width rect over the rounded body's top region. To preserve
     * the top rounded corners we use buf_fill_rounded with the same
     * radius and full headerbar height (the corner clipping clips the
     * top corners; the bottom row is just one flat line — accepted
     * since the divider below covers any seam). */
    uint32_t hdr = focused ? COL_HDR_BG : COL_HDR_INA;
    buf_fill_rounded(g_back, g_scr_w, g_scr_h,
                     fx, fy, fw, TITLEBAR_H, CORNER_RADIUS, hdr);

    /* 4. 1-px subtle divider between headerbar and body. */
    buf_fill_rect(g_back, g_scr_w, g_scr_h,
                  fx + CORNER_RADIUS, fy + TITLEBAR_H - 1,
                  fw - 2 * CORNER_RADIUS, 1, COL_DIVIDER);

    /* 5. Title text — centered horizontally in the headerbar. */
    int title_w = 0;
    for (const char *p = w->title; *p; p++) title_w += 8;
    int text_x = fx + (fw - title_w) / 2;
    int text_y = fy + (TITLEBAR_H - 8) / 2;
    buf_draw_text(g_back, g_scr_w, g_scr_h,
                  text_x, text_y, w->title,
                  focused ? COL_TITLE_FG : COL_TITLE_DIM);

    /* 6. Close button — circular pill on the right. Subtle gray by
     * default, red on hover (Adwaita pattern). */
    int cb_x = fx + fw - CLOSE_BTN_SIZE - CLOSE_BTN_PAD - 2;
    int cb_y = fy + CLOSE_BTN_PAD;
    int cb_hover = (g_cx >= cb_x && g_cx < cb_x + CLOSE_BTN_SIZE &&
                    g_cy >= cb_y && g_cy < cb_y + CLOSE_BTN_SIZE);
    buf_fill_rounded(g_back, g_scr_w, g_scr_h,
                     cb_x, cb_y, CLOSE_BTN_SIZE, CLOSE_BTN_SIZE,
                     CLOSE_BTN_SIZE / 2,
                     cb_hover ? COL_CLOSE_BG_F : COL_CLOSE_BG);
    buf_draw_glyph(g_back, g_scr_w, g_scr_h,
                   cb_x + (CLOSE_BTN_SIZE - 8) / 2,
                   cb_y + (CLOSE_BTN_SIZE - 8) / 2,
                   'x', COL_CLOSE_FG);

    /* 7. Body — blit client content with rounded-bottom mask so the
     * window's bottom corners stay rounded (Adwaita style). */
    buf_blit_rounded_bot(g_back, g_scr_w, g_scr_h, wx, wy,
                         w->back, ww, wh, CORNER_RADIUS);
}

static void draw_menu(void) {
    if (!g_menu_visible) return;
    int mh = (int)MENU_N * MENU_ITEM_H + 8;
    int mx = g_menu_x, my = g_menu_y;

    /* Shadow under the menu (same primitive as windows). */
    buf_draw_shadow(g_back, g_scr_w, g_scr_h, mx, my, MENU_W, mh);

    /* Rounded dark background. */
    buf_fill_rounded(g_back, g_scr_w, g_scr_h,
                     mx, my, MENU_W, mh, CORNER_RADIUS, COL_MENU_BG);

    /* Items. */
    for (size_t i = 0; i < MENU_N; i++) {
        int iy = my + 4 + (int)i * MENU_ITEM_H;
        int hovered = (g_cx >= mx && g_cx < mx + MENU_W &&
                       g_cy >= iy && g_cy < iy + MENU_ITEM_H);
        if (hovered) {
            /* Adwaita accent blue, rounded pill inside the menu. */
            buf_fill_rounded(g_back, g_scr_w, g_scr_h,
                             mx + 6, iy, MENU_W - 12, MENU_ITEM_H,
                             4, COL_MENU_HI);
        } else if (i > 0) {
            /* Subtle separator between non-hovered items. */
            buf_blend_rect(g_back, g_scr_w, g_scr_h,
                           mx + 14, iy, MENU_W - 28, 1,
                           COL_MENU_SEP, 180);
        }
        buf_draw_text(g_back, g_scr_w, g_scr_h,
                      mx + 16, iy + (MENU_ITEM_H - 8) / 2,
                      g_menu[i].label,
                      hovered ? COL_MENU_FG_HI : COL_MENU_FG);
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

/* Copy a rect from g_back_clean → g_back. Used when only the cursor
 * moved: we don't need to recompose, just restore the underlying
 * pixels at old cursor position from the cached "no-cursor" frame. */
static void restore_rect_from_clean(int dx0, int dy0, int dx1, int dy1) {
    int dw = dx1 - dx0;
    for (int row = dy0; row < dy1; row++) {
        memcpy(g_back       + (size_t)row * g_scr_w + dx0,
               g_back_clean + (size_t)row * g_scr_w + dx0,
               (size_t)dw * sizeof(uint32_t));
    }
}

/* Full recomposition WITHOUT cursor — used to rebuild g_back_clean
 * whenever a non-cursor change happens. Caller is responsible for
 * snapshotting to g_back_clean and drawing the cursor afterwards. */
static void compose_no_cursor(void) {
    memcpy(g_back, g_wp_scaled,
           (size_t)g_scr_w * g_scr_h * sizeof(uint32_t));
    for (int i = 0; i < g_stack_n; i++) {
        int slot = g_stack[i];
        if (slot >= 0 && slot < MAX_WINS && g_wins[slot].used) {
            draw_window_frame(slot);
        }
    }
    draw_menu();
}

/* Partial recomposition — restore only `dirty rect` of the no-cursor
 * image, by replaying wallpaper + intersecting windows + menu. Used
 * when window content updated (IPC_OX_PRESENT) and we want to refresh
 * only the affected zone without a full screen rebuild. */
static void compose_no_cursor_dirty(int dx0, int dy0, int dx1, int dy1) {
    int dw = dx1 - dx0;
    for (int row = dy0; row < dy1; row++) {
        memcpy(g_back       + (size_t)row * g_scr_w + dx0,
               g_wp_scaled  + (size_t)row * g_scr_w + dx0,
               (size_t)dw * sizeof(uint32_t));
    }
    for (int i = 0; i < g_stack_n; i++) {
        int slot = g_stack[i];
        if (slot < 0 || slot >= MAX_WINS || !g_wins[slot].used) continue;
        ox_window_t *w = &g_wins[slot];
        int fx = w->x - SHADOW_DEPTH;
        int fy = w->y - TITLEBAR_H - SHADOW_DEPTH;
        int fw = w->w + 2 * SHADOW_DEPTH;
        int fh = TITLEBAR_H + w->h + 2 * SHADOW_DEPTH + SHADOW_OFFSET;
        if (rect_intersects(fx, fy, fw, fh, dx0, dy0, dx1, dy1)) {
            draw_window_frame(slot);
        }
    }
    if (g_menu_visible) {
        int mh = (int)MENU_N * MENU_ITEM_H + 8;
        if (rect_intersects(g_menu_x, g_menu_y, MENU_W, mh,
                            dx0, dy0, dx1, dy1)) {
            draw_menu();
        }
    }
}

/* Copy a rect from g_back → g_back_clean to keep the cache in sync
 * after we just drew non-cursor content into g_back. */
static void snapshot_to_clean(int dx0, int dy0, int dx1, int dy1) {
    int dw = dx1 - dx0;
    for (int row = dy0; row < dy1; row++) {
        memcpy(g_back_clean + (size_t)row * g_scr_w + dx0,
               g_back       + (size_t)row * g_scr_w + dx0,
               (size_t)dw * sizeof(uint32_t));
    }
}

static void blit_rect(int dx0, int dy0, int dx1, int dy1) {
    struct fb_blit_req req = {
        .x = (uint32_t)dx0, .y = (uint32_t)dy0,
        .w = (uint32_t)(dx1 - dx0), .h = (uint32_t)(dy1 - dy0),
        .src = g_back + (size_t)dy0 * g_scr_w + dx0,
        .src_pitch = g_scr_w * 4
    };
    ioctl(g_fb_fd, FBIO_BLIT, &req);
}

/* Track the cursor's previous draw position so we know what region
 * to restore from the clean cache. */
static int g_last_cursor_x = -1, g_last_cursor_y = -1;

/* Diagnostic counters — included in heartbeat to see what triggered
 * a slowdown. composite_full is the heavy path (~12 MB of memcpy);
 * composite_dirty stays cheap. If full_count grows after a close,
 * something is repeatedly invalidating the cache. */
static uint64_t g_composite_full_count = 0;
static uint64_t g_composite_dirty_count = 0;
static uint64_t g_ipc_drained_count = 0;
static uint64_t g_full_us_total = 0;     /* sum of microsec spent in full */
static uint64_t g_dirty_us_total = 0;    /* sum of microsec spent in dirty */
static uint64_t g_full_us_max = 0;       /* worst single full repaint */
static uint64_t g_dirty_us_max = 0;      /* worst single dirty repaint */
static uint64_t g_dirty_area_total = 0;  /* sum of dirty rect pixels */

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(0, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void composite_and_flush(void) {
    if (!g_back || !g_back_clean || !g_wp_scaled) return;

    int full = g_dirty_full || g_dirty_x0 >= g_dirty_x1;
    uint64_t t0 = now_us();
    uint64_t dirty_area = 0;
    if (full) {
        g_composite_full_count++;
    } else {
        g_composite_dirty_count++;
        dirty_area = (uint64_t)(g_dirty_x1 - g_dirty_x0) *
                     (uint64_t)(g_dirty_y1 - g_dirty_y0);
        g_dirty_area_total += dirty_area;
    }
    if (full) {
        /* Full: rebuild no-cursor + cache, then draw cursor on top. */
        compose_no_cursor();
        memcpy(g_back_clean, g_back,
               (size_t)g_scr_w * g_scr_h * sizeof(uint32_t));
        draw_cursor();
        g_last_cursor_x = g_cx; g_last_cursor_y = g_cy;
        struct fb_blit_req req = {
            .x = 0, .y = 0, .w = g_scr_w, .h = g_scr_h,
            .src = g_back, .src_pitch = g_scr_w * 4
        };
        ioctl(g_fb_fd, FBIO_BLIT, &req);
    } else {
        /* Compute the cursor's old + new bbox union. Anything else
         * in the dirty rect counts as "content changed" — we rebuild
         * just that subregion of the clean cache before re-drawing
         * the cursor. */
        int cx0 = g_last_cursor_x >= 0 ? g_last_cursor_x : g_cx;
        int cy0 = g_last_cursor_y >= 0 ? g_last_cursor_y : g_cy;
        int old_l = cx0, old_t = cy0;
        int old_r = cx0 + CURSOR_W, old_b = cy0 + CURSOR_H;
        int new_l = g_cx, new_t = g_cy;
        int new_r = g_cx + CURSOR_W, new_b = g_cy + CURSOR_H;
        int cur_l = old_l < new_l ? old_l : new_l;
        int cur_t = old_t < new_t ? old_t : new_t;
        int cur_r = old_r > new_r ? old_r : new_r;
        int cur_b = old_b > new_b ? old_b : new_b;

        /* Is the dirty rect bigger than just the cursor union? If so,
         * some non-cursor content changed and we need to refresh the
         * clean cache for that region. */
        int content_dirty = (g_dirty_x0 < cur_l) || (g_dirty_y0 < cur_t) ||
                            (g_dirty_x1 > cur_r) || (g_dirty_y1 > cur_b);
        if (content_dirty) {
            int cx0r = g_dirty_x0, cy0r = g_dirty_y0;
            int cx1r = g_dirty_x1, cy1r = g_dirty_y1;
            compose_no_cursor_dirty(cx0r, cy0r, cx1r, cy1r);
            snapshot_to_clean(cx0r, cy0r, cx1r, cy1r);
        }
        /* Restore old cursor area from cache (wipes the cursor). */
        if (g_last_cursor_x >= 0) {
            int rx0 = old_l, ry0 = old_t, rx1 = old_r, ry1 = old_b;
            if (rx0 < 0) rx0 = 0;
            if (ry0 < 0) ry0 = 0;
            if (rx1 > (int)g_scr_w) rx1 = g_scr_w;
            if (ry1 > (int)g_scr_h) ry1 = g_scr_h;
            if (rx0 < rx1 && ry0 < ry1)
                restore_rect_from_clean(rx0, ry0, rx1, ry1);
        }
        /* Draw cursor at new position. */
        draw_cursor();
        g_last_cursor_x = g_cx; g_last_cursor_y = g_cy;

        /* Blit the union of dirty rect + cursor union. */
        int bx0 = g_dirty_x0 < cur_l ? g_dirty_x0 : cur_l;
        int by0 = g_dirty_y0 < cur_t ? g_dirty_y0 : cur_t;
        int bx1 = g_dirty_x1 > cur_r ? g_dirty_x1 : cur_r;
        int by1 = g_dirty_y1 > cur_b ? g_dirty_y1 : cur_b;
        if (bx0 < 0) bx0 = 0;
        if (by0 < 0) by0 = 0;
        if (bx1 > (int)g_scr_w) bx1 = g_scr_w;
        if (by1 > (int)g_scr_h) by1 = g_scr_h;
        if (bx0 < bx1 && by0 < by1) blit_rect(bx0, by0, bx1, by1);
    }
    g_dirty_full = 0;
    g_dirty_x0 = g_dirty_x1 = 0;
    g_dirty_y0 = g_dirty_y1 = 0;
    uint64_t dt = now_us() - t0;
    if (full) {
        g_full_us_total += dt;
        if (dt > g_full_us_max) g_full_us_max = dt;
    } else {
        g_dirty_us_total += dt;
        if (dt > g_dirty_us_max) g_dirty_us_max = dt;
        /* Track dirty rect area to see if Settings is dirtying huge
         * rects. dx0/x1 were already cleared above; use saved values. */
    }
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

/* Resume consrv + kbdsrv so the shell + TTY come back when oxsrv exits.
 * After consrv's RESUME (which clears the FB), we queue a follow-up
 * IPC_CONSOLE_WRITE with a faux shell prompt so the user sees something
 * useful immediately instead of a black screen until they press Enter.
 * Same IPC queue is FIFO so the WRITE is guaranteed to land AFTER the
 * RESUME's clear. */
static void resume_underlings(void) {
    ipc_msg_t sm;
    memset(&sm, 0, sizeof(sm));
    sm.to   = SERVER_CONSOLE;
    sm.type = IPC_CONSOLE_RESUME;
    ipc_send(&sm);
    /* Follow-up: faux prompt so the cursor position is at a useful
     * spot when the user starts typing. The shell is still running;
     * any keystrokes go to its real stdin and the line discipline
     * echoes at this cursor position. */
    memset(&sm, 0, sizeof(sm));
    sm.to   = SERVER_CONSOLE;
    sm.type = IPC_CONSOLE_WRITE;
    static const char banner[] =
        "Ox exited — back to shell.\nosnos:/# ";
    size_t bn = sizeof(banner) - 1;
    if (bn > IPC_DATA_SIZE) bn = IPC_DATA_SIZE;
    memcpy(sm.data, banner, bn);
    sm.arg1 = bn;
    sm.arg0 = 0xFFFFFF;       /* white */
    ipc_send(&sm);

    memset(&sm, 0, sizeof(sm));
    sm.to   = SERVER_KEYBOARD;
    sm.type = IPC_KEYBOARD_RESUME;
    ipc_send(&sm);
}

static void on_term(int sig) {
    (void)sig;
    resume_underlings();
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

static void send_event_mouse(int slot, int x, int y, int buttons,
                              int kind, int wheel) {
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
    m.data[2] = (char)(int8_t)wheel;   /* OX_MOUSE_WHEEL delta */
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
            /* Extended response: shm name packed into data[] so the
             * client can shm_open + mmap the same backing buffer and
             * draw locally. Backwards-compat: clients that ignore the
             * shm name still get the window id via arg1. */
            ipc_msg_t rsp;
            memset(&rsp, 0, sizeof(rsp));
            rsp.to   = from;
            rsp.type = IPC_OX_RESPONSE;
            rsp.arg0 = 0;
            rsp.arg1 = (uint64_t)g_wins[slot].id;
            size_t nl = strnlen(g_wins[slot].shm_name,
                                sizeof(g_wins[slot].shm_name));
            if (nl >= sizeof(rsp.data)) nl = sizeof(rsp.data) - 1;
            memcpy(rsp.data, g_wins[slot].shm_name, nl);
            rsp.data[nl] = 0;
            /* Retry on EAGAIN — see send_response notes. */
            for (int attempt = 0; attempt < 100; attempt++) {
                if (ipc_send(&rsp) == 0) break;
                if (errno != EAGAIN) break;
                struct timespec ts = { 0, 2 * 1000000 };
                nanosleep(&ts, 0);
            }
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
        /* CRITICAL: with SHM-backed windows, draws are LOCAL writes
         * (no DRAW_RECT/TEXT/IMAGE IPCs), so g_wins[slot].dirty is
         * never set. PRESENT is the ONLY signal that "client finished
         * a frame" — must always trigger a composite, no flag check.
         *
         * The legacy `if (g_wins[slot].dirty)` gate is what made
         * oxsettings appear empty until a second app opened (the
         * second app's alloc_window triggered a full repaint that
         * incidentally repainted settings with its loaded thumbs).
         * Same root cause for "mouse lag after close": the close
         * triggered a full repaint but subsequent client redraws
         * never composited until next focus change. */
        g_wins[slot].dirty = 0;
        g_dirty = 1;
        ox_window_t *w = &g_wins[slot];
        mark_dirty(w->x - SHADOW_DEPTH,
                   w->y - TITLEBAR_H - SHADOW_DEPTH,
                   w->w + 2 * SHADOW_DEPTH,
                   TITLEBAR_H + w->h + 2 * SHADOW_DEPTH + SHADOW_OFFSET);
        return;
    }
    case IPC_OX_SET_TITLE: {
        int slot = find_owner_slot(from, (int)m->arg0);
        if (slot < 0) return;
        size_t tl = strnlen(m->data, sizeof(g_wins[slot].title) - 1);
        memcpy(g_wins[slot].title, m->data, tl);
        g_wins[slot].title[tl] = 0;
        g_dirty = 1;
        ox_window_t *w = &g_wins[slot];
        mark_dirty(w->x, w->y - TITLEBAR_H, w->w, TITLEBAR_H);
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
    case IPC_OX_CLIPBOARD_SET: {
        /* arg1 = byte count; data = payload. Trunca al máximo del
         * buffer global (1023 bytes — fits en un IPC). Reemplaza el
         * clipboard global; no concat. */
        size_t n = (size_t)m->arg1;
        if (n > sizeof(g_clipboard) - 1) n = sizeof(g_clipboard) - 1;
        if (n > sizeof(m->data))         n = sizeof(m->data);
        memcpy(g_clipboard, m->data, n);
        g_clipboard[n] = 0;
        g_clip_len = (int)n;
        return;
    }
    case IPC_OX_CLIPBOARD_GET: {
        /* Reply con RESPONSE: arg1 = size, data = bytes. */
        ipc_msg_t r;
        memset(&r, 0, sizeof(r));
        r.to   = from;
        r.type = IPC_OX_RESPONSE;
        r.arg0 = 0;  /* OK */
        r.arg1 = (uint64_t)g_clip_len;
        size_t n = (size_t)g_clip_len;
        if (n > sizeof(r.data)) n = sizeof(r.data);
        memcpy(r.data, g_clipboard, n);
        /* Retry on EAGAIN — client está bloqueado esperando. */
        for (int attempt = 0; attempt < 100; attempt++) {
            if (ipc_send(&r) == 0) return;
            if (errno != EAGAIN) return;
            struct timespec ts = { 0, 2 * 1000000 };
            nanosleep(&ts, 0);
        }
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
    int cb_x = w->x + w->w - CLOSE_BTN_SIZE - CLOSE_BTN_PAD - 2;
    int cb_y = w->y - TITLEBAR_H + CLOSE_BTN_PAD;
    if (mx >= cb_x && mx < cb_x + CLOSE_BTN_SIZE &&
        my >= cb_y && my < cb_y + CLOSE_BTN_SIZE) {
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
    int old_cx = g_cx, old_cy = g_cy;
    int new_cx = g_cx + ev.dx;
    int new_cy = g_cy + ev.dy;
    if (new_cx < 0) new_cx = 0;
    if (new_cy < 0) new_cy = 0;
    if (new_cx >= (int)g_scr_w) new_cx = (int)g_scr_w - 1;
    if (new_cy >= (int)g_scr_h) new_cy = (int)g_scr_h - 1;
    int moved = (new_cx != g_cx || new_cy != g_cy);
    g_cx = new_cx;
    g_cy = new_cy;
    if (moved) {
        g_dirty = 1;   /* cursor moved → recompose */
        mark_dirty(old_cx, old_cy, CURSOR_W, CURSOR_H);
        mark_dirty(new_cx, new_cy, CURSOR_W, CURSOR_H);
        /* Hover-state propagation. Instead of full-repainting whenever
         * the cursor crosses any window titlebar or menu, mark only
         * those affected regions dirty. Keeps cursor-over-window as
         * cheap as cursor-over-wallpaper. */
        if (g_menu_visible) {
            int mh = (int)MENU_N * MENU_ITEM_H + 8;
            mark_dirty(g_menu_x, g_menu_y, MENU_W, mh);
        }
        /* Any window whose titlebar the cursor entered or left needs
         * its titlebar redrawn to update close-button hover color. */
        for (int i = 0; i < g_stack_n; i++) {
            int s = g_stack[i];
            if (!g_wins[s].used) continue;
            ox_window_t *w = &g_wins[s];
            int tb_y0 = w->y - TITLEBAR_H;
            int tb_y1 = w->y;
            int tb_x0 = w->x;
            int tb_x1 = w->x + w->w;
            int hit_old = (old_cx >= tb_x0 && old_cx < tb_x1 &&
                           old_cy >= tb_y0 && old_cy < tb_y1);
            int hit_new = (g_cx  >= tb_x0 && g_cx  < tb_x1 &&
                           g_cy  >= tb_y0 && g_cy  < tb_y1);
            if (hit_old || hit_new) {
                mark_dirty(tb_x0, tb_y0, w->w, TITLEBAR_H);
            }
        }
    }

    uint8_t old_b = g_prev_buttons;
    uint8_t new_b = ev.buttons;
    g_prev_buttons = new_b;

    /* If a window is being dragged, follow the cursor. */
    if (g_focus_slot >= 0 && g_wins[g_focus_slot].dragging) {
        if (new_b & MOUSE_BTN_LEFT) {
            ox_window_t *dw = &g_wins[g_focus_slot];
            int old_x = dw->x, old_y = dw->y;
            int new_x = g_cx - dw->drag_off_x;
            int new_y = g_cy - dw->drag_off_y;
            dw->x = new_x;
            dw->y = new_y;
            g_dirty = 1;
            /* Mark dirty UNION of old + new window bbox (with shadow
             * margin). Old position needs wallpaper restored; new
             * position needs the window drawn. This keeps a smooth
             * drag fast — instead of 12 MB memcpy per pixel of drag,
             * we touch only the affected window region. */
            int fw = dw->w + 2 * SHADOW_DEPTH;
            int fh = TITLEBAR_H + dw->h + 2 * SHADOW_DEPTH + SHADOW_OFFSET;
            mark_dirty(old_x - SHADOW_DEPTH,
                       old_y - TITLEBAR_H - SHADOW_DEPTH, fw, fh);
            mark_dirty(new_x - SHADOW_DEPTH,
                       new_y - TITLEBAR_H - SHADOW_DEPTH, fw, fh);
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
                    else if (g_menu[idx].action == 2) g_quit = 1;
                    else if (g_menu[idx].path) spawn_app(g_menu[idx].path);
                    g_dirty = 1;
                    mark_menu_dirty();
                    return;
                }
            }
            g_dirty = 1;   /* hover hilight */
            mark_menu_dirty();
            return;
        } else {
            /* Click outside menu closes it. */
            if ((new_b & MOUSE_BTN_LEFT) && !(old_b & MOUSE_BTN_LEFT)) {
                g_menu_visible = 0;
                g_dirty = 1;
                mark_menu_dirty();
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
            mark_menu_dirty();
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
                                  new_b, OX_MOUSE_DOWN, 0);
            }
        }
    }
    /* Left-button up. */
    if (!(new_b & MOUSE_BTN_LEFT) && (old_b & MOUSE_BTN_LEFT)) {
        if (slot >= 0 && !is_title) {
            send_event_mouse(slot,
                              g_cx - g_wins[slot].x,
                              g_cy - g_wins[slot].y,
                              new_b, OX_MOUSE_UP, 0);
        }
    }
    /* Wheel: forward to whichever window the cursor is over (focused
     * or not). Wheel events make sense for hover-targeted scroll. */
    if (ev.wheel != 0 && slot >= 0) {
        send_event_mouse(slot,
                          g_cx - g_wins[slot].x,
                          g_cy - g_wins[slot].y,
                          new_b, OX_MOUSE_WHEEL, ev.wheel);
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
        if (g_menu_visible) {
            mark_menu_dirty();   /* mark BEFORE clearing visibility */
            g_menu_visible = 0;
        } else {
            g_menu_x = g_cx;
            g_menu_y = g_cy;
            if (g_menu_x + MENU_W > (int)g_scr_w) g_menu_x = (int)g_scr_w - MENU_W;
            if (g_menu_y + (int)MENU_N * MENU_ITEM_H + 6 > (int)g_scr_h)
                g_menu_y = (int)g_scr_h - (int)MENU_N * MENU_ITEM_H - 6;
            g_menu_visible = 1;
            mark_menu_dirty();
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

#include "../../src/include/osnos_taskinfo.h"
extern long osnos_syscall2(long n, long a1, long a2);
#define SYS_TASKINFO_NUM 515

static void reap_dead_windows(void) {
    /* (1) Reap exited child processes (oxsrv spawns via osn_spawn).
     * Without waitpid, MAX_TASKS slots fill with ZOMBIEs and new
     * spawns eventually fail. WNOHANG keeps it non-blocking. */
    int loop_guard = 32;
    while (loop_guard-- > 0) {
        int status = 0;
        pid_t rp = waitpid(-1, &status, WNOHANG);
        if (rp <= 0) break;
    }

    /* (2) Build a one-shot snapshot of LIVE task pids — 16 syscalls
     * total. Then check each used window against the snapshot in
     * memory instead of 16-syscall-per-window. Old code did 16 ×
     * MAX_WINS = 256 syscalls per reap; this is just 16. */
    uint64_t live[16];
    int n_live = 0;
    for (int i = 0; i < 16; i++) {
        osnos_taskinfo_t info;
        if (osnos_syscall2(SYS_TASKINFO_NUM, i, (long)&info) < 0) continue;
        if (info.state == OSNOS_TASK_UNUSED ||
            info.state == OSNOS_TASK_DEAD   ||
            info.state == OSNOS_TASK_ZOMBIE) continue;
        live[n_live++] = info.pid;
    }

    for (int i = 0; i < MAX_WINS; i++) {
        if (!g_wins[i].used) continue;
        if (g_wins[i].should_close) { destroy_slot(i); continue; }
        int alive = 0;
        for (int j = 0; j < n_live; j++) {
            if (live[j] == g_wins[i].owner_pid) { alive = 1; break; }
        }
        if (!alive) destroy_slot(i);
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
    g_back_clean = (uint32_t *)malloc((size_t)g_scr_w * g_scr_h * sizeof(uint32_t));
    if (!g_back || !g_back_clean) {
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

    /* Take exclusive ownership of the framebuffer AND the keyboard
     * ring (/dev/input0). consrv stops painting, kbdsrv stops
     * draining input events — both yield to us until RESUME on exit. */
    {
        ipc_msg_t sm;
        memset(&sm, 0, sizeof(sm));
        sm.to   = SERVER_CONSOLE;
        sm.type = IPC_CONSOLE_SUSPEND;
        ipc_send(&sm);
        memset(&sm, 0, sizeof(sm));
        sm.to   = SERVER_KEYBOARD;
        sm.type = IPC_KEYBOARD_SUSPEND;
        ipc_send(&sm);
        /* Give kbdsrv a moment to actually park (it polls IPC every
         * ~30 ms in suspended state). Without this, the first few
         * keystrokes still get stolen during the handoff. */
        struct timespec ts = { 0, 50 * 1000000 };
        nanosleep(&ts, 0);
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
    long frame = 0; (void)frame;
    for (;;) {
        if (g_quit) {
            /* "Exit Ox" was selected from the menu. Wake consrv +
             * kbdsrv so the shell takes the framebuffer back. */
            resume_underlings();
            break;
        }
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
        g_mouse_events += drained;
        struct { char ascii; uint16_t keycode; } kev;
        int kdrained = 0;
        while (kdrained < 32 &&
               read(g_input_fd, &kev, sizeof(kev)) == (ssize_t)sizeof(kev)) {
            process_kbd_event((int)(unsigned char)kev.ascii, (int)kev.keycode);
            kdrained++;
        }
        g_kbd_events += kdrained;
        /* Drain the ENTIRE queue per iteration. */
        ipc_msg_t m;
        int idrained = 0;
        while (idrained < 256 && ipc_recv(&m) == 0) {
            handle_ipc(&m);
            idrained++;
        }
        g_ipc_drained_count += idrained;
        g_ipc_events += idrained;
        /* Get the current time ONCE per iteration — multiple
         * throttles (reap, move, heartbeat) share this. Avoids 3×
         * clock_gettime syscalls at every event-driven wake. */
        struct timespec _now;
        clock_gettime(0, &_now);
        uint64_t now_ms = (uint64_t)_now.tv_sec * 1000 + _now.tv_nsec / 1000000;

        /* Throttle dead-window reaping to once per ~500 ms. */
        {
            static uint64_t last_reap_ms = 0;
            if (now_ms - last_reap_ms >= 500) {
                reap_dead_windows();
                last_reap_ms = now_ms;
            }
        }
        /* Emit at most one MOVE event per frame, throttled to 60Hz. */
        static uint64_t last_move_ms = 0;
        if (g_pending_move && g_pending_move_slot >= 0 &&
            g_pending_move_slot < MAX_WINS &&
            g_wins[g_pending_move_slot].used &&
            now_ms - last_move_ms >= 16) {
            last_move_ms = now_ms;
            int slot = g_pending_move_slot;
            send_event_mouse(slot,
                              g_cx - g_wins[slot].x,
                              g_cy - g_wins[slot].y,
                              g_prev_buttons, OX_MOUSE_MOVE, 0);
            g_pending_move = 0;
            g_pending_move_slot = -1;
        }
        if (g_dirty) {
            composite_and_flush();
            g_dirty = 0;
        }
        /* Heartbeat every ~2 SECONDS (time-based, not frame-based —
         * event-driven loop fires hundreds of times per second). */
        static uint64_t last_hb_ms = 0;
        /* Heartbeat to /dev/ttyS0 — blocks ~22ms writing 250 bytes at
         * 115200 baud (UART busy-loops byte-by-byte). Spacing it to
         * 5 sec keeps the freeze visible <0.5% of the time. */
        if (ttyfd >= 0 && now_ms - last_hb_ms >= 5000) {
            last_hb_ms = now_ms;
            char hb[320];
            unsigned long mem_free_kb = 0, ipc_used = 0;
            int sfd = open("/sys/meminfo", O_RDONLY);
            if (sfd >= 0) {
                char buf[1024];
                int sn = (int)read(sfd, buf, sizeof(buf) - 1);
                close(sfd);
                if (sn > 0) {
                    buf[sn] = 0;
                    char *p;
                    if ((p = strstr(buf, "mem free  kB: ")))
                        mem_free_kb = strtoul(p + 14, 0, 10);
                    if ((p = strstr(buf, "ipc:    ")))
                        ipc_used = strtoul(p + 8, 0, 10);
                }
            }
            unsigned long avg_dirty_px = g_composite_dirty_count
                ? (unsigned long)(g_dirty_area_total /
                                  g_composite_dirty_count) : 0;
            /* Iteration rate since previous heartbeat — tells us if
             * the main loop is actually spinning fast. <60 Hz means
             * we're missing mouse events. */
            static uint64_t last_hb_iters = 0;
            uint64_t iters_delta = g_loop_iters - last_hb_iters;
            last_hb_iters = g_loop_iters;
            unsigned long iter_hz = (unsigned long)(iters_delta / 5);
            /* Per-period event rates so we can see if events are
             * stuck at the kernel/driver layer or actually arriving. */
            static uint64_t last_hb_mouse = 0, last_hb_kbd = 0,
                            last_hb_ipc = 0;
            unsigned long mhz = (unsigned long)((g_mouse_events - last_hb_mouse) / 5);
            unsigned long khz = (unsigned long)((g_kbd_events   - last_hb_kbd) / 5);
            unsigned long ihz = (unsigned long)((g_ipc_events   - last_hb_ipc) / 5);
            last_hb_mouse = g_mouse_events;
            last_hb_kbd   = g_kbd_events;
            last_hb_ipc   = g_ipc_events;
            int n = snprintf(hb, sizeof(hb),
                "oxsrv: wins=%d memfree=%lukB ipc=%lu iters=%luHz "
                "ev/s(m=%lu k=%lu i=%lu) | "
                "full=%lu(a=%lu d=%lu r=%lu rs=%lu o=%lu) "
                "dirty=%lu | t_full_ms=%lu(max=%lu) t_dirty_ms=%lu(max=%lu avg_px=%lu) "
                "t_destroy_ms=%lu(max=%lu n=%lu) drains=%lu last=%s\n",
                g_stack_n, mem_free_kb, ipc_used, iter_hz,
                mhz, khz, ihz,
                (unsigned long)g_composite_full_count,
                (unsigned long)g_full_alloc,
                (unsigned long)g_full_destroy,
                (unsigned long)g_full_raise,
                (unsigned long)g_full_reload,
                (unsigned long)g_full_other,
                (unsigned long)g_composite_dirty_count,
                (unsigned long)(g_full_us_total / 1000),
                (unsigned long)(g_full_us_max / 1000),
                (unsigned long)(g_dirty_us_total / 1000),
                (unsigned long)(g_dirty_us_max / 1000),
                avg_dirty_px,
                (unsigned long)(g_destroy_us_total / 1000),
                (unsigned long)(g_destroy_us_max / 1000),
                (unsigned long)g_destroy_count,
                (unsigned long)g_ipc_drained_count,
                g_last_full_reason ? g_last_full_reason : "-");
            if (n > 0) write(ttyfd, hb, n);
        }
        frame++;
        g_loop_iters++;
        /* Event-driven wait: block in the kernel until ANY of mouse,
         * keyboard, or IPC has data ready — no userland busy-spin.
         * Wake hooks (devfs_mouse_push, devfs_input_push,
         * ipc_send→task_unblock) flip our state immediately. The 50ms
         * timeout is a soft cap so the watchdog tick (reap windows /
         * heartbeat) still fires when truly idle. */
        struct pollfd pfds[3];
        pfds[0].fd = g_mouse_fd; pfds[0].events = POLLIN; pfds[0].revents = 0;
        pfds[1].fd = g_input_fd; pfds[1].events = POLLIN; pfds[1].revents = 0;
        pfds[2].fd = -1;         pfds[2].events = POLL_IPC_PENDING; pfds[2].revents = 0;
        poll(pfds, 3, 50);
    }
    return 0;
}
