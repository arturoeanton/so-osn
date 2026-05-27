/*
 * /bin/oxfiles — modern file browser for Ox (GNOME Files / Finder look).
 *
 * Layout:
 *   +-----------------------------------------------+
 *   | [<] [>] [^]  /home                            |  toolbar
 *   +---------+-------------------------------------+
 *   | PLACES  | [icon] name                size     |
 *   |  Home   | [icon] ...                          |
 *   |  Walls  |                                     |
 *   |  System |                                     |  list
 *   +---------+-------------------------------------+
 *
 * Adwaita dark palette matches oxsrv's window decoration. Icons are
 * built from rect primitives (no real icon font on osnos): yellow
 * folder, light-gray file with corner fold.
 *
 * Keys:
 *   Up/Down       — move selection
 *   Enter / Right — open
 *   Backspace     — go up
 *   Home/End      — jump to first/last
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
#include <sys/stat.h>
#include <unistd.h>

#include "../../src/include/osnos_keys.h"

extern char **environ;

/* ---------------- geometry ----------------------------------------- */
#define WIN_W       720
#define WIN_H       500
#define TOOLBAR_H    38
#define SIDEBAR_W   160
#define ROW_H        24
#define ICON_W       16
#define ICON_H       14
#define BTN_W        28
#define BTN_H        24
#define MAX_ENTRIES 128
#define MAX_HISTORY  16

/* ---------------- Adwaita dark palette ----------------------------- */
#define COL_TOOLBAR     OX_RGB( 30, 30, 30)   /* #1e1e1e */
#define COL_SIDEBAR     OX_RGB( 24, 24, 24)   /* slightly darker */
#define COL_BODY        OX_RGB( 36, 36, 36)   /* #242424 */
#define COL_DIVIDER     OX_RGB( 18, 18, 18)
#define COL_BTN         OX_RGB( 50, 50, 50)
#define COL_BTN_HOVER   OX_RGB( 70, 70, 70)
#define COL_BTN_DIS     OX_RGB( 38, 38, 38)   /* button when no history */
#define COL_TEXT        OX_RGB(255,255,255)
#define COL_TEXT_DIM    OX_RGB(154,153,150)   /* #9a9996 */
#define COL_TEXT_DIS    OX_RGB( 90, 90, 90)
#define COL_ACCENT      OX_RGB( 53,132,228)   /* #3584e4 Adwaita blue */
#define COL_HOVER       OX_RGB( 48, 48, 48)
#define COL_SECTION     OX_RGB(154,153,150)
#define COL_FOLDER_BODY OX_RGB(229,165, 10)   /* #e5a50a Adwaita yellow */
#define COL_FOLDER_TAB  OX_RGB(176,127,  7)
#define COL_FILE_BODY   OX_RGB(192,191,188)   /* #c0bfbc light gray */
#define COL_FILE_FOLD   OX_RGB(155,154,151)
#define COL_FILE_LINE   OX_RGB(110,109,107)

/* ---------------- state -------------------------------------------- */
static ox_win_t g_win;
static char     g_cwd[256] = "/home";

typedef struct {
    char     name[64];
    int      is_dir;
    uint64_t size;
} entry_t;

static entry_t g_entries[MAX_ENTRIES];
static int     g_n_entries = 0;
static int     g_scroll = 0;
static int     g_hover = -1;          /* row index hovered (file list) */
static int     g_selected = -1;       /* row index selected */
static int     g_sb_hover = -1;       /* sidebar index hovered */
static int     g_btn_hover = -1;      /* 0=back, 1=fwd, 2=up */

static char    g_hist[MAX_HISTORY][256];
static int     g_hist_n = 0;
static char    g_fwd[MAX_HISTORY][256];
static int     g_fwd_n = 0;

/* ---------------- sidebar entries ---------------------------------- */
typedef struct {
    const char *label;
    const char *path;
} sb_entry_t;

static const sb_entry_t g_sidebar[] = {
    { "Home",       "/home" },
    { "Wallpapers", "/home/wallpapers" },
    { "Etc",        "/etc" },
    { "Bin",        "/bin" },
    { "System",     "/" },
};
#define SB_N (int)(sizeof(g_sidebar)/sizeof(g_sidebar[0]))

/* ---------------- helpers ------------------------------------------ */
static int list_rows_per_page(void) {
    return (WIN_H - TOOLBAR_H) / ROW_H;
}

static void format_size(uint64_t bytes, char *out, size_t out_sz) {
    if (bytes < 1024) {
        snprintf(out, out_sz, "%llu B", (unsigned long long)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, out_sz, "%llu.%llu KB",
                 (unsigned long long)(bytes / 1024),
                 (unsigned long long)((bytes * 10 / 1024) % 10));
    } else {
        snprintf(out, out_sz, "%llu.%llu MB",
                 (unsigned long long)(bytes / (1024 * 1024)),
                 (unsigned long long)((bytes * 10 / (1024 * 1024)) % 10));
    }
}

/* Push g_cwd onto the back-history stack (drop forward stack). */
static void push_history(void) {
    if (g_hist_n >= MAX_HISTORY) {
        /* Shift left (drop oldest). */
        for (int i = 1; i < MAX_HISTORY; i++)
            memcpy(g_hist[i-1], g_hist[i], sizeof(g_hist[0]));
        g_hist_n = MAX_HISTORY - 1;
    }
    strncpy(g_hist[g_hist_n], g_cwd, sizeof(g_hist[0]) - 1);
    g_hist[g_hist_n][sizeof(g_hist[0]) - 1] = 0;
    g_hist_n++;
    g_fwd_n = 0;
}

static void navigate_to(const char *path) {
    if (strcmp(path, g_cwd) == 0) return;
    push_history();
    strncpy(g_cwd, path, sizeof(g_cwd) - 1);
    g_cwd[sizeof(g_cwd) - 1] = 0;
    g_scroll = 0;
    g_selected = -1;
    g_hover = -1;
}

static void go_back(void) {
    if (g_hist_n == 0) return;
    /* push current onto fwd. */
    strncpy(g_fwd[g_fwd_n], g_cwd, sizeof(g_fwd[0]) - 1);
    g_fwd[g_fwd_n][sizeof(g_fwd[0]) - 1] = 0;
    g_fwd_n++;
    /* pop hist. */
    g_hist_n--;
    strncpy(g_cwd, g_hist[g_hist_n], sizeof(g_cwd) - 1);
    g_cwd[sizeof(g_cwd) - 1] = 0;
    g_scroll = 0; g_selected = -1; g_hover = -1;
}

static void go_forward(void) {
    if (g_fwd_n == 0) return;
    /* push current onto hist. */
    strncpy(g_hist[g_hist_n], g_cwd, sizeof(g_hist[0]) - 1);
    g_hist[g_hist_n][sizeof(g_hist[0]) - 1] = 0;
    g_hist_n++;
    /* pop fwd. */
    g_fwd_n--;
    strncpy(g_cwd, g_fwd[g_fwd_n], sizeof(g_cwd) - 1);
    g_cwd[sizeof(g_cwd) - 1] = 0;
    g_scroll = 0; g_selected = -1; g_hover = -1;
}

static void go_up(void) {
    if (strcmp(g_cwd, "/") == 0) return;
    char parent[256];
    strncpy(parent, g_cwd, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = 0;
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) *slash = 0;
    else strcpy(parent, "/");
    navigate_to(parent);
}

static void rescan(void) {
    g_n_entries = 0;
    DIR *d = opendir(g_cwd);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_n_entries < MAX_ENTRIES) {
        if (e->d_name[0] == '.' && e->d_name[1] == 0) continue;
        if (e->d_name[0] == '.' && e->d_name[1] == '.' && e->d_name[2] == 0) continue;
        size_t L = strlen(e->d_name);
        if (L >= sizeof(g_entries[0].name)) L = sizeof(g_entries[0].name) - 1;
        memcpy(g_entries[g_n_entries].name, e->d_name, L);
        g_entries[g_n_entries].name[L] = 0;
        /* stat for type + size. */
        char path[512];
        snprintf(path, sizeof(path), "%s/%s",
                 strcmp(g_cwd, "/") == 0 ? "" : g_cwd, e->d_name);
        struct stat st;
        g_entries[g_n_entries].is_dir = 0;
        g_entries[g_n_entries].size = 0;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) g_entries[g_n_entries].is_dir = 1;
            else                     g_entries[g_n_entries].size = (uint64_t)st.st_size;
        }
        g_n_entries++;
    }
    closedir(d);
    /* Simple sort: dirs first, then case-insensitive alpha. Insertion sort
     * is fine at MAX_ENTRIES=128. */
    for (int i = 1; i < g_n_entries; i++) {
        entry_t key = g_entries[i];
        int j = i - 1;
        while (j >= 0) {
            entry_t *a = &g_entries[j];
            int cmp;
            if (a->is_dir != key.is_dir) cmp = (a->is_dir ? -1 : 1);
            else {
                /* case-insensitive */
                const char *p = a->name, *q = key.name;
                while (*p && *q) {
                    int cp = *p, cq = *q;
                    if (cp >= 'A' && cp <= 'Z') cp += 32;
                    if (cq >= 'A' && cq <= 'Z') cq += 32;
                    if (cp != cq) { cmp = cp - cq; goto cmpdone; }
                    p++; q++;
                }
                cmp = (int)(unsigned char)*p - (int)(unsigned char)*q;
            cmpdone:;
            }
            if (cmp <= 0) break;
            g_entries[j + 1] = g_entries[j];
            j--;
        }
        g_entries[j + 1] = key;
    }
}

/* ---------------- drawing primitives (composed icons) -------------- */

static void draw_folder_icon(int x, int y) {
    /* 16x14 folder: top tab 6x3 at left, body 16x11 below. */
    ox_draw_rect(g_win, x,     y,     7, 3,  COL_FOLDER_TAB);
    ox_draw_rect(g_win, x,     y + 3, 16, 11, COL_FOLDER_BODY);
    /* small inner shadow strip for depth */
    ox_draw_rect(g_win, x + 1, y + 4, 14, 1,  COL_FOLDER_TAB);
}

static void draw_file_icon(int x, int y) {
    /* 14x16 sheet of paper with a folded top-right corner. */
    ox_draw_rect(g_win, x,     y,     11, 16, COL_FILE_BODY);
    ox_draw_rect(g_win, x + 11, y,     3,  3, COL_FILE_FOLD);
    /* a few horizontal "text" lines for visual texture */
    ox_draw_rect(g_win, x + 2, y + 5, 7, 1, COL_FILE_LINE);
    ox_draw_rect(g_win, x + 2, y + 8, 7, 1, COL_FILE_LINE);
    ox_draw_rect(g_win, x + 2, y + 11, 5, 1, COL_FILE_LINE);
}

/* Small text-button at (x,y) of fixed size BTN_W x BTN_H. Disabled
 * dims the background and text. */
static void draw_button(int x, int y, const char *glyph, int hovered, int disabled) {
    uint32_t bg = disabled ? COL_BTN_DIS
                           : (hovered ? COL_BTN_HOVER : COL_BTN);
    uint32_t fg = disabled ? COL_TEXT_DIS : COL_TEXT;
    ox_draw_rect(g_win, x, y, BTN_W, BTN_H, bg);
    /* center the 8x8 glyph. */
    int tx = x + (BTN_W - 8) / 2;
    int ty = y + (BTN_H - 8) / 2;
    ox_draw_text(g_win, tx, ty, glyph, fg);
}

/* ---------------- render ------------------------------------------- */
static void render(void) {
    /* zone backgrounds */
    ox_draw_rect(g_win, 0, 0, WIN_W, TOOLBAR_H, COL_TOOLBAR);
    ox_draw_rect(g_win, 0, TOOLBAR_H, SIDEBAR_W, WIN_H - TOOLBAR_H, COL_SIDEBAR);
    ox_draw_rect(g_win, SIDEBAR_W, TOOLBAR_H,
                 WIN_W - SIDEBAR_W, WIN_H - TOOLBAR_H, COL_BODY);
    /* dividers (1 px) */
    ox_draw_rect(g_win, 0, TOOLBAR_H - 1, WIN_W, 1, COL_DIVIDER);
    ox_draw_rect(g_win, SIDEBAR_W, TOOLBAR_H, 1, WIN_H - TOOLBAR_H, COL_DIVIDER);

    /* --- toolbar --- */
    /* Three nav buttons: back, forward, up. Use "<", ">", "^" glyphs. */
    int bx = 8, by = (TOOLBAR_H - BTN_H) / 2;
    draw_button(bx,                by, "<", g_btn_hover == 0, g_hist_n == 0);
    draw_button(bx + BTN_W + 4,    by, ">", g_btn_hover == 1, g_fwd_n == 0);
    draw_button(bx + (BTN_W + 4)*2, by, "^", g_btn_hover == 2, strcmp(g_cwd, "/") == 0);
    /* Path text (left-aligned after buttons). */
    int path_x = bx + (BTN_W + 4) * 3 + 12;
    int path_y = (TOOLBAR_H - 8) / 2;
    ox_draw_text(g_win, path_x, path_y, g_cwd, COL_TEXT);

    /* --- sidebar --- */
    ox_draw_text(g_win, 14, TOOLBAR_H + 12, "PLACES", COL_SECTION);
    for (int i = 0; i < SB_N; i++) {
        int y = TOOLBAR_H + 32 + i * 26;
        int active = (strcmp(g_cwd, g_sidebar[i].path) == 0);
        int hovered = (g_sb_hover == i);
        if (active || hovered) {
            ox_draw_rect(g_win, 8, y - 2, SIDEBAR_W - 16, ROW_H,
                         active ? COL_ACCENT : COL_HOVER);
        }
        ox_draw_text(g_win, 18, y + 3, g_sidebar[i].label,
                     active ? COL_TEXT : COL_TEXT);
    }

    /* --- file list --- */
    int list_x = SIDEBAR_W;
    int list_y = TOOLBAR_H;
    int list_w = WIN_W - SIDEBAR_W;
    int rpp = list_rows_per_page();
    for (int i = 0; i < rpp && g_scroll + i < g_n_entries; i++) {
        int idx = g_scroll + i;
        int y = list_y + i * ROW_H;
        /* row background */
        int is_sel = (idx == g_selected);
        int is_hov = (idx == g_hover);
        if (is_sel) {
            ox_draw_rect(g_win, list_x + 2, y + 1, list_w - 4, ROW_H - 2, COL_ACCENT);
        } else if (is_hov) {
            ox_draw_rect(g_win, list_x + 2, y + 1, list_w - 4, ROW_H - 2, COL_HOVER);
        }
        /* icon */
        int ix = list_x + 12;
        int iy = y + (ROW_H - ICON_H) / 2;
        if (g_entries[idx].is_dir) draw_folder_icon(ix, iy);
        else                       draw_file_icon(ix, iy - 1);
        /* name */
        int tx = ix + ICON_W + 8;
        int ty = y + (ROW_H - 8) / 2;
        ox_draw_text(g_win, tx, ty, g_entries[idx].name, COL_TEXT);
        /* size (only for files) — right-aligned at list right edge. */
        if (!g_entries[idx].is_dir) {
            char sz[24];
            format_size(g_entries[idx].size, sz, sizeof(sz));
            int sz_len = (int)strlen(sz);
            int sz_x = WIN_W - 16 - sz_len * 8;
            ox_draw_text(g_win, sz_x, ty, sz,
                         is_sel ? COL_TEXT : COL_TEXT_DIM);
        }
    }

    ox_present(g_win);
}

/* ---------------- file action -------------------------------------- */
static int has_ext(const char *name, const char *ext) {
    size_t nl = strlen(name), el = strlen(ext);
    if (nl < el) return 0;
    return strcmp(name + nl - el, ext) == 0;
}

static void set_wallpaper_via_oxrc(const char *name) {
    char base[64];
    size_t L = strlen(name);
    if (L >= sizeof(base)) L = sizeof(base) - 1;
    memcpy(base, name, L); base[L] = 0;
    if (L >= 4 && strcmp(base + L - 4, ".ppm") == 0) base[L - 4] = 0;
    int fd = open("/home/.oxrc", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "current_wallpaper=%s\n", base);
    write(fd, buf, n);
    close(fd);
    if (ipc_service_lookup(SERVER_OX) > 0) {
        ipc_msg_t m;
        memset(&m, 0, sizeof(m));
        m.to = SERVER_OX;
        m.type = IPC_OX_RELOAD_SETTINGS;
        ipc_send(&m);
    }
}

static void open_entry(int idx) {
    if (idx < 0 || idx >= g_n_entries) return;
    entry_t *e = &g_entries[idx];
    if (e->is_dir) {
        char next[256];
        if (strcmp(g_cwd, "/") == 0) {
            snprintf(next, sizeof(next), "/%s", e->name);
        } else {
            snprintf(next, sizeof(next), "%s/%s", g_cwd, e->name);
        }
        navigate_to(next);
        rescan(); render();
        return;
    }
    /* Wallpaper PPM in /home/wallpapers/ → set as current. */
    if (has_ext(e->name, ".ppm") && strstr(g_cwd, "wallpapers") != NULL) {
        set_wallpaper_via_oxrc(e->name);
        return;
    }
    /* Build absolute path. */
    char full[512];
    if (strcmp(g_cwd, "/") == 0) snprintf(full, sizeof(full), "/%s", e->name);
    else                         snprintf(full, sizeof(full), "%s/%s", g_cwd, e->name);
    static const char envp_flat[] =
        "PATH=/bin\0"
        "HOME=/home\0"
        "SHELL=/bin/sh\0"
        "TERM=osnos\0";
    /* Per-extension dispatch:
     *   .js  → /bin/oxjs    (JavaScript runner with Ox bindings)
     *   .db  → /bin/oxsqliteview
     *   .ppm → /bin/oxnotepad (or wallpaper handled above)
     *   else → /bin/oxnotepad (default text editor) */
    const char *opener = "/bin/oxnotepad";
    if (has_ext(e->name, ".js"))                opener = "/bin/oxjs";
    else if (has_ext(e->name, ".db"))           opener = "/bin/oxsqliteview";
    else if (has_ext(e->name, ".sqlite"))       opener = "/bin/oxsqliteview";
    ox_log("oxfiles: open %s → %s\n", full, opener);
    osn_spawn(opener, full, envp_flat, -1, -1);
}

/* ---------------- hit-testing -------------------------------------- */
static int hit_button(int x, int y) {
    if (y < (TOOLBAR_H - BTN_H) / 2 || y >= (TOOLBAR_H + BTN_H) / 2) return -1;
    int bx = 8;
    for (int i = 0; i < 3; i++) {
        int x0 = bx + i * (BTN_W + 4);
        if (x >= x0 && x < x0 + BTN_W) return i;
    }
    return -1;
}

static int hit_sidebar(int x, int y) {
    if (x < 8 || x >= SIDEBAR_W - 8) return -1;
    int y0 = TOOLBAR_H + 30;
    for (int i = 0; i < SB_N; i++) {
        int ry = y0 + i * 26;
        if (y >= ry && y < ry + ROW_H) return i;
    }
    return -1;
}

static int hit_row(int x, int y) {
    if (x < SIDEBAR_W || y < TOOLBAR_H) return -1;
    int row = (y - TOOLBAR_H) / ROW_H;
    int idx = g_scroll + row;
    if (idx < 0 || idx >= g_n_entries) return -1;
    return idx;
}

/* ---------------- main --------------------------------------------- */
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (ox_init() < 0) return 1;
    g_win = ox_window_create(WIN_W, WIN_H, "Files");
    if (g_win < 0) return 1;
    rescan();
    render();
    for (;;) {
        ox_event_t ev;
        if (!ox_wait_event(&ev)) continue;
        if (ev.type == OX_EV_CLOSE) break;

        if (ev.type == OX_EV_MOUSE) {
            int btn = hit_button(ev.x, ev.y);
            int sb  = hit_sidebar(ev.x, ev.y);
            int row = hit_row(ev.x, ev.y);

            if (ev.mouse_kind == OX_MOUSE_MOVE) {
                int dirty = 0;
                if (btn != g_btn_hover) { g_btn_hover = btn; dirty = 1; }
                if (sb  != g_sb_hover)  { g_sb_hover  = sb;  dirty = 1; }
                if (row != g_hover)     { g_hover     = row; dirty = 1; }
                if (dirty) render();
            } else if (ev.mouse_kind == OX_MOUSE_DOWN) {
                if (btn == 0) { go_back();    rescan(); render(); }
                else if (btn == 1) { go_forward(); rescan(); render(); }
                else if (btn == 2) { go_up();     rescan(); render(); }
                else if (sb >= 0) {
                    navigate_to(g_sidebar[sb].path);
                    rescan(); render();
                } else if (row >= 0) {
                    /* First click selects; double-click (treat as
                     * 2nd click on already-selected) opens. */
                    if (g_selected == row) {
                        open_entry(row);
                    } else {
                        g_selected = row;
                        render();
                    }
                } else {
                    /* Click anywhere else clears selection. */
                    if (g_selected >= 0) { g_selected = -1; render(); }
                }
            }
        } else if (ev.type == OX_EV_KEY) {
            if (ev.ascii == '\b' || ev.keycode == OX_KEY_BACKSPACE) {
                go_up(); rescan(); render();
            } else if (ev.ascii == '\r' || ev.ascii == '\n') {
                if (g_selected >= 0) open_entry(g_selected);
            } else if (ev.keycode == OX_KEY_DOWN) {
                if (g_n_entries > 0) {
                    g_selected = (g_selected + 1) % g_n_entries;
                    if (g_selected >= g_scroll + list_rows_per_page())
                        g_scroll = g_selected - list_rows_per_page() + 1;
                    if (g_selected < g_scroll) g_scroll = g_selected;
                    render();
                }
            } else if (ev.keycode == OX_KEY_UP) {
                if (g_n_entries > 0) {
                    if (g_selected <= 0) g_selected = g_n_entries - 1;
                    else                 g_selected--;
                    if (g_selected >= g_scroll + list_rows_per_page())
                        g_scroll = g_selected - list_rows_per_page() + 1;
                    if (g_selected < g_scroll) g_scroll = g_selected;
                    render();
                }
            }
        }
    }
    ox_window_destroy(g_win);
    return 0;
}
