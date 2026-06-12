/*
 * /bin/xdemo — classic Xlib client, compiled UNMODIFIED against the
 * osnos tinyX shim (lib/libc/include/X11/ + lib/libc/xlib.c).
 *
 * This file deliberately uses only stock Xlib idioms — if you paste it
 * into a Linux box and `cc xdemo.c -lX11`, it builds and runs the same
 * way. It is the acceptance test for the X compat track: window +
 * title, WM_DELETE_WINDOW handshake, Expose-driven redraw, resize via
 * ConfigureNotify, keyboard via XLookupString, mouse clicks.
 *
 * Interactions:
 *   click  — stamps a small square at the pointer
 *   r/g/b  — change the stamp color
 *   c      — clear the canvas
 *   q/Esc  — quit (also the window close button via WM_DELETE_WINDOW)
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <string.h>

#define MAX_STAMPS 256

typedef struct { int x, y; unsigned long color; } stamp_t;

static stamp_t      g_stamps[MAX_STAMPS];
static int          g_n_stamps = 0;
static unsigned long g_color = 0xE53935;   /* red */
static int          g_w = 480, g_h = 360;

static void redraw(Display *dpy, Window win, GC gc) {
    XClearWindow(dpy, win);

    /* Header. */
    XSetForeground(dpy, gc, 0x355E9E);
    XFillRectangle(dpy, win, gc, 0, 0, (unsigned int)g_w, 28);
    XSetForeground(dpy, gc, 0xFFFFFF);
    {
        const char *t = "Xlib on OSnOS — tinyX over Ox";
        XDrawString(dpy, win, gc, 10, 19, t, (int)strlen(t));
    }

    /* Palette hint + canvas frame. */
    XSetForeground(dpy, gc, 0x444444);
    {
        char hint[96];
        snprintf(hint, sizeof(hint),
                 "click=stamp  r/g/b=color  c=clear  q=quit  [%dx%d]",
                 g_w, g_h);
        XDrawString(dpy, win, gc, 10, 46, hint, (int)strlen(hint));
    }
    XDrawRectangle(dpy, win, gc, 8, 54,
                   (unsigned int)(g_w - 16), (unsigned int)(g_h - 62));

    /* Current color swatch (top-right). */
    XSetForeground(dpy, gc, g_color);
    XFillRectangle(dpy, win, gc, g_w - 36, 34, 28, 14);
    XSetForeground(dpy, gc, 0x000000);
    XDrawRectangle(dpy, win, gc, g_w - 36, 34, 28, 14);

    /* Stamps + a connecting polyline to exercise XDrawLine. */
    for (int i = 0; i < g_n_stamps; i++) {
        XSetForeground(dpy, gc, g_stamps[i].color);
        XFillRectangle(dpy, win, gc,
                       g_stamps[i].x - 5, g_stamps[i].y - 5, 10, 10);
        if (i > 0) {
            XSetForeground(dpy, gc, 0x999999);
            XDrawLine(dpy, win, gc,
                      g_stamps[i-1].x, g_stamps[i-1].y,
                      g_stamps[i].x,   g_stamps[i].y);
        }
    }

    XFlush(dpy);
}

int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "xdemo: cannot open display\n");
        return 1;
    }
    int screen = DefaultScreen(dpy);

    Window win = XCreateSimpleWindow(
        dpy, DefaultRootWindow(dpy), 60, 60,
        (unsigned int)g_w, (unsigned int)g_h, 1,
        BlackPixel(dpy, screen), 0xF2F1EC /* warm gray canvas */);
    if (!win) return 1;

    XStoreName(dpy, win, "xdemo");
    XSelectInput(dpy, win,
                 ExposureMask | KeyPressMask | ButtonPressMask |
                 StructureNotifyMask);

    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    XMapWindow(dpy, win);

    GC gc = XCreateGC(dpy, win, 0, NULL);

    int running = 1;
    while (running) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        switch (ev.type) {
        case Expose:
            if (ev.xexpose.count == 0) redraw(dpy, win, gc);
            break;
        case ConfigureNotify:
            if (ev.xconfigure.width  != g_w ||
                ev.xconfigure.height != g_h) {
                g_w = ev.xconfigure.width;
                g_h = ev.xconfigure.height;
                redraw(dpy, win, gc);
            }
            break;
        case ButtonPress:
            if (ev.xbutton.button == Button1 &&
                g_n_stamps < MAX_STAMPS) {
                g_stamps[g_n_stamps].x = ev.xbutton.x;
                g_stamps[g_n_stamps].y = ev.xbutton.y;
                g_stamps[g_n_stamps].color = g_color;
                g_n_stamps++;
                redraw(dpy, win, gc);
            }
            break;
        case KeyPress: {
            char buf[8];
            KeySym ks = NoSymbol;
            XLookupString(&ev.xkey, buf, sizeof(buf), &ks, NULL);
            if (ks == XK_Escape || ks == XK_q) running = 0;
            else if (ks == (KeySym)'r') { g_color = 0xE53935; redraw(dpy, win, gc); }
            else if (ks == (KeySym)'g') { g_color = 0x43A047; redraw(dpy, win, gc); }
            else if (ks == (KeySym)'b') { g_color = 0x1E88E5; redraw(dpy, win, gc); }
            else if (ks == (KeySym)'c') { g_n_stamps = 0; redraw(dpy, win, gc); }
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

    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
