/*
 * /bin/oxsettings — wallpaper picker for Ox.
 *
 * Lists the available wallpapers in /home/wallpapers/ (any .ppm).
 * Click to pick; "Apply" writes /home/.oxrc and tells oxsrv to
 * reload via IPC_OX_RELOAD_SETTINGS.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <osnos_ipc.h>
#include <ox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WIN_W 400
#define WIN_H 300

#define MAX_WALLS 8
static char     g_walls[MAX_WALLS][32];
static int      g_n_walls = 0;
static int      g_selected = 0;

static ox_win_t g_win;

static void scan_walls(void) {
    DIR *d = opendir("/home/wallpapers");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_n_walls < MAX_WALLS) {
        const char *name = e->d_name;
        if (name[0] == '.') continue;
        size_t L = strlen(name);
        if (L < 5) continue;
        if (strcmp(name + L - 4, ".ppm") != 0) continue;
        size_t base = L - 4;
        if (base >= sizeof(g_walls[0])) base = sizeof(g_walls[0]) - 1;
        memcpy(g_walls[g_n_walls], name, base);
        g_walls[g_n_walls][base] = 0;
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
        if (strcmp(g_walls[i], p) == 0) { g_selected = i; return; }
    }
}

static void write_current(void) {
    int fd = open("/home/.oxrc", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "current_wallpaper=%s\n", g_walls[g_selected]);
    write(fd, buf, n);
    close(fd);
}

static void notify_oxsrv(void) {
    if (ipc_service_lookup(SERVER_OX) <= 0) return;
    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.to   = SERVER_OX;
    m.type = IPC_OX_RELOAD_SETTINGS;
    ipc_send(&m);
}

static int radio_y(int i) { return 50 + i * 32; }
static int apply_x(void)  { return WIN_W - 100; }
static int apply_y(void)  { return WIN_H - 50; }

static void render(void) {
    ox_draw_rect(g_win, 0, 0, WIN_W, WIN_H, OX_RGB(245, 245, 240));
    ox_draw_text(g_win, 16, 16, "Wallpaper", OX_RGB(20, 20, 60));
    ox_draw_rect(g_win, 16, 32, WIN_W - 32, 1, OX_RGB(180, 180, 180));
    for (int i = 0; i < g_n_walls; i++) {
        int y = radio_y(i);
        /* Radio circle (square in our font's world). */
        ox_draw_rect(g_win, 24, y, 14, 14, OX_RGB(200, 200, 200));
        ox_draw_rect(g_win, 26, y + 2, 10, 10, OX_RGB(255, 255, 255));
        if (i == g_selected) {
            ox_draw_rect(g_win, 28, y + 4, 6, 6, OX_RGB(40, 100, 200));
        }
        ox_draw_text(g_win, 50, y + 3, g_walls[i], OX_RGB(20, 20, 40));
    }
    if (g_n_walls == 0) {
        ox_draw_text(g_win, 24, 60,
                      "no wallpapers found in /home/wallpapers/",
                      OX_RGB(120, 40, 40));
    }
    /* Apply button. */
    ox_draw_rect(g_win, apply_x(), apply_y(), 80, 28, OX_RGB(40, 120, 60));
    ox_draw_text(g_win, apply_x() + 24, apply_y() + 10, "Apply",
                  OX_RGB(255, 255, 255));
    ox_present(g_win);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (ox_init() < 0) return 1;
    g_win = ox_window_create(WIN_W, WIN_H, "Settings");
    if (g_win < 0) return 1;
    scan_walls();
    read_current();
    render();
    for (;;) {
        ox_event_t ev;
        if (!ox_wait_event(&ev)) continue;
        if (ev.type == OX_EV_CLOSE) break;
        if (ev.type == OX_EV_MOUSE && ev.mouse_kind == OX_MOUSE_DOWN) {
            /* Apply button hit? */
            if (ev.x >= apply_x() && ev.x < apply_x() + 80 &&
                ev.y >= apply_y() && ev.y < apply_y() + 28) {
                if (g_n_walls > 0) {
                    write_current();
                    notify_oxsrv();
                }
                render();
                continue;
            }
            /* Radio rows. */
            for (int i = 0; i < g_n_walls; i++) {
                int y = radio_y(i);
                if (ev.y >= y && ev.y < y + 14 && ev.x >= 24 && ev.x < WIN_W - 24) {
                    g_selected = i;
                    render();
                    break;
                }
            }
        }
    }
    ox_window_destroy(g_win);
    return 0;
}
