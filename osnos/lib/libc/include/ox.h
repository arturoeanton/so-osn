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

/* Mouse event sub-type. */
#define OX_MOUSE_MOVE 0
#define OX_MOUSE_DOWN 1
#define OX_MOUSE_UP   2

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
    int      mouse_kind;  /* OX_MOUSE_MOVE / DOWN / UP                */
    /* EXPOSE:                                                        */
    int      ex, ey, ew, eh;
} ox_event_t;

/* Connect to the running oxsrv. Returns 0 on success, -1 on error
 * (errno set to ENOENT if SERVER_OX is unregistered). */
int  ox_init(void);

/* Create a window of (w, h) pixels with the given title. Returns the
 * server-assigned win_id (>0) or -1 on error. The server places the
 * window at a default position; resize/move not yet supported. */
ox_win_t ox_window_create(int w, int h, const char *title);

void ox_window_destroy(ox_win_t win);
void ox_window_set_title(ox_win_t win, const char *title);

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

/* Internal — exported so oxsrv can render text using the same font
 * the client compiled against (so glyph metrics line up). */
const uint8_t *ox_font_glyph(int c);     /* 8 bytes, 8x8 bitmap */
