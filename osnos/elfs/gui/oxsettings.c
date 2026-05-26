/*
 * /bin/oxsettings — wallpaper picker for Ox (GNOME Background style).
 *
 * Reads thumbnails from /home/wallpapers/thumbs/<name>.ppm (200x120
 * P6 binary, generated at build time by tools/gen_wallpapers.sh) and
 * renders them as a tile grid. Click a tile to select; "Apply" writes
 * /home/.oxrc and IPC_OX_RELOAD_SETTINGS-es oxsrv to repaint.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <osnos_ipc.h>
#include <ox.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

/* ---------------- geometry ----------------------------------------- */
#define WIN_W       720
#define WIN_H       560
#define HEADER_H     44
#define FOOTER_H     56
#define TILE_W      200
#define TILE_H      120
#define TILE_PAD     12          /* gap between tiles */
#define LABEL_H      20          /* below thumbnail */
#define BORDER_W      3          /* selection border thickness */
#define GRID_COLS     3

#define MAX_WALLS    16

/* ---------------- palette (Adwaita dark) --------------------------- */
#define COL_HEADER      OX_RGB( 30, 30, 30)   /* #1e1e1e */
#define COL_BODY        OX_RGB( 36, 36, 36)   /* #242424 */
#define COL_FOOTER      OX_RGB( 30, 30, 30)
#define COL_DIVIDER     OX_RGB( 18, 18, 18)
#define COL_TILE_BG     OX_RGB( 48, 48, 48)   /* behind thumb if load fails */
#define COL_TEXT        OX_RGB(255,255,255)
#define COL_TEXT_DIM    OX_RGB(154,153,150)
#define COL_ACCENT      OX_RGB( 53,132,228)   /* #3584e4 */
#define COL_ACCENT_HOV  OX_RGB( 99,164,235)
#define COL_BTN         OX_RGB( 53,132,228)
#define COL_BTN_HOVER   OX_RGB( 99,164,235)
#define COL_BTN_FG      OX_RGB(255,255,255)
#define COL_HOVER       OX_RGB( 60, 60, 60)

/* ---------------- state -------------------------------------------- */
typedef struct {
    char     name[40];
    uint32_t *thumb;       /* TILE_W * TILE_H BGRA pixels, or NULL on fail */
    int       thumb_w;
    int       thumb_h;
} wall_t;

static wall_t g_walls[MAX_WALLS];
static int    g_n_walls = 0;
static int    g_selected = -1;     /* index into g_walls, -1 if none */
static int    g_hover = -1;
static int    g_scroll = 0;        /* first visible row */
static int    g_apply_hover = 0;
static ox_win_t g_win;

/* ---------------- PPM P6 loader ------------------------------------ */
static int read_ws(int fd, char *c) {
    for (;;) {
        if (read(fd, c, 1) != 1) return -1;
        if (*c == '#') {
            char d;
            do { if (read(fd, &d, 1) != 1) return -1; } while (d != '\n');
            continue;
        }
        if (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') continue;
        return 0;
    }
}
static int read_int(int fd, int *out) {
    char c;
    if (read_ws(fd, &c) < 0) return -1;
    int v = 0;
    while (c >= '0' && c <= '9') {
        v = v * 10 + (c - '0');
        if (read(fd, &c, 1) != 1) break;
    }
    *out = v;
    return 0;
}

/* Load a PPM P6, return malloc'd BGRA buffer (alpha=0xFF). */
static uint32_t *load_thumb(const char *path, int *out_w, int *out_h) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    char magic[2];
    if (read(fd, magic, 2) != 2 || magic[0] != 'P' || magic[1] != '6') {
        close(fd); return NULL;
    }
    int w, h, maxv;
    if (read_int(fd, &w) < 0 || read_int(fd, &h) < 0 || read_int(fd, &maxv) < 0) {
        close(fd); return NULL;
    }
    /* read_int already consumed the trailing whitespace byte (its loop
     * reads one byte past the last digit). Pixel data starts here.
     * (Earlier extra read() stole the first pixel byte → bulk-read
     * went short → load failed → all tiles fell to placeholder.) */
    if (w <= 0 || h <= 0 || maxv != 255) { close(fd); return NULL; }
    /* Bulk-read the entire pixel block in one syscall (was: 3 bytes per
     * pixel × w*h reads = ~24k syscalls per thumbnail, painfully slow
     * over the FAT driver). */
    size_t npix = (size_t)w * h;
    size_t rgb_sz = npix * 3;
    unsigned char *rgb_buf = malloc(rgb_sz);
    if (!rgb_buf) { close(fd); return NULL; }
    size_t got = 0;
    while (got < rgb_sz) {
        ssize_t n = read(fd, rgb_buf + got, rgb_sz - got);
        if (n <= 0) { free(rgb_buf); close(fd); return NULL; }
        got += (size_t)n;
    }
    close(fd);
    uint32_t *pix = malloc(npix * sizeof(uint32_t));
    if (!pix) { free(rgb_buf); return NULL; }
    for (size_t i = 0; i < npix; i++) {
        pix[i] = ((uint32_t)rgb_buf[i*3]     << 16) |
                 ((uint32_t)rgb_buf[i*3 + 1] << 8)  |
                  (uint32_t)rgb_buf[i*3 + 2];
    }
    free(rgb_buf);
    *out_w = w; *out_h = h;
    return pix;
}

/* ---------------- scan + load ------------------------------------- */
static void scan_walls(void) {
    DIR *d = opendir("/home/wallpapers");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_n_walls < MAX_WALLS) {
        const char *name = e->d_name;
        if (name[0] == '.') continue;
        size_t L = strlen(name);
        if (L < 5 || strcmp(name + L - 4, ".ppm") != 0) continue;
        size_t base = L - 4;
        if (base >= sizeof(g_walls[0].name)) base = sizeof(g_walls[0].name) - 1;
        memcpy(g_walls[g_n_walls].name, name, base);
        g_walls[g_n_walls].name[base] = 0;
        g_walls[g_n_walls].thumb = NULL;
        g_walls[g_n_walls].thumb_w = 0;
        g_walls[g_n_walls].thumb_h = 0;
        /* Try to load thumbnail. */
        char path[256];
        snprintf(path, sizeof(path),
                 "/home/wallpapers/thumbs/%s", name);
        g_walls[g_n_walls].thumb =
            load_thumb(path, &g_walls[g_n_walls].thumb_w,
                       &g_walls[g_n_walls].thumb_h);
        g_n_walls++;
    }
    closedir(d);
}

static void read_current(void) {
    int fd = open("/home/.oxrc", O_RDONLY);
    if (fd < 0) return;
    char buf[256];
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    char *p = strstr(buf, "current_wallpaper=");
    if (!p) return;
    p += strlen("current_wallpaper=");
    char *nl = strchr(p, '\n');
    if (nl) *nl = 0;
    for (int i = 0; i < g_n_walls; i++) {
        if (strcmp(g_walls[i].name, p) == 0) { g_selected = i; return; }
    }
}

static void write_current(void) {
    if (g_selected < 0 || g_selected >= g_n_walls) return;
    int fd = open("/home/.oxrc", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "current_wallpaper=%s\n", g_walls[g_selected].name);
    write(fd, buf, n);
    close(fd);
}

static void notify_oxsrv(void) {
    if (ipc_service_lookup(SERVER_OX) <= 0) return;
    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.to = SERVER_OX;
    m.type = IPC_OX_RELOAD_SETTINGS;
    ipc_send(&m);
}

/* ---------------- layout helpers ----------------------------------- */
static int grid_origin_x(void) {
    /* center the grid. */
    int total_w = GRID_COLS * TILE_W + (GRID_COLS - 1) * TILE_PAD;
    return (WIN_W - total_w) / 2;
}
static int grid_origin_y(void) { return HEADER_H + 16; }

static void tile_xy(int idx, int *out_x, int *out_y) {
    int col = idx % GRID_COLS;
    int row = idx / GRID_COLS - g_scroll;
    *out_x = grid_origin_x() + col * (TILE_W + TILE_PAD);
    *out_y = grid_origin_y() + row * (TILE_H + LABEL_H + TILE_PAD);
}

static int apply_btn_x(void) { return WIN_W - 100 - 16; }
static int apply_btn_y(void) { return WIN_H - FOOTER_H + (FOOTER_H - 32) / 2; }

/* ---------------- render ------------------------------------------- */

/* Paint one tile: border (background) + thumbnail + label.
 * Used by both full render() and incremental updates on hover/select.
 * This is the EXPENSIVE bit because each thumbnail is ~120 IPC sends. */
static void render_tile(int i) {
    int tx, ty;
    tile_xy(i, &tx, &ty);
    int is_sel = (i == g_selected);
    int is_hov = (i == g_hover);
    /* Border region surrounding the tile — body color when neither
     * sel/hov, accent or hover-gray otherwise. */
    uint32_t border = COL_BODY;
    if (is_sel)      border = COL_ACCENT;
    else if (is_hov) border = COL_HOVER;
    ox_draw_rect(g_win, tx - BORDER_W, ty - BORDER_W,
                 TILE_W + 2 * BORDER_W,
                 TILE_H + LABEL_H + 2 * BORDER_W, border);
    /* Thumbnail (or flat placeholder if load failed — name only goes
     * on the label below to avoid double-printing). */
    if (g_walls[i].thumb && g_walls[i].thumb_w == TILE_W
                          && g_walls[i].thumb_h == TILE_H) {
        ox_draw_image(g_win, tx, ty, TILE_W, TILE_H,
                      g_walls[i].thumb, TILE_W);
    } else {
        ox_draw_rect(g_win, tx, ty, TILE_W, TILE_H, COL_TILE_BG);
    }
    /* Label (must be repainted because changing border alpha changes it). */
    int nlen = (int)strlen(g_walls[i].name) * 8;
    int lx = tx + (TILE_W - nlen) / 2;
    int ly = ty + TILE_H + 6;
    /* Wipe the label band first (so we don't overlay text on stale border). */
    ox_draw_rect(g_win, tx, ty + TILE_H, TILE_W, LABEL_H, COL_BODY);
    ox_draw_text(g_win, lx, ly, g_walls[i].name,
                 is_sel ? COL_TEXT : COL_TEXT_DIM);
}

/* Lightweight border-only update for hover changes — does NOT re-blit
 * the thumbnail. Paints only the 4 strips that form the border. */
static void render_tile_border(int i) {
    int tx, ty;
    tile_xy(i, &tx, &ty);
    int is_sel = (i == g_selected);
    int is_hov = (i == g_hover);
    uint32_t border = COL_BODY;
    if (is_sel)      border = COL_ACCENT;
    else if (is_hov) border = COL_HOVER;
    /* Top strip. */
    ox_draw_rect(g_win, tx - BORDER_W, ty - BORDER_W,
                 TILE_W + 2 * BORDER_W, BORDER_W, border);
    /* Bottom strip (covers below label area). */
    ox_draw_rect(g_win, tx - BORDER_W, ty + TILE_H + LABEL_H,
                 TILE_W + 2 * BORDER_W, BORDER_W, border);
    /* Left strip. */
    ox_draw_rect(g_win, tx - BORDER_W, ty - BORDER_W,
                 BORDER_W, TILE_H + LABEL_H + 2 * BORDER_W, border);
    /* Right strip. */
    ox_draw_rect(g_win, tx + TILE_W, ty - BORDER_W,
                 BORDER_W, TILE_H + LABEL_H + 2 * BORDER_W, border);
    /* Re-paint the label color (selected = brighter). */
    int nlen = (int)strlen(g_walls[i].name) * 8;
    int lx = tx + (TILE_W - nlen) / 2;
    int ly = ty + TILE_H + 6;
    ox_draw_rect(g_win, tx, ty + TILE_H, TILE_W, LABEL_H, COL_BODY);
    ox_draw_text(g_win, lx, ly, g_walls[i].name,
                 is_sel ? COL_TEXT : COL_TEXT_DIM);
}

/* Paint header + footer + Apply button (no tiles). Cheap. */
static void render_chrome(void) {
    /* zones */
    ox_draw_rect(g_win, 0, 0, WIN_W, HEADER_H, COL_HEADER);
    ox_draw_rect(g_win, 0, WIN_H - FOOTER_H, WIN_W, FOOTER_H, COL_FOOTER);
    ox_draw_rect(g_win, 0, HEADER_H - 1, WIN_W, 1, COL_DIVIDER);
    ox_draw_rect(g_win, 0, WIN_H - FOOTER_H, WIN_W, 1, COL_DIVIDER);
    ox_draw_text(g_win, 18, (HEADER_H - 8) / 2, "Background", COL_TEXT);
    char count[32];
    snprintf(count, sizeof(count), "%d wallpapers", g_n_walls);
    int count_x = WIN_W - 18 - (int)strlen(count) * 8;
    ox_draw_text(g_win, count_x, (HEADER_H - 8) / 2, count, COL_TEXT_DIM);
    /* Selected name in footer. */
    if (g_selected >= 0) {
        char sel_label[80];
        snprintf(sel_label, sizeof(sel_label),
                 "Selected: %s", g_walls[g_selected].name);
        ox_draw_text(g_win, 18,
                     WIN_H - FOOTER_H + (FOOTER_H - 8) / 2,
                     sel_label, COL_TEXT);
    }
    int ax = apply_btn_x(), ay = apply_btn_y();
    ox_draw_rect(g_win, ax, ay, 100, 32,
                 g_apply_hover ? COL_BTN_HOVER : COL_BTN);
    ox_draw_text(g_win, ax + (100 - 5 * 8) / 2, ay + (32 - 8) / 2,
                 "Apply", COL_BTN_FG);
}

static void render(void) {
    /* Body fill behind tiles. */
    ox_draw_rect(g_win, 0, HEADER_H, WIN_W, WIN_H - HEADER_H - FOOTER_H, COL_BODY);
    render_chrome();
    for (int i = 0; i < g_n_walls; i++) {
        int tx, ty;
        tile_xy(i, &tx, &ty);
        if (ty + TILE_H + LABEL_H < HEADER_H) continue;
        if (ty > WIN_H - FOOTER_H) continue;
        render_tile(i);
    }
    /* empty-state */
    if (g_n_walls == 0) {
        const char *msg = "no wallpapers found in /home/wallpapers/";
        int len = (int)strlen(msg) * 8;
        ox_draw_text(g_win, (WIN_W - len) / 2, HEADER_H + 60,
                     msg, COL_TEXT_DIM);
    }

    /* footer — selected name (left) + Apply button (right) */
    if (g_selected >= 0) {
        char sel_label[80];
        snprintf(sel_label, sizeof(sel_label),
                 "Selected: %s", g_walls[g_selected].name);
        ox_draw_text(g_win, 18,
                     WIN_H - FOOTER_H + (FOOTER_H - 8) / 2,
                     sel_label, COL_TEXT);
    }
    /* Apply button — accent blue (matches GNOME suggested-action). */
    int ax = apply_btn_x(), ay = apply_btn_y();
    ox_draw_rect(g_win, ax, ay, 100, 32,
                 g_apply_hover ? COL_BTN_HOVER : COL_BTN);
    ox_draw_text(g_win, ax + (100 - 5 * 8) / 2, ay + (32 - 8) / 2,
                 "Apply", COL_BTN_FG);

    ox_present(g_win);
}

/* ---------------- hit tests ---------------------------------------- */
static int hit_tile(int mx, int my) {
    for (int i = 0; i < g_n_walls; i++) {
        int tx, ty;
        tile_xy(i, &tx, &ty);
        if (mx >= tx && mx < tx + TILE_W &&
            my >= ty && my < ty + TILE_H + LABEL_H) {
            return i;
        }
    }
    return -1;
}
static int hit_apply(int mx, int my) {
    int ax = apply_btn_x(), ay = apply_btn_y();
    return (mx >= ax && mx < ax + 100 && my >= ay && my < ay + 32);
}

/* ---------------- main --------------------------------------------- */
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (ox_init() < 0) return 1;
    g_win = ox_window_create(WIN_W, WIN_H, "Settings");
    if (g_win < 0) return 1;
    /* First paint: empty chrome so the window doesn't sit in its
     * default grey while we slow-load thumbnails from disk. */
    render();
    scan_walls();
    read_current();
    /* Re-render with the loaded thumbnails. */
    render();
    /* Scroll bookkeeping: grid is GRID_COLS wide. Total rows = ceil(N/cols).
     * Visible rows fit within (body_h) / (tile + label + pad). */
    int total_rows  = (g_n_walls + GRID_COLS - 1) / GRID_COLS;
    int row_height  = TILE_H + LABEL_H + TILE_PAD;
    int body_h      = WIN_H - HEADER_H - FOOTER_H - 16;
    int visible_rows = body_h / row_height;
    if (visible_rows < 1) visible_rows = 1;
    int max_scroll  = total_rows - visible_rows;
    if (max_scroll < 0) max_scroll = 0;

    for (;;) {
        ox_event_t ev;
        if (!ox_wait_event(&ev)) continue;
        if (ev.type == OX_EV_CLOSE) break;
        if (ev.type == OX_EV_KEY) {
            int kc = ev.keycode;
            int new_scroll = g_scroll;
            if      (kc == OX_KEY_UP   || kc == OX_KEY_PGUP) new_scroll--;
            else if (kc == OX_KEY_DOWN || kc == OX_KEY_PGDN) new_scroll++;
            else if (kc == OX_KEY_HOME)                       new_scroll = 0;
            else if (kc == OX_KEY_END)                        new_scroll = max_scroll;
            if (new_scroll < 0)           new_scroll = 0;
            if (new_scroll > max_scroll)  new_scroll = max_scroll;
            if (new_scroll != g_scroll) {
                g_scroll = new_scroll;
                render();
            }
            continue;
        }
        if (ev.type == OX_EV_MOUSE) {
            if (ev.mouse_kind == OX_MOUSE_WHEEL) {
                int new_scroll = g_scroll - ev.wheel_delta;
                if (new_scroll < 0)           new_scroll = 0;
                if (new_scroll > max_scroll)  new_scroll = max_scroll;
                if (new_scroll != g_scroll) {
                    g_scroll = new_scroll;
                    render();
                }
                continue;
            }
            int tile = hit_tile(ev.x, ev.y);
            int over_apply = hit_apply(ev.x, ev.y);
            if (ev.mouse_kind == OX_MOUSE_MOVE) {
                /* Hover changes: only repaint affected tile borders +
                 * Apply button if its state changed. NO thumb re-blit. */
                if (tile != g_hover) {
                    int prev = g_hover;
                    g_hover = tile;
                    if (prev >= 0)  render_tile_border(prev);
                    if (tile >= 0)  render_tile_border(tile);
                    ox_present(g_win);
                }
                if (over_apply != g_apply_hover) {
                    g_apply_hover = over_apply;
                    render_chrome();
                    ox_present(g_win);
                }
            } else if (ev.mouse_kind == OX_MOUSE_DOWN) {
                if (over_apply) {
                    if (g_selected >= 0) {
                        write_current();
                        notify_oxsrv();
                    }
                } else if (tile >= 0) {
                    if (g_selected == tile) {
                        write_current();
                        notify_oxsrv();
                    } else {
                        int prev = g_selected;
                        g_selected = tile;
                        if (prev >= 0) render_tile_border(prev);
                        render_tile_border(tile);
                        render_chrome();   /* updates "Selected: ..." */
                        ox_present(g_win);
                    }
                }
            }
        }
    }
    /* Cleanup — free thumbnails. */
    for (int i = 0; i < g_n_walls; i++) {
        if (g_walls[i].thumb) free(g_walls[i].thumb);
    }
    ox_window_destroy(g_win);
    return 0;
}
