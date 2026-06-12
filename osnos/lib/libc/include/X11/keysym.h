#pragma once
/*
 * X11/keysym.h — osnos tinyX subset. Values match the real X11
 * keysymdef.h exactly (Latin-1 keysyms ARE their ASCII codes; the
 * 0xffXX block is the standard function-key range).
 */

#define XK_BackSpace   0xff08
#define XK_Tab         0xff09
#define XK_Return      0xff0d
#define XK_Escape      0xff1b
#define XK_Delete      0xffff

#define XK_Home        0xff50
#define XK_Left        0xff51
#define XK_Up          0xff52
#define XK_Right       0xff53
#define XK_Down        0xff54
#define XK_Page_Up     0xff55
#define XK_Page_Down   0xff56
#define XK_End         0xff57

#define XK_F1          0xffbe
#define XK_F2          0xffbf
#define XK_F3          0xffc0
#define XK_F4          0xffc1
#define XK_F5          0xffc2
#define XK_F6          0xffc3
#define XK_F7          0xffc4
#define XK_F8          0xffc5
#define XK_F9          0xffc6
#define XK_F10         0xffc7
#define XK_F11         0xffc8
#define XK_F12         0xffc9

#define XK_space       0x0020
#define XK_0           0x0030
#define XK_9           0x0039
#define XK_A           0x0041
#define XK_Z           0x005a
#define XK_a           0x0061
#define XK_q           0x0071
#define XK_z           0x007a
