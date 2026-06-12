/*
 * /bin/xeyes — port of the classic X11 xeyes to osnos tinyX
 * (FASE 15.3).
 *
 * This is a PORT, not a rewrite: the eye geometry constants, the
 * pupil-tracking math (computePupil) and the eye rendering
 * (eyeLiner / eyeBall, including the abstract eye-space → pixel
 * transform) are taken from the original xeyes sources (Eyes.c /
 * Eyes.h / transform.c, X Consortium, MIT/X11 license — written by
 * Keith Packard, MIT X Consortium). What's replaced is the shell:
 * the original wraps the eyes in an Xt/Xaw widget; osnos has no Xt
 * yet, so the widget lifecycle becomes a plain Xlib main loop (the
 * same calls the Xt internals issue), polling the pointer with
 * XQueryPointer on a timer exactly like the original's
 * delay-driven tick.
 *
 * Everything goes through <X11/Xlib.h> — the osnos tinyX shim — so
 * this file also builds unmodified on a Linux box with
 * `cc xeyes.c -lX11 -lm`.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---- Geometry constants — verbatim from xeyes Eyes.h ------------- */

#define NUM_EYES    2
#define EYE_X(n)    ((n) * 2.0)
#define EYE_Y(n)    (0.0)
#define EYE_OFFSET  (0.1)     /* padding between eyes              */
#define EYE_THICK   (0.175)   /* thickness of eye rim              */
#define BALL_WIDTH  (0.3)
#define BALL_PAD    (0.05)
#define EYE_WIDTH   (2.0 - (EYE_THICK + EYE_OFFSET) * 2)
#define EYE_HEIGHT  EYE_WIDTH
#define EYE_HWIDTH  (EYE_WIDTH / 2.0)
#define EYE_HHEIGHT (EYE_HEIGHT / 2.0)
#define BALL_DIST   ((EYE_WIDTH - BALL_WIDTH) / 2.0 - BALL_PAD)
#define W_MIN_X     (-1.0 + EYE_OFFSET)
#define W_MAX_X     (3.0 - EYE_OFFSET)
#define W_MIN_Y     (-1.0 + EYE_OFFSET)
#define W_MAX_Y     (1.0 - EYE_OFFSET)

typedef struct { double x, y; } TPoint;

/* ---- Transform: abstract eye space → window pixels ----------------
 * Equivalent of the original transform.c Transform record +
 * SetTransform/Xx/Xy/Xwidth/Xheight macros: a linear map from the
 * rectangle [W_MIN_X..W_MAX_X]x[W_MIN_Y..W_MAX_Y] onto the window,
 * Y flipped (X11 y grows down, eye space y grows up). */
typedef struct {
    double mx, bx;   /* x' = mx * x + bx */
    double my, by;
} Transform;

static Transform g_t;

static void SetTransform(Transform *t, int xx1, int xx2, int xy1, int xy2,
                         double tx1, double tx2, double ty1, double ty2) {
    t->mx = ((double)xx2 - xx1) / (tx2 - tx1);
    t->bx = (double)xx1 - t->mx * tx1;
    t->my = ((double)xy2 - xy1) / (ty2 - ty1);
    t->by = (double)xy1 - t->my * ty1;
}

static int Xx(double x, double y, const Transform *t) {
    (void)y;
    return (int)(t->mx * x + t->bx + 0.5);
}
static int Xy(double x, double y, const Transform *t) {
    (void)x;
    return (int)(t->my * y + t->by + 0.5);
}
static int Xwidth(double w, double h, const Transform *t) {
    (void)h;
    return (int)(t->mx * w + 0.5);
}
static int Xheight(double w, double h, const Transform *t) {
    (void)w;
    return (int)(-t->my * h + 0.5);   /* my is negative (Y flip) */
}

/* TFillArc — transform.c's helper: an arc spec in eye space, drawn
 * in pixel space. */
static void TFillArc(Display *dpy, Drawable win, GC gc,
                     const Transform *t,
                     double x, double y, double width, double height,
                     int angle1, int angle2) {
    int xx = Xx(x, y + height, t);
    int xy = Xy(x, y + height, t);
    int xw = Xwidth(width, height, t);
    int xh = Xheight(width, height, t);
    if (xw < 0) { xx += xw; xw = -xw; }
    if (xh < 0) { xy += xh; xh = -xh; }
    XFillArc(dpy, win, gc, xx, xy, (unsigned int)xw, (unsigned int)xh,
             angle1, angle2);
}

/* ---- Pupil math — verbatim logic from xeyes Eyes.c ---------------- */

static double local_hypot(double a, double b) {
    return sqrt(a * a + b * b);
}

static TPoint computePupil(int num, TPoint mouse) {
    double cx, cy;
    double dist;
    double angle;
    double dx, dy;
    double cosa, sina;
    TPoint ret;

    cx = EYE_X(num); dx = mouse.x - cx;
    cy = EYE_Y(num); dy = mouse.y - cy;
    if (!(dx == 0 && dy == 0)) {
        angle = atan2(dy, dx);
        cosa = cos(angle);
        sina = sin(angle);
        dist = BALL_DIST * local_hypot(EYE_HWIDTH * cosa,
                                       EYE_HHEIGHT * sina);
        if (dist > local_hypot(dx, dy))
            dist = local_hypot(dx, dy);
        cx += dist * cosa;
        cy += dist * sina;
    }
    ret.x = cx;
    ret.y = cy;
    return ret;
}

/* ---- Eye rendering — structure of Eyes.c eyeLiner / eyeBall ------- */

static unsigned long g_px_outline; /* black  */
static unsigned long g_px_white;   /* white  */
static unsigned long g_px_pupil;   /* black  */
static unsigned long g_px_bg;      /* window background */

static void eyeLiner(Display *dpy, Window win, GC gc, int num) {
    XSetForeground(dpy, gc, g_px_outline);
    TFillArc(dpy, win, gc, &g_t,
             EYE_X(num) - 1.0, EYE_Y(num) - 1.0, 2.0, 2.0,
             0, 360 * 64);
    XSetForeground(dpy, gc, g_px_white);
    TFillArc(dpy, win, gc, &g_t,
             EYE_X(num) - (1.0 - EYE_THICK),
             EYE_Y(num) - (1.0 - EYE_THICK),
             2.0 - 2.0 * EYE_THICK, 2.0 - 2.0 * EYE_THICK,
             0, 360 * 64);
}

static void eyeBall(Display *dpy, Window win, GC gc, int num,
                    TPoint pupil, TPoint prev) {
    (void)num;
    /* Erase old ball (white over previous), draw new (pupil color) —
     * same incremental strategy as Eyes.c. */
    XSetForeground(dpy, gc, g_px_white);
    TFillArc(dpy, win, gc, &g_t,
             prev.x - BALL_WIDTH / 2.0, prev.y - BALL_WIDTH / 2.0,
             BALL_WIDTH, BALL_WIDTH, 0, 360 * 64);
    XSetForeground(dpy, gc, g_px_pupil);
    TFillArc(dpy, win, gc, &g_t,
             pupil.x - BALL_WIDTH / 2.0, pupil.y - BALL_WIDTH / 2.0,
             BALL_WIDTH, BALL_WIDTH, 0, 360 * 64);
}

/* ---- Main loop (replaces the Xt shell) ----------------------------- */

static int g_w = 300, g_h = 200;

static void recompute_transform(void) {
    SetTransform(&g_t, 0, g_w, g_h, 0,
                 W_MIN_X, W_MAX_X, W_MIN_Y, W_MAX_Y);
}

int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "xeyes: cannot open display\n");
        return 1;
    }
    int screen = DefaultScreen(dpy);
    g_px_outline = BlackPixel(dpy, screen);
    g_px_white   = WhitePixel(dpy, screen);
    g_px_pupil   = BlackPixel(dpy, screen);
    g_px_bg      = 0xD6D6D6;   /* classic gray widget background */

    Window win = XCreateSimpleWindow(
        dpy, DefaultRootWindow(dpy), 80, 80,
        (unsigned int)g_w, (unsigned int)g_h, 1,
        g_px_outline, g_px_bg);
    if (!win) return 1;
    XStoreName(dpy, win, "xeyes");
    XSelectInput(dpy, win,
                 ExposureMask | KeyPressMask | StructureNotifyMask);
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    XMapWindow(dpy, win);
    GC gc = XCreateGC(dpy, win, 0, NULL);

    recompute_transform();

    TPoint pupils[NUM_EYES];
    TPoint mouse = { -1000, -1000 };
    int need_full = 1;
    for (int n = 0; n < NUM_EYES; n++)
        pupils[n] = computePupil(n, mouse);

    int running = 1;
    while (running) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) need_full = 1;
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width  != g_w ||
                    ev.xconfigure.height != g_h) {
                    g_w = ev.xconfigure.width;
                    g_h = ev.xconfigure.height;
                    recompute_transform();
                    need_full = 1;
                }
                break;
            case KeyPress: {
                char buf[8];
                KeySym ks = NoSymbol;
                XLookupString(&ev.xkey, buf, sizeof(buf), &ks, NULL);
                if (ks == XK_Escape || ks == XK_q) running = 0;
                break;
            }
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wm_delete) running = 0;
                break;
            case DestroyNotify:
                running = 0;
                break;
            }
        }

        /* Track the pointer — the original polls via an Xt timer at
         * ~10 Hz and converts root coords into eye space through the
         * inverse transform; identical math here. */
        Window rr, cr;
        int rx, ry, wx, wy;
        unsigned int mask;
        XQueryPointer(dpy, win, &rr, &cr, &rx, &ry, &wx, &wy, &mask);
        TPoint m2;
        m2.x = ((double)wx - g_t.bx) / g_t.mx;
        m2.y = ((double)wy - g_t.by) / g_t.my;

        int moved = (m2.x != mouse.x || m2.y != mouse.y);
        if (need_full) {
            XSetForeground(dpy, gc, g_px_bg);
            XFillRectangle(dpy, win, gc, 0, 0,
                           (unsigned int)g_w, (unsigned int)g_h);
            for (int n = 0; n < NUM_EYES; n++) {
                eyeLiner(dpy, win, gc, n);
                pupils[n] = computePupil(n, m2);
                eyeBall(dpy, win, gc, n, pupils[n], pupils[n]);
            }
            mouse = m2;
            need_full = 0;
            XFlush(dpy);
        } else if (moved) {
            for (int n = 0; n < NUM_EYES; n++) {
                TPoint np = computePupil(n, m2);
                if (np.x != pupils[n].x || np.y != pupils[n].y) {
                    eyeBall(dpy, win, gc, n, np, pupils[n]);
                    pupils[n] = np;
                }
            }
            mouse = m2;
            XFlush(dpy);
        }

        usleep(100 * 1000);   /* 10 Hz pointer poll, like the original */
    }

    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
