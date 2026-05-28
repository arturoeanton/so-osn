#pragma once

#include <stdint.h>

/* asm-generic/ioctls.h subset. */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

/* Framebuffer ioctls (FASE 12 — ox window system).
 * Constants + types also exposed via <linux/fb.h> so the eventual
 * tinyX port works against either header. Numeric values are wire-
 * locked with src/include/osnos_fb_abi.h. */
#define FBIOGET_VSCREENINFO  0x4600    /* Linux-compat               */
#define FBIO_BLIT            0x4680    /* osnos-specific blit rect   */

struct fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t line_length;
    uint8_t  red_offset;
    uint8_t  green_offset;
    uint8_t  blue_offset;
    uint8_t  alpha_offset;
};

struct fb_blit_req {
    uint32_t    x;
    uint32_t    y;
    uint32_t    w;
    uint32_t    h;
    const void *src;
    uint32_t    src_pitch;
};

int ioctl(int fd, unsigned long request, ...);
