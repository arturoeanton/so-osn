#pragma once

#include <stdint.h>

/*
 * OSnOS framebuffer ABI — shared between kernel and ring-3.
 * Mirrors a thin subset of Linux <linux/fb.h> so the eventual
 * tinyX / X11 port can `#include <linux/fb.h>` and find the same
 * shape. New fields go at the end; numeric ioctl values are wire-
 * locked.
 */

/* Linux ioctl numbers we honour today. */
#define OSNOS_FBIOGET_VSCREENINFO 0x4600     /* Linux FBIOGET_VSCREENINFO */

/*
 * osnos-specific: blit a rectangle from a user-supplied BGRA buffer
 * into the framebuffer. Chosen above the Linux-claimed 0x46xx range
 * to avoid collision with future Linux additions while staying close
 * enough that headers feel related.
 */
#define OSNOS_FBIO_BLIT           0x4680

/* Layout-compatible (size-truncated) subset of Linux struct
 * fb_var_screeninfo. Today we only fill the geometry + pixel format
 * fields a window system needs. */
struct osnos_fb_var_screeninfo {
    uint32_t xres;            /* visible resolution X */
    uint32_t yres;            /* visible resolution Y */
    uint32_t xres_virtual;    /* == xres for now      */
    uint32_t yres_virtual;    /* == yres for now      */
    uint32_t xoffset;         /* 0 — no panning       */
    uint32_t yoffset;         /* 0                    */
    uint32_t bits_per_pixel;  /* always 32 today      */
    uint32_t line_length;     /* row stride in BYTES  */
    /* Channel offsets — Limine uses BGRA on x86_64 (32 bpp). */
    uint8_t  red_offset;      /* 16 for BGRA          */
    uint8_t  green_offset;    /* 8                    */
    uint8_t  blue_offset;     /* 0                    */
    uint8_t  alpha_offset;    /* 24                   */
};

/* Argument to OSNOS_FBIO_BLIT. `src` points into user-space; the
 * kernel copy_from_user's the rectangle row by row. */
struct osnos_fb_blit_req {
    uint32_t    x;
    uint32_t    y;
    uint32_t    w;
    uint32_t    h;
    const void *src;          /* user-space pointer, BGRA pixels */
    uint32_t    src_pitch;    /* bytes per source row            */
};
