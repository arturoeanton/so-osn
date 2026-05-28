/*
 * gen_placeholder.c — host C program that emits a 1280×800 PPM P6
 * wallpaper procedurally. Two themes: "samurai" (sunset gradient +
 * silhouette of a samurai with katana) and "girl" (lavender→pink
 * diagonal with a long-haired silhouette). Both are deliberately
 * stylized rather than photorealistic — they ship so the GUI has
 * something to display when no real assets exist in
 * res/wallpapers/source/.
 *
 * Build: cc -O2 tools/gen_placeholder.c -o build/genplc
 * Run:   ./build/genplc samurai out.ppm
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 1280
#define H 800

static uint8_t buf[H][W][3];

static int lerp(int a, int b, int num, int den) {
    return a + (b - a) * num / den;
}

/* Returns 1 if the pixel (x, y) lies inside any silhouette polygon
 * for the given theme. Simple convex-fill approach: for each scanline
 * compute the silhouette's [left, right] interval. */

/* --- Samurai silhouette --------------------------------------------- */
/* Body widens from head (y=350) to base (y=780). Head circle radius 60
 * centred at (640, 320). Arms extend at y=450. Katana diagonal from
 * right hand to upper-right. */
static int in_samurai(int x, int y) {
    int dx, dy;
    /* Head. */
    dx = x - 640; dy = y - 320;
    if (dx * dx + dy * dy <= 60 * 60) return 1;
    /* Body. */
    if (y >= 380 && y <= 780) {
        int half = 80 + (y - 380) / 8;
        if (x >= 640 - half && x <= 640 + half) return 1;
    }
    /* Arms (horizontal bar at chest). */
    if (y >= 450 && y <= 490) {
        if (x >= 510 && x <= 770) return 1;
    }
    /* Katana — diagonal stripe from right hand (770, 470) upward+right
     * to (1050, 200). 8px thick. */
    {
        int kx1 = 770, ky1 = 470, kx2 = 1050, ky2 = 200;
        long long num   = (long long)(y - ky1) * (kx2 - kx1)
                        - (long long)(x - kx1) * (ky2 - ky1);
        long long denom = ((long long)(kx2 - kx1) * (kx2 - kx1) +
                            (long long)(ky2 - ky1) * (ky2 - ky1));
        if (denom != 0) {
            long long dist2 = num * num / denom;
            if (dist2 <= 16) {
                /* Inside the segment range. */
                int minx = kx1 < kx2 ? kx1 : kx2;
                int maxx = kx1 > kx2 ? kx1 : kx2;
                if (x >= minx - 6 && x <= maxx + 6) return 1;
            }
        }
    }
    return 0;
}

/* --- Anime girl silhouette ----------------------------------------- */
/* Long-haired figure: oval head (640, 280) r=70/90, hair falls to y=560,
 * shoulders 540-580, torso narrowing to base. */
static int in_girl(int x, int y) {
    int dx, dy;
    /* Hair — wide oval around the head. */
    dx = x - 640; dy = y - 360;
    if ((long long)dx * dx * 90 * 90 + (long long)dy * dy * 130 * 130
        <= (long long)90 * 90 * 130 * 130) {
        /* Cut hair below y=560 (let body show through). */
        if (y < 560) return 1;
    }
    /* Hair locks falling on shoulders: rectangles. */
    if (y >= 350 && y <= 600) {
        if ((x >= 540 && x <= 580) || (x >= 700 && x <= 740)) return 1;
    }
    /* Head (inside hair). */
    dx = x - 640; dy = y - 310;
    if (dx * dx + dy * dy <= 65 * 65) return 1;
    /* Body. */
    if (y >= 470 && y <= 780) {
        int half = 50 + (y - 470) / 10;
        if (x >= 640 - half && x <= 640 + half) return 1;
    }
    /* Shoulders. */
    if (y >= 470 && y <= 510) {
        if (x >= 550 && x <= 730) return 1;
    }
    return 0;
}

static void gen_samurai(void) {
    for (int y = 0; y < H; y++) {
        /* Sunset gradient: deep red → orange → yellow → near-black sky bottom. */
        int r, g, b;
        if (y < H * 2 / 3) {
            int n = y, d = H * 2 / 3;
            r = lerp(255, 220, n, d);
            g = lerp(190, 60,  n, d);
            b = lerp( 60, 30,  n, d);
        } else {
            int n = y - H * 2 / 3, d = H / 3;
            r = lerp(220, 30,  n, d);
            g = lerp( 60, 10,  n, d);
            b = lerp( 30, 5,   n, d);
        }
        /* Subtle horizontal banding for "atmosphere". */
        int band = (y / 4) & 1 ? 0 : 8;
        r = r + band > 255 ? 255 : r + band;
        for (int x = 0; x < W; x++) {
            /* Sun circle, low in the sky. */
            int dx = x - 640, dy = y - 480;
            if (dx * dx + dy * dy <= 110 * 110) {
                buf[y][x][0] = 255;
                buf[y][x][1] = 230;
                buf[y][x][2] = 130;
                continue;
            }
            if (in_samurai(x, y)) {
                buf[y][x][0] = 10;
                buf[y][x][1] = 10;
                buf[y][x][2] = 15;
            } else {
                buf[y][x][0] = (uint8_t)r;
                buf[y][x][1] = (uint8_t)g;
                buf[y][x][2] = (uint8_t)b;
            }
        }
    }
    /* Ground silhouette: hills along bottom. */
    for (int x = 0; x < W; x++) {
        int hill = 720 + (int)(40 * ((x % 200) < 100
            ? (x % 100) / 8 : (100 - (x % 100)) / 8));
        for (int y = hill; y < H; y++) {
            buf[y][x][0] = 20;
            buf[y][x][1] = 15;
            buf[y][x][2] = 25;
        }
    }
}

static void gen_girl(void) {
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            /* Diagonal gradient: lavender top-left → pink bottom-right. */
            int n = x + y;
            int d = W + H;
            int r = lerp(180, 250, n, d);
            int g = lerp(170, 180, n, d);
            int b = lerp(230, 210, n, d);
            /* Sakura petal speckles. */
            if (((x * 31 + y * 17) % 4099) < 30) {
                buf[y][x][0] = 255;
                buf[y][x][1] = 200;
                buf[y][x][2] = 220;
                continue;
            }
            if (in_girl(x, y)) {
                /* Soft purple silhouette. */
                buf[y][x][0] = 60;
                buf[y][x][1] = 40;
                buf[y][x][2] = 90;
            } else {
                buf[y][x][0] = (uint8_t)r;
                buf[y][x][1] = (uint8_t)g;
                buf[y][x][2] = (uint8_t)b;
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <samurai|girl> <out.ppm>\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "samurai") == 0) {
        gen_samurai();
    } else if (strcmp(argv[1], "girl") == 0) {
        gen_girl();
    } else {
        /* Unknown wallpaper name — happens on hosts without ImageMagick
         * (gen_wallpapers.sh falls back to placeholders for every name
         * in res/wallpapers/source/). Pick one of the two themes based
         * on a tiny hash of the name so the user sees some variety
         * instead of every wallpaper looking identical, and so the
         * build never fails just because the host lacks `convert`. */
        unsigned h = 0;
        for (const char *p = argv[1]; *p; p++) h = h * 31 + (unsigned char)*p;
        if (h & 1) gen_samurai(); else gen_girl();
        fprintf(stderr, "note: '%s' has no procedural theme — using %s "
                        "placeholder (install ImageMagick to use the "
                        "real source image)\n",
                argv[1], (h & 1) ? "samurai" : "girl");
    }
    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    fwrite(buf, 1, sizeof(buf), f);
    fclose(f);
    fprintf(stderr, "wrote %s (%d x %d)\n", argv[2], W, H);
    return 0;
}
