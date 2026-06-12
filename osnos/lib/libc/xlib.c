/*
 * xlib.c — osnos tinyX: Xlib subset backed by the Ox window system.
 *
 * See lib/libc/include/X11/Xlib.h for scope. Design notes:
 *
 *  - One Display per process; XOpenDisplay calls ox_init(). Window
 *    XIDs are the Ox win ids (>0) passed through unchanged.
 *  - Every X window is created with ox_window_create_resizable so a
 *    user drag-resize arrives as ConfigureNotify (the SHM remap is
 *    handled transparently by lib/libc/ox.c).
 *  - Drawing goes straight into the window's SHM backing via ox_draw_*.
 *    A per-window dirty flag is raised by every draw call; XFlush /
 *    XSync / XNextEvent (Xlib flushes before blocking — same here)
 *    present the dirty windows.
 *  - Pixel values are 0xRRGGBB (24-bit TrueColor), so the classic
 *    BlackPixel()/WhitePixel() idiom produces correct colors.
 *  - X keycode convention: kernel keycode + 8 (matches Xorg evdev).
 *    The cooked ascii rides in XKeyEvent._osnos_ascii so XLookupString
 *    needs no keymap.
 */

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <ox.h>
#include <stdlib.h>
#include <string.h>

#define XSHIM_MAX_WINS  8
#define XSHIM_QUEUE     32

/* Pre-interned atoms. Values are arbitrary but stable non-zero ids
 * (real servers also hand out small ints); only identity matters. */
#define ATOM_WM_PROTOCOLS      68   /* matches the real predefined atom */
#define ATOM_WM_DELETE_WINDOW  200
#define ATOM_DYN_BASE          1000

typedef struct {
    Window   xid;              /* == ox win id, 0 = slot free */
    long     event_mask;
    int      w, h;
    int      dirty;            /* needs present on next flush */
    int      wants_delete;     /* XSetWMProtocols(WM_DELETE_WINDOW) */
    unsigned long background;
} xshim_win_t;

struct _XGC {
    unsigned long fg, bg;
    int           in_use;
};

struct _XDisplay {
    int          open;
    xshim_win_t  wins[XSHIM_MAX_WINS];
    XEvent       queue[XSHIM_QUEUE];   /* synthetic events (Expose, …) */
    int          q_head, q_count;
    char         dyn_atoms[16][32];    /* names for XInternAtom */
    int          n_dyn_atoms;
};

static struct _XDisplay g_dpy;          /* one display per process */
static struct _XGC g_gcs[8];

/* ---- helpers -------------------------------------------------------- */

static xshim_win_t *win_lookup(Display *dpy, Window w) {
    for (int i = 0; i < XSHIM_MAX_WINS; i++)
        if (dpy->wins[i].xid == w && w != 0) return &dpy->wins[i];
    return NULL;
}

static void q_push(Display *dpy, const XEvent *e) {
    if (dpy->q_count >= XSHIM_QUEUE) return;          /* drop oldest-first */
    int tail = (dpy->q_head + dpy->q_count) % XSHIM_QUEUE;
    dpy->queue[tail] = *e;
    dpy->q_count++;
}

static int q_pop(Display *dpy, XEvent *out) {
    if (dpy->q_count == 0) return 0;
    *out = dpy->queue[dpy->q_head];
    dpy->q_head = (dpy->q_head + 1) % XSHIM_QUEUE;
    dpy->q_count--;
    return 1;
}

static void flush_dirty(Display *dpy) {
    for (int i = 0; i < XSHIM_MAX_WINS; i++) {
        if (dpy->wins[i].xid && dpy->wins[i].dirty) {
            ox_present((ox_win_t)dpy->wins[i].xid);
            dpy->wins[i].dirty = 0;
        }
    }
}

static unsigned int buttons_to_state(int buttons) {
    unsigned int st = 0;
    if (buttons & 0x01) st |= Button1Mask;
    if (buttons & 0x02) st |= Button3Mask;   /* Ox bit1 = right */
    if (buttons & 0x04) st |= Button2Mask;   /* Ox bit2 = middle */
    return st;
}

static unsigned int mods_to_state(int mods) {
    unsigned int st = 0;
    if (mods & OX_MOD_SHIFT) st |= ShiftMask;
    if (mods & OX_MOD_CTRL)  st |= ControlMask;
    if (mods & OX_MOD_ALT)   st |= Mod1Mask;
    return st;
}

/* Translate one Ox event into an X event. Returns 1 if `out` was
 * filled, 0 if the event has no X representation (or the window is
 * not selecting it — mask filtering keeps queues lean). */
static int translate(Display *dpy, const ox_event_t *oe, XEvent *out) {
    xshim_win_t *xw = win_lookup(dpy, (Window)oe->win);
    if (!xw) return 0;
    memset(out, 0, sizeof(*out));

    switch (oe->type) {
    case OX_EV_KEY:
        if (!(xw->event_mask & KeyPressMask)) return 0;
        out->xkey.type    = KeyPress;
        out->xkey.display = dpy;
        out->xkey.window  = xw->xid;
        out->xkey.keycode = (unsigned int)oe->keycode + 8;
        out->xkey.state   = mods_to_state(oe->mods);
        out->xkey._osnos_ascii = oe->ascii;
        return 1;
    case OX_EV_MOUSE: {
        static int prev_buttons[XSHIM_MAX_WINS];
        int slot = (int)(xw - dpy->wins);
        int prev = prev_buttons[slot];
        if (oe->mouse_kind == OX_MOUSE_DOWN ||
            oe->mouse_kind == OX_MOUSE_UP ||
            oe->mouse_kind == OX_MOUSE_WHEEL) {
            int press;
            unsigned int button;
            if (oe->mouse_kind == OX_MOUSE_WHEEL) {
                /* X convention: wheel = Button4 (up) / Button5 (down),
                 * delivered as press+release; we send the press only. */
                press  = 1;
                button = oe->wheel_delta > 0 ? Button4 : Button5;
            } else {
                press = (oe->mouse_kind == OX_MOUSE_DOWN);
                int changed = oe->buttons ^ prev;
                if      (changed & 0x01) button = Button1;
                else if (changed & 0x04) button = Button2;
                else if (changed & 0x02) button = Button3;
                else                     button = Button1;
                prev_buttons[slot] = oe->buttons;
            }
            if (press  && !(xw->event_mask & ButtonPressMask))   return 0;
            if (!press && !(xw->event_mask & ButtonReleaseMask)) return 0;
            out->xbutton.type    = press ? ButtonPress : ButtonRelease;
            out->xbutton.display = dpy;
            out->xbutton.window  = xw->xid;
            out->xbutton.x       = oe->x;
            out->xbutton.y       = oe->y;
            out->xbutton.button  = button;
            out->xbutton.state   = buttons_to_state(prev) |
                                   mods_to_state(oe->mods);
            return 1;
        }
        prev_buttons[slot] = oe->buttons;
        if (!(xw->event_mask & (PointerMotionMask | ButtonMotionMask)))
            return 0;
        if ((xw->event_mask & ButtonMotionMask) &&
            !(xw->event_mask & PointerMotionMask) && oe->buttons == 0)
            return 0;
        out->xmotion.type    = MotionNotify;
        out->xmotion.display = dpy;
        out->xmotion.window  = xw->xid;
        out->xmotion.x       = oe->x;
        out->xmotion.y       = oe->y;
        out->xmotion.state   = buttons_to_state(oe->buttons) |
                               mods_to_state(oe->mods);
        return 1;
    }
    case OX_EV_EXPOSE:
        if (!(xw->event_mask & ExposureMask)) return 0;
        out->xexpose.type    = Expose;
        out->xexpose.display = dpy;
        out->xexpose.window  = xw->xid;
        out->xexpose.x       = oe->ex;
        out->xexpose.y       = oe->ey;
        out->xexpose.width   = oe->ew > 0 ? oe->ew : xw->w;
        out->xexpose.height  = oe->eh > 0 ? oe->eh : xw->h;
        out->xexpose.count   = 0;
        return 1;
    case OX_EV_RESIZE:
        xw->w = oe->new_w;
        xw->h = oe->new_h;
        if (!(xw->event_mask & StructureNotifyMask)) return 0;
        out->xconfigure.type    = ConfigureNotify;
        out->xconfigure.display = dpy;
        out->xconfigure.window  = xw->xid;
        out->xconfigure.event   = xw->xid;
        out->xconfigure.width   = oe->new_w;
        out->xconfigure.height  = oe->new_h;
        return 1;
    case OX_EV_CLOSE:
        if (xw->wants_delete) {
            out->xclient.type         = ClientMessage;
            out->xclient.display      = dpy;
            out->xclient.window       = xw->xid;
            out->xclient.message_type = ATOM_WM_PROTOCOLS;
            out->xclient.format       = 32;
            out->xclient.data.l[0]    = ATOM_WM_DELETE_WINDOW;
        } else {
            out->xdestroywindow.type    = DestroyNotify;
            out->xdestroywindow.display = dpy;
            out->xdestroywindow.window  = xw->xid;
            out->xdestroywindow.event   = xw->xid;
        }
        return 1;
    default:
        return 0;
    }
}

/* ---- connection ------------------------------------------------------ */

Display *XOpenDisplay(const char *display_name) {
    (void)display_name;                    /* always ":0" — local Ox */
    if (g_dpy.open) return &g_dpy;
    if (ox_init() < 0) return NULL;
    memset(&g_dpy, 0, sizeof(g_dpy));
    g_dpy.open = 1;
    return &g_dpy;
}

int XCloseDisplay(Display *dpy) {
    if (!dpy || !dpy->open) return 0;
    for (int i = 0; i < XSHIM_MAX_WINS; i++)
        if (dpy->wins[i].xid)
            ox_window_destroy((ox_win_t)dpy->wins[i].xid);
    memset(dpy, 0, sizeof(*dpy));
    return 0;
}

int XDefaultScreen(Display *dpy)        { (void)dpy; return 0; }
Window XDefaultRootWindow(Display *dpy) { (void)dpy; return 1; }
unsigned long XBlackPixel(Display *dpy, int s) { (void)dpy; (void)s; return 0x000000UL; }
unsigned long XWhitePixel(Display *dpy, int s) { (void)dpy; (void)s; return 0xFFFFFFUL; }
/* Ox doesn't expose screen geometry to clients yet; report the QEMU
 * default mode. Used only for initial window placement math. */
int XDisplayWidth (Display *dpy, int s) { (void)dpy; (void)s; return 1024; }
int XDisplayHeight(Display *dpy, int s) { (void)dpy; (void)s; return 768;  }

/* ---- windows --------------------------------------------------------- */

Window XCreateSimpleWindow(Display *dpy, Window parent,
                           int x, int y,
                           unsigned int width, unsigned int height,
                           unsigned int border_width,
                           unsigned long border,
                           unsigned long background) {
    (void)parent; (void)x; (void)y; (void)border_width; (void)border;
    if (!dpy || !dpy->open) return 0;
    int slot = -1;
    for (int i = 0; i < XSHIM_MAX_WINS; i++)
        if (dpy->wins[i].xid == 0) { slot = i; break; }
    if (slot < 0) return 0;
    ox_win_t ow = ox_window_create_resizable((int)width, (int)height,
                                             "X11");
    if (ow < 0) return 0;
    xshim_win_t *xw = &dpy->wins[slot];
    memset(xw, 0, sizeof(*xw));
    xw->xid        = (Window)ow;
    xw->w          = (int)width;
    xw->h          = (int)height;
    xw->background = background;
    ox_draw_rect(ow, 0, 0, (int)width, (int)height, (uint32_t)background);
    xw->dirty = 1;
    return xw->xid;
}

int XDestroyWindow(Display *dpy, Window w) {
    xshim_win_t *xw = win_lookup(dpy, w);
    if (!xw) return 0;
    ox_window_destroy((ox_win_t)w);
    memset(xw, 0, sizeof(*xw));
    return 1;
}

int XMapWindow(Display *dpy, Window w) {
    /* Ox windows are visible from creation; the X contract is that a
     * map produces the first Expose, so synthesize one. */
    xshim_win_t *xw = win_lookup(dpy, w);
    if (!xw) return 0;
    XEvent e;
    memset(&e, 0, sizeof(e));
    e.xexpose.type    = Expose;
    e.xexpose.display = dpy;
    e.xexpose.window  = w;
    e.xexpose.width   = xw->w;
    e.xexpose.height  = xw->h;
    q_push(dpy, &e);
    return 1;
}

int XMapRaised(Display *dpy, Window w)  { return XMapWindow(dpy, w); }
int XUnmapWindow(Display *dpy, Window w){ (void)dpy; (void)w; return 1; }

int XStoreName(Display *dpy, Window w, const char *window_name) {
    if (!win_lookup(dpy, w)) return 0;
    ox_window_set_title((ox_win_t)w, window_name ? window_name : "");
    return 1;
}

int XSelectInput(Display *dpy, Window w, long event_mask) {
    xshim_win_t *xw = win_lookup(dpy, w);
    if (!xw) return 0;
    xw->event_mask = event_mask;
    return 1;
}

int XClearWindow(Display *dpy, Window w) {
    xshim_win_t *xw = win_lookup(dpy, w);
    if (!xw) return 0;
    ox_draw_rect((ox_win_t)w, 0, 0, xw->w, xw->h,
                 (uint32_t)xw->background);
    xw->dirty = 1;
    return 1;
}

int XClearArea(Display *dpy, Window w, int x, int y,
               unsigned int width, unsigned int height, Bool exposures) {
    xshim_win_t *xw = win_lookup(dpy, w);
    if (!xw) return 0;
    if (width == 0)  width  = (unsigned int)(xw->w - x);
    if (height == 0) height = (unsigned int)(xw->h - y);
    ox_draw_rect((ox_win_t)w, x, y, (int)width, (int)height,
                 (uint32_t)xw->background);
    xw->dirty = 1;
    if (exposures && (xw->event_mask & ExposureMask)) {
        XEvent e;
        memset(&e, 0, sizeof(e));
        e.xexpose.type    = Expose;
        e.xexpose.display = dpy;
        e.xexpose.window  = w;
        e.xexpose.x = x; e.xexpose.y = y;
        e.xexpose.width = (int)width; e.xexpose.height = (int)height;
        q_push(dpy, &e);
    }
    return 1;
}

/* ---- GC + drawing ----------------------------------------------------- */

GC XCreateGC(Display *dpy, Drawable d,
             unsigned long valuemask, XGCValues *values) {
    (void)dpy; (void)d;
    for (int i = 0; i < (int)(sizeof(g_gcs)/sizeof(g_gcs[0])); i++) {
        if (!g_gcs[i].in_use) {
            g_gcs[i].in_use = 1;
            g_gcs[i].fg = (valuemask & GCForeground) && values
                              ? values->foreground : 0x000000UL;
            g_gcs[i].bg = (valuemask & GCBackground) && values
                              ? values->background : 0xFFFFFFUL;
            return &g_gcs[i];
        }
    }
    return NULL;
}

int XFreeGC(Display *dpy, GC gc) {
    (void)dpy;
    if (gc) gc->in_use = 0;
    return 1;
}

int XSetForeground(Display *dpy, GC gc, unsigned long fg) {
    (void)dpy;
    if (gc) gc->fg = fg;
    return 1;
}

int XSetBackground(Display *dpy, GC gc, unsigned long bg) {
    (void)dpy;
    if (gc) gc->bg = bg;
    return 1;
}

static void mark_dirty(Display *dpy, Drawable d) {
    xshim_win_t *xw = win_lookup(dpy, (Window)d);
    if (xw) xw->dirty = 1;
}

int XFillRectangle(Display *dpy, Drawable d, GC gc,
                   int x, int y, unsigned int width, unsigned int height) {
    if (!gc) return 0;
    ox_draw_rect((ox_win_t)d, x, y, (int)width, (int)height,
                 (uint32_t)gc->fg);
    mark_dirty(dpy, d);
    return 1;
}

int XDrawRectangle(Display *dpy, Drawable d, GC gc,
                   int x, int y, unsigned int width, unsigned int height) {
    if (!gc) return 0;
    uint32_t c = (uint32_t)gc->fg;
    ox_draw_rect((ox_win_t)d, x, y, (int)width + 1, 1, c);
    ox_draw_rect((ox_win_t)d, x, y + (int)height, (int)width + 1, 1, c);
    ox_draw_rect((ox_win_t)d, x, y, 1, (int)height + 1, c);
    ox_draw_rect((ox_win_t)d, x + (int)width, y, 1, (int)height + 1, c);
    mark_dirty(dpy, d);
    return 1;
}

int XDrawLine(Display *dpy, Drawable d, GC gc,
              int x1, int y1, int x2, int y2) {
    if (!gc) return 0;
    uint32_t c = (uint32_t)gc->fg;
    if (y1 == y2) {           /* horizontal fast path */
        int lo = x1 < x2 ? x1 : x2;
        int n  = (x1 < x2 ? x2 - x1 : x1 - x2) + 1;
        ox_draw_rect((ox_win_t)d, lo, y1, n, 1, c);
    } else if (x1 == x2) {    /* vertical fast path */
        int lo = y1 < y2 ? y1 : y2;
        int n  = (y1 < y2 ? y2 - y1 : y1 - y2) + 1;
        ox_draw_rect((ox_win_t)d, x1, lo, 1, n, c);
    } else {                  /* Bresenham via 1x1 rects */
        int dx = x2 > x1 ? x2 - x1 : x1 - x2;
        int dy = y2 > y1 ? y2 - y1 : y1 - y2;
        int sx = x1 < x2 ? 1 : -1;
        int sy = y1 < y2 ? 1 : -1;
        int err = dx - dy;
        int x = x1, y = y1;
        for (;;) {
            ox_draw_rect((ox_win_t)d, x, y, 1, 1, c);
            if (x == x2 && y == y2) break;
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x += sx; }
            if (e2 <  dx) { err += dx; y += sy; }
        }
    }
    mark_dirty(dpy, d);
    return 1;
}

int XDrawPoint(Display *dpy, Drawable d, GC gc, int x, int y) {
    if (!gc) return 0;
    ox_draw_rect((ox_win_t)d, x, y, 1, 1, (uint32_t)gc->fg);
    mark_dirty(dpy, d);
    return 1;
}

int XDrawString(Display *dpy, Drawable d, GC gc,
                int x, int y, const char *string, int length) {
    if (!gc || !string) return 0;
    char buf[256];
    if (length > (int)sizeof(buf) - 1) length = (int)sizeof(buf) - 1;
    if (length < 0) length = 0;
    memcpy(buf, string, (size_t)length);
    buf[length] = 0;
    /* X text origin is the BASELINE; the Ox 8x8 font draws from the
     * glyph top, so shift up by the glyph height. */
    ox_draw_text((ox_win_t)d, x, y - 8, buf, (uint32_t)gc->fg);
    mark_dirty(dpy, d);
    return 1;
}

/* ---- event queue ------------------------------------------------------ */

int XFlush(Display *dpy) {
    if (!dpy || !dpy->open) return 0;
    flush_dirty(dpy);
    return 1;
}

int XSync(Display *dpy, Bool discard) {
    XFlush(dpy);
    if (discard) {
        dpy->q_count = 0;
        ox_event_t oe;
        while (ox_poll_event(&oe)) { /* drain */ }
    }
    return 1;
}

int XPending(Display *dpy) {
    if (!dpy || !dpy->open) return 0;
    /* Pull everything the server already sent into our queue. */
    ox_event_t oe;
    while (dpy->q_count < XSHIM_QUEUE && ox_poll_event(&oe)) {
        XEvent e;
        if (translate(dpy, &oe, &e)) q_push(dpy, &e);
    }
    return dpy->q_count;
}

int XNextEvent(Display *dpy, XEvent *out) {
    if (!dpy || !dpy->open) return 1;
    flush_dirty(dpy);              /* Xlib flushes before blocking */
    for (;;) {
        if (q_pop(dpy, out)) return 0;
        ox_event_t oe;
        if (!ox_wait_event(&oe)) continue;
        XEvent e;
        if (translate(dpy, &oe, &e)) { *out = e; return 0; }
    }
}

/* ---- arcs --------------------------------------------------------------- */

/* X angles are 1/64 deg, CCW from 3 o'clock. We rasterize the full
 * ellipse with the standard (dx/rx)^2 + (dy/ry)^2 test and clip each
 * pixel to the angular range — exact for the full-circle case every
 * xeyes-class client uses, sane for partial arcs. No libm needed:
 * the angle test works on cross/dot products of the pixel vector
 * against the range's edge vectors. */

static void arc_edge_vec(int a64, long *vx, long *vy) {
    /* Tiny 0..90 deg sine table (x1000), 1-degree steps. */
    static const short sin_t[91] = {
          0,  17,  35,  52,  70,  87, 105, 122, 139, 156,
        174, 191, 208, 225, 242, 259, 276, 292, 309, 326,
        342, 358, 375, 391, 407, 423, 438, 454, 469, 485,
        500, 515, 530, 545, 559, 574, 588, 602, 616, 629,
        643, 656, 669, 682, 695, 707, 719, 731, 743, 755,
        766, 777, 788, 799, 809, 819, 829, 839, 848, 857,
        866, 875, 883, 891, 899, 906, 914, 921, 927, 934,
        940, 946, 951, 956, 961, 966, 970, 974, 978, 982,
        985, 988, 990, 993, 995, 996, 998, 999, 999, 1000,
       1000 };
    int deg = (a64 / 64) % 360;
    if (deg < 0) deg += 360;
    int q = deg / 90, r = deg % 90;
    long s, c;
    s = sin_t[r]; c = sin_t[90 - r];
    switch (q) {
    case 0:  *vx =  c; *vy =  s; break;
    case 1:  *vx = -s; *vy =  c; break;
    case 2:  *vx = -c; *vy = -s; break;
    default: *vx =  s; *vy = -c; break;
    }
}

static void arc_render(Display *dpy, Drawable d, GC gc,
                       int x, int y, unsigned int width,
                       unsigned int height, int angle1, int angle2,
                       int filled) {
    if (!gc || width == 0 || height == 0) return;
    uint32_t c = (uint32_t)gc->fg;
    long rx2 = (long)width * width;    /* (2rx)^2 actually — see below */
    long ry2 = (long)height * height;
    int full = angle2 >= 360 * 64 || angle2 <= -360 * 64;
    long e1x = 0, e1y = 0, e2x = 0, e2y = 0;
    if (!full) {
        arc_edge_vec(angle1, &e1x, &e1y);
        arc_edge_vec(angle1 + angle2, &e2x, &e2y);
    }
    /* Center in half-pixel units to keep integer math exact. */
    long cx2 = 2 * x + (long)width;    /* 2*center_x */
    long cy2 = 2 * y + (long)height;
    for (unsigned int row = 0; row <= height; row++) {
        for (unsigned int col = 0; col <= width; col++) {
            long dx2 = (2 * ((long)x + col) + 1) - cx2;  /* 2*dx */
            long dy2 = cy2 - (2 * ((long)y + row) + 1);  /* 2*dy, Y up */
            /* Inside test: (dx/rx)^2 + (dy/ry)^2 <= 1, scaled. */
            long lhs = dx2 * dx2 * ry2 + dy2 * dy2 * rx2;
            long rhs = rx2 * ry2;
            if (filled) {
                if (lhs > rhs) continue;
            } else {
                /* Ring: between (r-1) and r, approximated. */
                long inner = (long)(width > 2 ? width - 2 : 0) *
                             (width > 2 ? width - 2 : 0) *
                             (long)(height > 2 ? height - 2 : 0) *
                             (height > 2 ? height - 2 : 0);
                long lhs_in = 0;
                if (width > 2 && height > 2) {
                    long irx2 = (long)(width - 2) * (width - 2);
                    long iry2 = (long)(height - 2) * (height - 2);
                    lhs_in = dx2 * dx2 * iry2 + dy2 * dy2 * irx2;
                    inner  = irx2 * iry2;
                }
                if (lhs > rhs) continue;
                if (width > 2 && height > 2 && lhs_in <= inner) continue;
            }
            if (!full) {
                /* Pixel angle within [a1, a1+a2]? Sweep via sign of
                 * cross products against the two edge vectors. */
                long cr1 = e1x * dy2 - e1y * dx2;   /* edge1 x p */
                long cr2 = e2x * dy2 - e2y * dx2;   /* edge2 x p */
                int sweep_pos = angle2 >= 0;
                int in;
                int span = angle2 >= 0 ? angle2 : -angle2;
                if (span <= 180 * 64)
                    in = sweep_pos ? (cr1 >= 0 && cr2 <= 0)
                                   : (cr1 <= 0 && cr2 >= 0);
                else
                    in = sweep_pos ? (cr1 >= 0 || cr2 <= 0)
                                   : (cr1 <= 0 || cr2 >= 0);
                if (!in) continue;
            }
            ox_draw_rect((ox_win_t)d, (int)(x + col), (int)(y + row),
                         1, 1, c);
        }
    }
    mark_dirty(dpy, d);
}

int XFillArc(Display *dpy, Drawable d, GC gc,
             int x, int y, unsigned int width, unsigned int height,
             int angle1, int angle2) {
    arc_render(dpy, d, gc, x, y, width, height, angle1, angle2, 1);
    return 1;
}

int XDrawArc(Display *dpy, Drawable d, GC gc,
             int x, int y, unsigned int width, unsigned int height,
             int angle1, int angle2) {
    arc_render(dpy, d, gc, x, y, width, height, angle1, angle2, 0);
    return 1;
}

/* ---- pointer ------------------------------------------------------------ */

Bool XQueryPointer(Display *dpy, Window w,
                   Window *root_return, Window *child_return,
                   int *root_x_return, int *root_y_return,
                   int *win_x_return, int *win_y_return,
                   unsigned int *mask_return) {
    if (root_return)  *root_return  = 1;
    if (child_return) *child_return = None;
    int gx = 0, gy = 0, wx0 = 0, wy0 = 0, btn = 0;
    if (!dpy || !dpy->open ||
        ox_query_pointer((ox_win_t)w, &gx, &gy, &wx0, &wy0, &btn) < 0) {
        if (root_x_return) *root_x_return = 0;
        if (root_y_return) *root_y_return = 0;
        if (win_x_return)  *win_x_return  = 0;
        if (win_y_return)  *win_y_return  = 0;
        if (mask_return)   *mask_return   = 0;
        return False;
    }
    if (root_x_return) *root_x_return = gx;
    if (root_y_return) *root_y_return = gy;
    if (win_x_return)  *win_x_return  = gx - wx0;
    if (win_y_return)  *win_y_return  = gy - wy0;
    if (mask_return)   *mask_return   = buttons_to_state(btn);
    return True;
}

/* ---- atoms + WM protocols ---------------------------------------------- */

Atom XInternAtom(Display *dpy, const char *atom_name, Bool only_if_exists) {
    if (!atom_name) return None;
    if (strcmp(atom_name, "WM_PROTOCOLS") == 0)     return ATOM_WM_PROTOCOLS;
    if (strcmp(atom_name, "WM_DELETE_WINDOW") == 0) return ATOM_WM_DELETE_WINDOW;
    for (int i = 0; i < dpy->n_dyn_atoms; i++)
        if (strcmp(dpy->dyn_atoms[i], atom_name) == 0)
            return (Atom)(ATOM_DYN_BASE + i);
    if (only_if_exists) return None;
    if (dpy->n_dyn_atoms >= (int)(sizeof(dpy->dyn_atoms) /
                                  sizeof(dpy->dyn_atoms[0])))
        return None;
    size_t L = strlen(atom_name);
    if (L >= sizeof(dpy->dyn_atoms[0])) L = sizeof(dpy->dyn_atoms[0]) - 1;
    memcpy(dpy->dyn_atoms[dpy->n_dyn_atoms], atom_name, L);
    dpy->dyn_atoms[dpy->n_dyn_atoms][L] = 0;
    return (Atom)(ATOM_DYN_BASE + dpy->n_dyn_atoms++);
}

Status XSetWMProtocols(Display *dpy, Window w, Atom *protocols, int count) {
    xshim_win_t *xw = win_lookup(dpy, w);
    if (!xw) return 0;
    for (int i = 0; i < count; i++)
        if (protocols[i] == ATOM_WM_DELETE_WINDOW)
            xw->wants_delete = 1;
    return 1;
}

/* ---- keyboard ----------------------------------------------------------- */

int XLookupString(XKeyEvent *ev, char *buffer_return, int bytes_buffer,
                  KeySym *keysym_return, XComposeStatus *status_in_out) {
    (void)status_in_out;
    if (!ev) return 0;
    int kernel_kc = (int)ev->keycode - 8;
    KeySym ks = NoSymbol;
    switch (kernel_kc) {
    case OX_KEY_ESC:       ks = XK_Escape;    break;
    case OX_KEY_ENTER:     ks = XK_Return;    break;
    case OX_KEY_BACKSPACE: ks = XK_BackSpace; break;
    case OX_KEY_TAB:       ks = XK_Tab;       break;
    case OX_KEY_DELETE:    ks = XK_Delete;    break;
    case OX_KEY_UP:        ks = XK_Up;        break;
    case OX_KEY_DOWN:      ks = XK_Down;      break;
    case OX_KEY_LEFT:      ks = XK_Left;      break;
    case OX_KEY_RIGHT:     ks = XK_Right;     break;
    case OX_KEY_HOME:      ks = XK_Home;      break;
    case OX_KEY_END:       ks = XK_End;       break;
    case OX_KEY_PGUP:      ks = XK_Page_Up;   break;
    case OX_KEY_PGDN:      ks = XK_Page_Down; break;
    default:
        if (ev->_osnos_ascii >= 0x20 && ev->_osnos_ascii < 0x7f)
            ks = (KeySym)ev->_osnos_ascii;   /* Latin-1 == ASCII */
        break;
    }
    if (keysym_return) *keysym_return = ks;
    int n = 0;
    if (ev->_osnos_ascii > 0 && bytes_buffer > 0 && buffer_return) {
        buffer_return[0] = (char)ev->_osnos_ascii;
        if (bytes_buffer > 1) buffer_return[1] = 0;
        n = 1;
    }
    return n;
}
