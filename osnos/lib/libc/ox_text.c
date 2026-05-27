/*
 * ox_text.c — high-level proportional text rendering for the Ox WM.
 *
 * Two paths:
 *
 *   1. TrueType (preferred): loads a .ttf via stb_truetype, rasterizes
 *      glyphs into an alpha8 cache, draws with per-pixel alpha blend
 *      onto the BGRA backing buffer. Up to 95 ASCII glyphs cached at
 *      the configured pixel height.
 *
 *   2. Bitmap fallback: the original 8x8 chunky glyphs from ox_font.c.
 *      Used when no TTF could be loaded (no /home/.fonts/default.ttf,
 *      malformed file, etc.). Apps stay readable even without a font.
 *
 * Public API exposed via ox.h. Server (oxsrv) and apps call
 *   ox_text_init("/home/.fonts/default.ttf", 12)
 * once at startup, then everything routes through ox_text_draw /
 * ox_text_width / ox_text_height.
 *
 * The bitmap path costs zero RAM. The TTF path costs ~60 KB for the
 * font file kept in memory + ~24 KB for the glyph cache at 12 px.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "include/ox.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
/* Use libc malloc/free; suppress assert to keep things quiet. */
#define STBTT_assert(x)         ((void)0)
#define STBTT_malloc(s, u)      ((void)(u), malloc(s))
#define STBTT_free(p, u)        ((void)(u), free(p))
#include "../../vendor/stb/stb_truetype.h"

/* --- Glyph cache ---------------------------------------------------- */

#define GLYPH_FIRST     32
#define GLYPH_LAST      127
#define GLYPH_N         (GLYPH_LAST - GLYPH_FIRST)
#define GLYPH_MAX_W     24
#define GLYPH_MAX_H     28

typedef struct {
    int     w, h;
    int     xoff, yoff;
    int     advance;
    int     valid;
    uint8_t alpha[GLYPH_MAX_W * GLYPH_MAX_H];
} ox_glyph_t;

static stbtt_fontinfo g_font;
static uint8_t       *g_font_data = NULL;
static int            g_font_loaded = 0;
static float          g_font_scale = 0.0f;
static int            g_font_ascent = 0;
static int            g_pixel_height = 12;
static ox_glyph_t     g_cache[GLYPH_N];

/* --- File slurp helper --------------------------------------------- */

static uint8_t *slurp_file(const char *path, int *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0 || st.st_size > 4 * 1024 * 1024) {
        close(fd);
        return NULL;
    }
    int len = (int)st.st_size;
    uint8_t *buf = (uint8_t *)malloc((size_t)len);
    if (!buf) { close(fd); return NULL; }
    int got = 0;
    while (got < len) {
        int n = (int)read(fd, buf + got, (size_t)(len - got));
        if (n <= 0) { free(buf); close(fd); return NULL; }
        got += n;
    }
    close(fd);
    if (out_len) *out_len = len;
    return buf;
}

/* --- Public init/teardown ------------------------------------------ */

int ox_text_init(const char *ttf_path, int pixel_height) {
    if (g_font_loaded) return 0;
    if (pixel_height < 8)  pixel_height = 8;
    if (pixel_height > 24) pixel_height = 24;
    g_pixel_height = pixel_height;
    memset(g_cache, 0, sizeof(g_cache));

    if (!ttf_path) return -1;
    int data_len = 0;
    uint8_t *data = slurp_file(ttf_path, &data_len);
    if (!data) return -1;

    if (!stbtt_InitFont(&g_font, data, stbtt_GetFontOffsetForIndex(data, 0))) {
        free(data);
        return -1;
    }
    g_font_data = data;
    g_font_scale = stbtt_ScaleForPixelHeight(&g_font, (float)pixel_height);
    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &line_gap);
    g_font_ascent = (int)((float)ascent * g_font_scale);
    g_font_loaded = 1;
    return 0;
}

int ox_text_loaded(void) { return g_font_loaded; }

/* --- Glyph rasterization ------------------------------------------- */

static ox_glyph_t *get_glyph(int c) {
    if (c < GLYPH_FIRST || c >= GLYPH_LAST) return NULL;
    ox_glyph_t *g = &g_cache[c - GLYPH_FIRST];
    if (g->valid) return g;

    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&g_font, c, &adv, &lsb);
    g->advance = (int)((float)adv * g_font_scale + 0.5f);

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBox(&g_font, c, g_font_scale, g_font_scale,
                                 &x0, &y0, &x1, &y1);
    int w = x1 - x0;
    int h = y1 - y0;
    if (w > GLYPH_MAX_W) w = GLYPH_MAX_W;
    if (h > GLYPH_MAX_H) h = GLYPH_MAX_H;
    g->w = w;
    g->h = h;
    g->xoff = x0;
    g->yoff = y0;
    if (w > 0 && h > 0) {
        stbtt_MakeCodepointBitmap(&g_font, g->alpha, w, h, w,
                                   g_font_scale, g_font_scale, c);
    }
    g->valid = 1;
    return g;
}

/* --- Drawing ------------------------------------------------------- */

int ox_text_height(void) {
    return g_font_loaded ? g_pixel_height : 8;
}

int ox_text_line_height(void) {
    return g_font_loaded ? (g_pixel_height + 4) : 10;
}

static int bitmap_width(const char *s) {
    int n = 0;
    while (*s) {
        if (*s == '\n') break;
        n++;
        s++;
    }
    return n * 8;
}

int ox_text_width(const char *s) {
    if (!s) return 0;
    if (!g_font_loaded) return bitmap_width(s);
    int w = 0;
    while (*s) {
        if (*s == '\n') break;
        ox_glyph_t *g = get_glyph((unsigned char)*s);
        if (g) w += g->advance;
        else   w += g_pixel_height / 2;
        s++;
    }
    return w;
}

/* Alpha-blend a single pixel: dst = lerp(dst, src, a/255). BGRA in
 * the high byte is unused (left as-is); only R/G/B channels matter. */
static inline uint32_t blend(uint32_t dst, uint32_t src, uint8_t a) {
    if (a == 0)   return dst;
    if (a == 255) return src;
    uint32_t dr = (dst >> 16) & 0xff;
    uint32_t dg = (dst >>  8) & 0xff;
    uint32_t db = (dst >>  0) & 0xff;
    uint32_t sr = (src >> 16) & 0xff;
    uint32_t sg = (src >>  8) & 0xff;
    uint32_t sb = (src >>  0) & 0xff;
    uint32_t na = 255 - a;
    uint32_t rr = (sr * a + dr * na) / 255;
    uint32_t rg = (sg * a + dg * na) / 255;
    uint32_t rb = (sb * a + db * na) / 255;
    return (rr << 16) | (rg << 8) | rb;
}

static void draw_bitmap(uint32_t *buf, int bw, int bh,
                         int x, int y, const char *s, uint32_t color) {
    int cx = x;
    while (*s) {
        if (*s == '\n') { y += 10; cx = x; s++; continue; }
        const uint8_t *g = ox_font_glyph((unsigned char)*s);
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                if (g[row] & (1 << (7 - col))) {
                    int px = cx + col, py = y + row;
                    if (px < 0 || py < 0 || px >= bw || py >= bh) continue;
                    buf[(size_t)py * bw + px] = color;
                }
            }
        }
        cx += 8;
        s++;
    }
}

void ox_text_draw(uint32_t *buf, int bw, int bh,
                   int x, int y, const char *s, uint32_t color) {
    if (!buf || !s) return;
    if (!g_font_loaded) { draw_bitmap(buf, bw, bh, x, y, s, color); return; }

    /* Caller's `y` is the top of the line, matching the bitmap path.
     * stbtt yoff is measured from the baseline, so we shift each
     * glyph by the cached ascent. */
    int cx = x;
    int line_y = y;
    while (*s) {
        if (*s == '\n') { cx = x; line_y += ox_text_line_height(); s++; continue; }
        ox_glyph_t *g = get_glyph((unsigned char)*s);
        if (!g) { cx += g_pixel_height / 2; s++; continue; }
        int gx = cx + g->xoff;
        int gy = line_y + g_font_ascent + g->yoff;
        for (int row = 0; row < g->h; row++) {
            int py = gy + row;
            if (py < 0 || py >= bh) continue;
            const uint8_t *src_row = g->alpha + (size_t)row * g->w;
            uint32_t *dst_row = buf + (size_t)py * bw;
            for (int col = 0; col < g->w; col++) {
                int px = gx + col;
                if (px < 0 || px >= bw) continue;
                uint8_t a = src_row[col];
                if (a) dst_row[px] = blend(dst_row[px], color, a);
            }
        }
        cx += g->advance;
        s++;
    }
}
