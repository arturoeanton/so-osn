#pragma once

/*
 * Ox — osnos mini-X window-system client library.
 *
 * GUI apps link against this to talk to /bin/oxsrv (SERVER_OX=5)
 * over the kernel IPC queue (opcodes 0x60-0x7F). The protocol is
 * thin and synchronous-ish: every draw request is one IPC msg, and
 * the server batches damage and flushes on ox_present(). Eventos
 * (key / mouse / expose / close) llegan vía ox_poll_event /
 * ox_wait_event.
 *
 * Forward path to tinyX / X11: the API shape mirrors a tiny Xlib
 * subset (XCreateWindow, XDrawRectangle, XNextEvent, ...). When
 * tinyX lands, oxlib stays as a fallback while clients migrate.
 */

#include <stdint.h>
#include <stddef.h>

typedef int ox_win_t;

/* Color helper — pack BGRA bytes the way the framebuffer expects.
 * Alpha is currently ignored (no compositing) but we keep the slot
 * so future blend ops don't break ABI. */
#define OX_RGB(r, g, b)        (((uint32_t)(r) << 16) | \
                                 ((uint32_t)(g) << 8)  | \
                                  (uint32_t)(b))
#define OX_RGBA(r, g, b, a)    (((uint32_t)(a) << 24) | OX_RGB((r),(g),(b)))

/* Event types. */
#define OX_EV_NONE   0
#define OX_EV_KEY    1
#define OX_EV_MOUSE  2
#define OX_EV_EXPOSE 3
#define OX_EV_CLOSE  4
#define OX_EV_RESIZE 5    /* window size changed — re-render at new w,h */

/* Mouse event sub-type. */
#define OX_MOUSE_MOVE  0
#define OX_MOUSE_DOWN  1
#define OX_MOUSE_UP    2
#define OX_MOUSE_WHEEL 3   /* ev.wheel_delta: +1 up, -1 down */

/* Modifier bits. */
#define OX_MOD_SHIFT 0x01
#define OX_MOD_CTRL  0x02
#define OX_MOD_ALT   0x04

/* Special keycodes (a superset of <osnos_keys.h>; tinyX consumers
 * should look at OX_KEY_* before falling back to ascii). */
#define OX_KEY_UP    103
#define OX_KEY_DOWN  108
#define OX_KEY_LEFT  105
#define OX_KEY_RIGHT 106
#define OX_KEY_HOME  102
#define OX_KEY_END   107
#define OX_KEY_PGUP  104
#define OX_KEY_PGDN  109
#define OX_KEY_DELETE 111
#define OX_KEY_F1    59
#define OX_KEY_F4    62
#define OX_KEY_F12   88
#define OX_KEY_TAB   15
#define OX_KEY_ESC   1
#define OX_KEY_ENTER 28
#define OX_KEY_BACKSPACE 14

typedef struct {
    int      type;        /* OX_EV_*                                  */
    ox_win_t win;
    /* KEY:                                                           */
    int      ascii;       /* 0 if non-printable                       */
    int      keycode;     /* OX_KEY_*                                 */
    int      mods;        /* OX_MOD_* bitmask                         */
    /* MOUSE:                                                         */
    int      x, y;        /* window-local                             */
    int      buttons;     /* bit0 left, bit1 right, bit2 middle       */
    int      mouse_kind;  /* OX_MOUSE_MOVE / DOWN / UP / WHEEL        */
    int      wheel_delta; /* +1 up, -1 down (only OX_MOUSE_WHEEL)     */
    /* EXPOSE:                                                        */
    int      ex, ey, ew, eh;
    /* RESIZE: new dimensions (also valid on EXPOSE after resize). */
    int      new_w, new_h;
} ox_event_t;

/* Connect to the running oxsrv. Returns 0 on success, -1 on error
 * (errno set to ENOENT if SERVER_OX is unregistered). */
int  ox_init(void);

/* Create a window of (w, h) pixels with the given title. Returns the
 * server-assigned win_id (>0) or -1 on error. The server places the
 * window at a default position; resize/move not yet supported. */
ox_win_t ox_window_create(int w, int h, const char *title);

/* Like ox_window_create but tells the server "I handle OX_EV_RESIZE
 * properly — when the user zooms/unzooms, allocate a new SHM at the
 * new dims and notify me via OX_EV_RESIZE instead of doing the legacy
 * 2x scaled blit". For apps that don't opt in, the server keeps the
 * old behaviour (visual zoom that magnifies pixels). Used by oxterm
 * and any other app that wants more rows/cols at the same font size
 * when maximized. */
ox_win_t ox_window_create_resizable(int w, int h, const char *title);

void ox_window_destroy(ox_win_t win);
void ox_window_set_title(ox_win_t win, const char *title);

/* Current window dimensions. Updated by ox_poll_event / ox_wait_event
 * when an OX_EV_RESIZE arrives (the libc side handles the SHM remap
 * transparently; apps just need to re-render at the new w/h). Either
 * pointer may be NULL. Returns 0 on success, -1 if the window id is
 * unknown. */
int  ox_window_dims(ox_win_t win, int *out_w, int *out_h);

/* Drawing primitives — all act on the window's backing buffer.
 * The result becomes visible after ox_present(). */
void ox_draw_rect (ox_win_t win, int x, int y, int w, int h,
                    uint32_t color);
void ox_draw_text (ox_win_t win, int x, int y, const char *s,
                    uint32_t color);
void ox_draw_image(ox_win_t win, int x, int y, int w, int h,
                    const uint32_t *bgra, int src_pitch_px);

/* Flush the window's backing buffer to the screen. Cheap if nothing
 * changed since the last present. */
void ox_present(ox_win_t win);

/* Non-blocking event pop. Returns 1 if an event was filled in, 0
 * otherwise. */
int  ox_poll_event(ox_event_t *out);

/* Blocking event pop. Sleeps via nanosleep until something arrives. */
int  ox_wait_event(ox_event_t *out);

/* System clipboard — shared across all Ox apps via oxsrv. Cap is
 * ~1000 bytes; SET truncates silently if longer. GET fills `buf` with
 * up to `cap` bytes (NUL-terminated) and returns the byte count
 * written (excluding NUL), or 0 on empty/error. */
int  ox_clipboard_set(const char *bytes, int len);
int  ox_clipboard_get(char *buf, int cap);

/* Internal — exported so oxsrv can render text using the same font
 * the client compiled against (so glyph metrics line up). */
const uint8_t *ox_font_glyph(int c);     /* 8 bytes, 8x8 bitmap */

/* ---- Proportional text rendering (Haiku/BeOS look) ------------------
 *
 * Optional TTF backend: callers ox_text_init() with a TTF path at
 * startup. If load succeeds, subsequent ox_text_draw / ox_text_width /
 * ox_text_height use proportional anti-aliased glyphs. If load fails
 * (file missing, malformed), the calls transparently fall back to the
 * 8x8 bitmap font — so apps stay readable even without a font.
 *
 * Recommended default: ox_text_init("/home/.fonts/default.ttf", 12);
 *
 * The Ox WM server (oxsrv) calls this once at boot; client apps may
 * call it themselves to render text inside their own buffers with the
 * same look.
 */
int  ox_text_init(const char *ttf_path, int pixel_height);
int  ox_text_loaded(void);
int  ox_text_height(void);                   /* px from top to bottom of glyphs */
int  ox_text_line_height(void);              /* px between baselines */
int  ox_text_width(const char *s);           /* width of single-line string */
void ox_text_draw(uint32_t *buf, int bw, int bh,
                   int x, int y, const char *s, uint32_t color);

/* ---- Icon system (BeOS-style 16x16 mono icons) ---------------------
 *
 * Built-in icon registry of small monochrome bitmaps indexed by app
 * name ("notepad", "browser", "sqlite", etc.). Each icon is a
 * 16-row mask (uint16_t per row, bit 15 = leftmost pixel).
 *
 * Apps and oxsrv use these for window decorations, menu items, and
 * deskbar tiles. ox_icon_get returns NULL for an unknown name (caller
 * should draw a fallback). ox_icon_draw paints the icon with the given
 * foreground color where mask bits are set; background is transparent.
 */
#define OX_ICON_W 16
#define OX_ICON_H 16
const uint16_t *ox_icon_get(const char *name);
void  ox_icon_draw(uint32_t *buf, int bw, int bh, int x, int y,
                    const uint16_t *icon, uint32_t color);

/* Color icon API — 24x24 RGBA bitmaps loaded from /home/.icons/.
 * Apps and oxsrv prefer this; the mono path above is a fallback when
 * no .rgba file is staged on disk for the requested key. */
const uint8_t *ox_icon_get_rgba(const char *name);
int   ox_icon_dims(int *w, int *h);
void  ox_icon_draw_rgba(uint32_t *buf, int bw, int bh, int x, int y,
                         const uint8_t *rgba);

/* Diagnostic log — writes printf-style text to /dev/ttyS0 (which
 * QEMU captures into serial.log). Use this from Ox apps because
 * stderr is broken for children of oxsrv. Always available; the
 * underlying open() of /dev/ttyS0 is lazy + per-process.
 *
 * Convention: every line should start with "<app>: " so multi-app
 * traces in serial.log stay readable. */
void ox_log(const char *fmt, ...);
