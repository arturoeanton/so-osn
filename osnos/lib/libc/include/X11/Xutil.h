#pragma once
/*
 * X11/Xutil.h — osnos tinyX subset. The pieces classic clients pull
 * from Xutil (XLookupString, XComposeStatus) live in Xlib.h here;
 * this header exists so `#include <X11/Xutil.h>` compiles unmodified.
 */
#include <X11/Xlib.h>

typedef struct {
    long flags;
    int x, y;
    int width, height;
    int min_width, min_height;
    int max_width, max_height;
} XSizeHints;

#define PPosition  (1L<<2)
#define PSize      (1L<<3)
#define PMinSize   (1L<<4)
#define PMaxSize   (1L<<5)

/* Accepted and ignored — Ox windows are freely resizable. */
static inline void XSetWMNormalHints(Display *dpy, Window w,
                                     XSizeHints *hints) {
    (void)dpy; (void)w; (void)hints;
}
