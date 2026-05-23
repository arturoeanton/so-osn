/*
 * /bin/oxfiles — minimal file browser for Ox.
 *
 * Single-window list of entries in a directory. Click ".." to go up.
 * Click a subdirectory to enter it. Click a regular file to open
 * it (heuristic: .ppm files relayed to oxsrv as wallpaper, anything
 * else spawned via /bin/oxnotepad if it looks textual).
 *
 * Keys:
 *   Up/Down  — navigate (currently scroll via click only — V1)
 *   Enter    — open highlighted entry
 *   Backspace / left — parent dir
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

extern char **environ;

#define WIN_W 520
#define WIN_H 480
#define HEADER_H 28
#define ROW_H 18
#define MAX_ENTRIES 64

static ox_win_t g_win;
static char     g_cwd[256] = "/home";

typedef struct {
    char name[64];
    int  is_dir;
} entry_t;

static entry_t g_entries[MAX_ENTRIES];
static int     g_n_entries = 0;
static int     g_scroll = 0;          /* first visible entry index */
static int     g_hover = -1;

static int rows_per_page(void) {
    return (WIN_H - HEADER_H) / ROW_H;
}

static void rescan(void) {
    g_n_entries = 0;
    g_scroll = 0;
    g_hover = -1;
    /* Always offer ".." unless at "/". */
    if (strcmp(g_cwd, "/") != 0) {
        strcpy(g_entries[g_n_entries].name, "..");
        g_entries[g_n_entries].is_dir = 1;
        g_n_entries++;
    }
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
        /* Stat to learn type. */
        char path[512];
        snprintf(path, sizeof(path), "%s/%s",
                 strcmp(g_cwd, "/") == 0 ? "" : g_cwd, e->d_name);
        struct stat st;
        g_entries[g_n_entries].is_dir = 0;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            g_entries[g_n_entries].is_dir = 1;
        }
        g_n_entries++;
    }
    closedir(d);
}

static void render(void) {
    /* Background. */
    ox_draw_rect(g_win, 0, 0, WIN_W, WIN_H, OX_RGB(245, 245, 240));
    /* Header bar with cwd. */
    ox_draw_rect(g_win, 0, 0, WIN_W, HEADER_H, OX_RGB(70, 100, 160));
    char header[256];
    snprintf(header, sizeof(header), " %s   (%d entries)", g_cwd, g_n_entries);
    ox_draw_text(g_win, 8, 10, header, OX_RGB(255, 255, 255));
    /* Rows. */
    int rpp = rows_per_page();
    for (int i = 0; i < rpp && g_scroll + i < g_n_entries; i++) {
        int idx = g_scroll + i;
        int y = HEADER_H + i * ROW_H;
        uint32_t bg = (idx == g_hover) ? OX_RGB(200, 220, 255)
                                       : OX_RGB(255, 255, 255);
        ox_draw_rect(g_win, 2, y, WIN_W - 4, ROW_H, bg);
        const char *prefix = g_entries[idx].is_dir ? "[ ] " : "    ";
        char line[80];
        snprintf(line, sizeof(line), "%s%s%s",
                 prefix, g_entries[idx].name,
                 g_entries[idx].is_dir ? "/" : "");
        uint32_t fg = g_entries[idx].is_dir ? OX_RGB(40, 80, 160)
                                            : OX_RGB(20, 20, 30);
        ox_draw_text(g_win, 6, y + 5, line, fg);
    }
    ox_present(g_win);
}

static void set_wallpaper_via_oxrc(const char *name) {
    /* Strip trailing ".ppm". */
    char base[64];
    size_t L = strlen(name);
    if (L >= sizeof(base)) L = sizeof(base) - 1;
    memcpy(base, name, L);
    base[L] = 0;
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
        m.to   = SERVER_OX;
        m.type = IPC_OX_RELOAD_SETTINGS;
        ipc_send(&m);
    }
}

static int has_ext(const char *name, const char *ext) {
    size_t nl = strlen(name), el = strlen(ext);
    if (nl < el) return 0;
    return strcmp(name + nl - el, ext) == 0;
}

static void open_entry(int idx) {
    if (idx < 0 || idx >= g_n_entries) return;
    entry_t *e = &g_entries[idx];
    if (e->is_dir) {
        /* Compute new cwd. */
        if (strcmp(e->name, "..") == 0) {
            char *slash = strrchr(g_cwd, '/');
            if (slash && slash != g_cwd) *slash = 0;
            else strcpy(g_cwd, "/");
        } else {
            size_t cl = strlen(g_cwd);
            if (strcmp(g_cwd, "/") == 0) {
                snprintf(g_cwd + cl, sizeof(g_cwd) - cl, "%s", e->name);
            } else {
                snprintf(g_cwd + cl, sizeof(g_cwd) - cl, "/%s", e->name);
            }
        }
        rescan();
        render();
        return;
    }
    /* Wallpaper PPM → set as current. */
    if (has_ext(e->name, ".ppm")) {
        char dir[256];
        strncpy(dir, g_cwd, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = 0;
        /* Only treat as wallpaper if path is /home/wallpapers/. */
        if (strstr(dir, "wallpapers") != NULL) {
            set_wallpaper_via_oxrc(e->name);
            return;
        }
    }
    /* Default: open in /bin/oxnotepad (it'll see if the path is a
     * real file; if not it shows empty). */
    /* TODO: a real "open notepad with this file" requires notepad to
     * accept a path argv. For V1 we just spawn notepad, user can
     * re-open the project file there. */
    static const char envp_flat[] =
        "PATH=/bin\0"
        "HOME=/home\0"
        "SHELL=/bin/uxsh\0"
        "TERM=osnos\0";
    osn_spawn("/bin/oxnotepad", "", envp_flat, -1, -1);
}

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
            if (ev.y < HEADER_H) continue;
            int row = (ev.y - HEADER_H) / ROW_H;
            int idx = g_scroll + row;
            if (idx < 0 || idx >= g_n_entries) {
                if (g_hover != -1) { g_hover = -1; render(); }
                continue;
            }
            if (ev.mouse_kind == OX_MOUSE_MOVE) {
                if (g_hover != idx) { g_hover = idx; render(); }
            } else if (ev.mouse_kind == OX_MOUSE_DOWN) {
                open_entry(idx);
            }
        } else if (ev.type == OX_EV_KEY) {
            if (ev.ascii == '\b' || ev.keycode == OX_KEY_BACKSPACE) {
                /* Up one dir. */
                if (strcmp(g_cwd, "/") != 0) {
                    char *slash = strrchr(g_cwd, '/');
                    if (slash && slash != g_cwd) *slash = 0;
                    else strcpy(g_cwd, "/");
                    rescan();
                    render();
                }
            } else if (ev.ascii == '\r' || ev.ascii == '\n') {
                if (g_hover >= 0) open_entry(g_hover);
            }
        }
    }
    ox_window_destroy(g_win);
    return 0;
}
