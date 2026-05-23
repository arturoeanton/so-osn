#pragma once

/*
 * <linux/fb.h> shim — exposes the framebuffer ABI under the path
 * tinyX / X11 server code expects. Wraps the same constants +
 * structs <sys/ioctl.h> already defines for osnos.
 */

#include <sys/ioctl.h>

/* Linux's struct fb_fix_screeninfo is unused today; tinyX checks for
 * its presence at configure time but doesn't dereference fields we
 * don't fill, so we provide a stub layout. Add fields as needed. */
struct fb_fix_screeninfo {
    char     id[16];
    unsigned long smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;
    unsigned long mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
};
