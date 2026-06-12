#pragma once
/*
 * X11/Xlib.h — osnos tinyX: a source-compatible Xlib subset backed by
 * the Ox window system (FASE 15.1, first slice of the Linux X compat
 * track).
 *
 * Scope: enough of Xlib that small, classic X programs — create a
 * window, select input, draw rects/lines/text in an event loop —
 * compile UNMODIFIED against this header and run on osnos, each X
 * window backed by a real (resizable) Ox window.
 *
 * What maps where:
 *   XOpenDisplay        → ox_init()
 *   XCreateSimpleWindow → ox_window_create_resizable()
 *   XFillRectangle/...  → ox_draw_* into the window's SHM backing
 *   XFlush / XSync      → ox_present()
 *   XNextEvent          → ox_wait_event() translated to X events
 *   ConfigureNotify     ← OX_EV_RESIZE (the SHM remap is transparent)
 *   ClientMessage(WM_DELETE_WINDOW) ← OX_EV_CLOSE
 *
 * Non-goals (today): pixmaps, real fonts (XDrawString uses the Ox
 * 8x8 bitmap font), colormaps beyond 24-bit TrueColor pixels, and
 * any form of wire protocol — this is a client-side shim, not an X
 * server. Pixel values are 0xRRGGBB, so BlackPixel()/WhitePixel()
 * behave exactly like a 24-bit TrueColor visual.
 */

#include <X11/X.h>
#include <stddef.h>

typedef int Bool;
typedef int Status;
#define True  1
#define False 0

typedef char *XPointer;

/* Opaque-ish connection object. One per XOpenDisplay. */
typedef struct _XDisplay Display;

typedef struct {
    unsigned long foreground;
    unsigned long background;
    int           function;
    int           line_width;
    Font          font;
} XGCValues;

typedef struct _XGC *GC;

/* ---- events ------------------------------------------------------- */

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window window;
} XAnyEvent;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window window;
    Window root, subwindow;
    Time time;
    int x, y;
    int x_root, y_root;
    unsigned int state;     /* ShiftMask | ControlMask | ButtonNMask */
    unsigned int keycode;   /* X keycode = kernel keycode + 8 */
    Bool same_screen;
    int _osnos_ascii;       /* shim-private: cooked ascii for XLookupString */
} XKeyEvent;
typedef XKeyEvent XKeyPressedEvent;
typedef XKeyEvent XKeyReleasedEvent;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window window;
    Window root, subwindow;
    Time time;
    int x, y;
    int x_root, y_root;
    unsigned int state;
    unsigned int button;    /* Button1..Button5 */
    Bool same_screen;
} XButtonEvent;
typedef XButtonEvent XButtonPressedEvent;
typedef XButtonEvent XButtonReleasedEvent;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window window;
    Window root, subwindow;
    Time time;
    int x, y;
    int x_root, y_root;
    unsigned int state;
    char is_hint;
    Bool same_screen;
} XMotionEvent;
typedef XMotionEvent XPointerMovedEvent;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window window;
    int x, y;
    int width, height;
    int count;              /* 0 = last expose in series */
} XExposeEvent;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window event;
    Window window;
    int x, y;
    int width, height;
    int border_width;
    Window above;
    Bool override_redirect;
} XConfigureEvent;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window window;
    Atom message_type;      /* WM_PROTOCOLS */
    int format;             /* 32 */
    union {
        char  b[20];
        short s[10];
        long  l[5];         /* l[0] == WM_DELETE_WINDOW on close */
    } data;
} XClientMessageEvent;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window event;
    Window window;
} XDestroyWindowEvent;

typedef union _XEvent {
    int type;
    XAnyEvent           xany;
    XKeyEvent           xkey;
    XButtonEvent        xbutton;
    XMotionEvent        xmotion;
    XExposeEvent        xexpose;
    XConfigureEvent     xconfigure;
    XClientMessageEvent xclient;
    XDestroyWindowEvent xdestroywindow;
    long pad[24];
} XEvent;

/* ---- connection / screen ------------------------------------------ */

Display *XOpenDisplay(const char *display_name);
int      XCloseDisplay(Display *dpy);

int           XDefaultScreen(Display *dpy);
Window        XDefaultRootWindow(Display *dpy);
unsigned long XBlackPixel(Display *dpy, int screen);
unsigned long XWhitePixel(Display *dpy, int screen);
int           XDisplayWidth(Display *dpy, int screen);
int           XDisplayHeight(Display *dpy, int screen);

/* Xlib exposes these both as functions and as macros; provide the
 * macro spellings classic code uses. */
#define DefaultScreen(d)        XDefaultScreen(d)
#define DefaultRootWindow(d)    XDefaultRootWindow(d)
#define BlackPixel(d, s)        XBlackPixel((d), (s))
#define WhitePixel(d, s)        XWhitePixel((d), (s))
#define DisplayWidth(d, s)      XDisplayWidth((d), (s))
#define DisplayHeight(d, s)     XDisplayHeight((d), (s))

/* ---- windows ------------------------------------------------------- */

Window XCreateSimpleWindow(Display *dpy, Window parent,
                           int x, int y,
                           unsigned int width, unsigned int height,
                           unsigned int border_width,
                           unsigned long border,
                           unsigned long background);
int XDestroyWindow(Display *dpy, Window w);
int XMapWindow(Display *dpy, Window w);
int XMapRaised(Display *dpy, Window w);
int XUnmapWindow(Display *dpy, Window w);
int XStoreName(Display *dpy, Window w, const char *window_name);
int XSelectInput(Display *dpy, Window w, long event_mask);
int XClearWindow(Display *dpy, Window w);
int XClearArea(Display *dpy, Window w, int x, int y,
               unsigned int width, unsigned int height, Bool exposures);

/* ---- GC + drawing --------------------------------------------------- */

GC  XCreateGC(Display *dpy, Drawable d,
              unsigned long valuemask, XGCValues *values);
int XFreeGC(Display *dpy, GC gc);
int XSetForeground(Display *dpy, GC gc, unsigned long foreground);
int XSetBackground(Display *dpy, GC gc, unsigned long background);

int XFillRectangle(Display *dpy, Drawable d, GC gc,
                   int x, int y, unsigned int width, unsigned int height);
int XDrawRectangle(Display *dpy, Drawable d, GC gc,
                   int x, int y, unsigned int width, unsigned int height);
int XDrawLine(Display *dpy, Drawable d, GC gc,
              int x1, int y1, int x2, int y2);
int XDrawPoint(Display *dpy, Drawable d, GC gc, int x, int y);
int XDrawString(Display *dpy, Drawable d, GC gc,
                int x, int y, const char *string, int length);

/* Arcs — angles in 1/64ths of a degree, X convention. The shim
 * renders full ellipses exactly; partial arcs are clipped to the
 * angular range per-pixel (good enough for xeyes/xclock-class
 * clients). */
int XFillArc(Display *dpy, Drawable d, GC gc,
             int x, int y, unsigned int width, unsigned int height,
             int angle1, int angle2);
int XDrawArc(Display *dpy, Drawable d, GC gc,
             int x, int y, unsigned int width, unsigned int height,
             int angle1, int angle2);

/* ---- event queue ----------------------------------------------------- */

int XFlush(Display *dpy);
int XSync(Display *dpy, Bool discard);
int XPending(Display *dpy);
int XNextEvent(Display *dpy, XEvent *event_return);

/* ---- pointer ---------------------------------------------------------- */

/* Global pointer position + button mask. root/child returns are the
 * shim's pseudo-root (1) and None. Backed by an oxsrv query, so the
 * position is screen-global even when the pointer is outside `w` —
 * this is what lets xeyes follow the cursor everywhere. */
Bool XQueryPointer(Display *dpy, Window w,
                   Window *root_return, Window *child_return,
                   int *root_x_return, int *root_y_return,
                   int *win_x_return, int *win_y_return,
                   unsigned int *mask_return);

/* ---- atoms + WM protocols -------------------------------------------- */

Atom   XInternAtom(Display *dpy, const char *atom_name, Bool only_if_exists);
Status XSetWMProtocols(Display *dpy, Window w, Atom *protocols, int count);

/* ---- keyboard --------------------------------------------------------- */

/* Real Xlib declares this in <X11/Xutil.h>; declared here too so the
 * common single-include pattern works. XComposeStatus is accepted and
 * ignored. */
typedef struct { XPointer compose_ptr; int chars_matched; } XComposeStatus;
int XLookupString(XKeyEvent *event_struct, char *buffer_return,
                  int bytes_buffer, KeySym *keysym_return,
                  XComposeStatus *status_in_out);
