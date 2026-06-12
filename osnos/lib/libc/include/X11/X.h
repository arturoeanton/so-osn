#pragma once
/*
 * X11/X.h — osnos tinyX subset.
 *
 * Constants and core types lifted from the real X11 protocol headers
 * so X client source code compiles unmodified. Numeric values MATCH
 * stock X11 (this is the same ABI-fidelity rule the rest of osnos
 * follows for errno / keycodes / ioctls). Only the subset the Ox
 * shim implements is present — growing it means copying more values
 * from the real <X11/X.h>, never inventing new ones.
 */

typedef unsigned long XID;
typedef unsigned long Mask;
typedef unsigned long Atom;
typedef unsigned long VisualID;
typedef unsigned long Time;
typedef XID Window;
typedef XID Drawable;
typedef XID Font;
typedef XID Pixmap;
typedef XID Cursor;
typedef XID Colormap;
typedef XID GContext;
typedef XID KeySym;
typedef unsigned char KeyCode;

#define None                 0L
#define ParentRelative       1L
#define CopyFromParent       0L
#define PointerWindow        0L
#define InputFocus           1L
#define PointerRoot          1L
#define AnyPropertyType      0L
#define AnyKey               0L
#define AnyButton            0L
#define AllTemporary         0L
#define CurrentTime          0L
#define NoSymbol             0L

/* Event masks (XSelectInput). */
#define NoEventMask             0L
#define KeyPressMask            (1L<<0)
#define KeyReleaseMask          (1L<<1)
#define ButtonPressMask         (1L<<2)
#define ButtonReleaseMask       (1L<<3)
#define EnterWindowMask         (1L<<4)
#define LeaveWindowMask         (1L<<5)
#define PointerMotionMask       (1L<<6)
#define ButtonMotionMask        (1L<<13)
#define ExposureMask            (1L<<15)
#define VisibilityChangeMask    (1L<<16)
#define StructureNotifyMask     (1L<<17)
#define SubstructureNotifyMask  (1L<<19)
#define FocusChangeMask         (1L<<21)
#define PropertyChangeMask      (1L<<22)

/* Event type codes. */
#define KeyPress         2
#define KeyRelease       3
#define ButtonPress      4
#define ButtonRelease    5
#define MotionNotify     6
#define EnterNotify      7
#define LeaveNotify      8
#define FocusIn          9
#define FocusOut        10
#define KeymapNotify    11
#define Expose          12
#define GraphicsExpose  13
#define NoExpose        14
#define VisibilityNotify 15
#define CreateNotify    16
#define DestroyNotify   17
#define UnmapNotify     18
#define MapNotify       19
#define MapRequest      20
#define ReparentNotify  21
#define ConfigureNotify 22
#define ClientMessage   33
#define MappingNotify   34
#define LASTEvent       36

/* Button names + state masks. */
#define Button1          1
#define Button2          2
#define Button3          3
#define Button4          4
#define Button5          5
#define Button1Mask      (1<<8)
#define Button2Mask      (1<<9)
#define Button3Mask      (1<<10)
#define Button4Mask      (1<<11)
#define Button5Mask      (1<<12)
#define ShiftMask        (1<<0)
#define LockMask         (1<<1)
#define ControlMask      (1<<2)
#define Mod1Mask         (1<<3)

/* XCreateGC value mask bits (accepted, mostly ignored by the shim). */
#define GCFunction        (1L<<0)
#define GCForeground      (1L<<2)
#define GCBackground      (1L<<3)
#define GCLineWidth       (1L<<4)
#define GCFont            (1L<<14)
