/*
 * ox_icons.c — runtime-loaded color icons for the Ox WM.
 *
 * Icons are 24x24 raw RGBA byte streams (2304 bytes each) staged at
 * /home/.icons/<name>.rgba on the FAT image. The host-side build
 * downloads PNGs from the Yaru icon theme (Ubuntu, GPL-3) via
 * tools/fetch_icons.sh, ImageMagick resizes/quantizes to 24x24
 * RGBA8888, and `make` mcopy's the .rgba files into the FAT.
 *
 * At runtime, ox_icon_get(name) lazily slurps the file once and
 * caches the bytes. ox_icon_draw alpha-blends each pixel onto a
 * target BGRA buffer, so icons composite correctly against any
 * background (yellow titlebar, dark deskbar, cream menu, etc.).
 *
 * Backward-compatible signature: ox_icon_get returns const uint16_t*
 * for the legacy mono icon path. Color icons use ox_icon_get_rgba.
 * Callers should prefer the RGBA variant and only fall back to the
 * mono one if rgba is NULL.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "include/ox.h"

#define ICON_W 24
#define ICON_H 24
#define ICON_BYTES (ICON_W * ICON_H * 4)
#define ICON_MAX 16

typedef struct {
    char     name[16];
    uint8_t *rgba;          /* NULL if load failed or not yet attempted */
    int      attempted;     /* 1 once we've tried to load */
} icon_slot_t;

static icon_slot_t g_icons[ICON_MAX];
static int         g_icon_n = 0;
static const char *g_icon_dir = "/home/.icons";

static icon_slot_t *slot_for(const char *name) {
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < g_icon_n; i++)
        if (strncmp(g_icons[i].name, name, sizeof g_icons[i].name) == 0)
            return &g_icons[i];
    if (g_icon_n >= ICON_MAX) return NULL;
    icon_slot_t *s = &g_icons[g_icon_n++];
    strncpy(s->name, name, sizeof s->name - 1);
    s->name[sizeof s->name - 1] = 0;
    s->rgba = NULL;
    s->attempted = 0;
    return s;
}

static uint8_t *load_rgba(const char *name) {
    char path[128];
    snprintf(path, sizeof path, "%s/%s.rgba", g_icon_dir, name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size != ICON_BYTES) {
        close(fd);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc(ICON_BYTES);
    if (!buf) { close(fd); return NULL; }
    int got = 0;
    while (got < ICON_BYTES) {
        int n = (int)read(fd, buf + got, ICON_BYTES - got);
        if (n <= 0) { free(buf); close(fd); return NULL; }
        got += n;
    }
    close(fd);
    return buf;
}

/* Try a list of fallback name keys until one loads. Falls back to
 * the generic "app" icon. Returns NULL only if literally nothing on
 * disk was loadable. */
static const uint8_t *get_rgba_internal(const char *name) {
    icon_slot_t *s = slot_for(name);
    if (!s) return NULL;
    if (!s->attempted) {
        s->rgba = load_rgba(s->name);
        s->attempted = 1;
    }
    return s->rgba;
}

const uint8_t *ox_icon_get_rgba(const char *name) {
    if (!name || !name[0]) name = "app";
    const uint8_t *r = get_rgba_internal(name);
    if (r) return r;
    /* Fall back to the generic app icon — same payload shape, looks
     * like the system "tweaks" tile from the Yaru set. Returning
     * NULL would force every caller to repeat this dance. */
    if (strcmp(name, "app") != 0) {
        r = get_rgba_internal("app");
        if (r) return r;
    }
    return NULL;
}

int ox_icon_dims(int *w, int *h) {
    if (w) *w = ICON_W;
    if (h) *h = ICON_H;
    return 1;
}

/* Blend src (RGBA8888) over dst (BGRA stored as 0x00RRGGBB) using the
 * src alpha channel. Plain Porter-Duff "over" math. */
static inline uint32_t alpha_over(uint32_t dst, uint8_t r, uint8_t g,
                                   uint8_t b, uint8_t a) {
    if (a == 0)   return dst;
    if (a == 255) return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    uint32_t dr = (dst >> 16) & 0xff;
    uint32_t dg = (dst >>  8) & 0xff;
    uint32_t db = (dst >>  0) & 0xff;
    uint32_t na = 255 - a;
    uint32_t rr = (r * a + dr * na) / 255;
    uint32_t rg = (g * a + dg * na) / 255;
    uint32_t rb = (b * a + db * na) / 255;
    return (rr << 16) | (rg << 8) | rb;
}

void ox_icon_draw_rgba(uint32_t *buf, int bw, int bh,
                        int x, int y, const uint8_t *rgba) {
    if (!buf || !rgba) return;
    for (int row = 0; row < ICON_H; row++) {
        int py = y + row;
        if (py < 0 || py >= bh) continue;
        const uint8_t *src = rgba + (size_t)row * ICON_W * 4;
        uint32_t *dst = buf + (size_t)py * bw;
        for (int col = 0; col < ICON_W; col++) {
            int px = x + col;
            if (px < 0 || px >= bw) continue;
            uint8_t r = src[col * 4 + 0];
            uint8_t g = src[col * 4 + 1];
            uint8_t b = src[col * 4 + 2];
            uint8_t a = src[col * 4 + 3];
            dst[px] = alpha_over(dst[px], r, g, b, a);
        }
    }
}

/* ---- Legacy mono path -------------------------------------------------
 *
 * Older code paths (and the build-time fallback when no .rgba files
 * are present at /home/.icons/) keep using ox_icon_get / ox_icon_draw
 * with 16x16 uint16_t masks. We synthesize a minimal generic mask
 * here so neither path crashes; the caller can detect the fallback
 * by checking ox_icon_get_rgba() first. */

static const uint16_t ICON_GENERIC_MONO[16] = {
    0x0000, 0x3FFC, 0x2004, 0x2FF4, 0x2004, 0x2FF4, 0x2004, 0x2FF4,
    0x2004, 0x2FF4, 0x2004, 0x2FF4, 0x2004, 0x3FFC, 0x0000, 0x0000,
};

const uint16_t *ox_icon_get(const char *name) {
    (void)name;
    return ICON_GENERIC_MONO;
}

void ox_icon_draw(uint32_t *buf, int bw, int bh, int x, int y,
                   const uint16_t *icon, uint32_t color) {
    if (!buf || !icon) return;
    for (int row = 0; row < OX_ICON_H; row++) {
        uint16_t mask = icon[row];
        if (!mask) continue;
        int py = y + row;
        if (py < 0 || py >= bh) continue;
        for (int col = 0; col < OX_ICON_W; col++) {
            if (mask & (1u << (15 - col))) {
                int px = x + col;
                if (px < 0 || px >= bw) continue;
                buf[(size_t)py * bw + px] = color;
            }
        }
    }
}
