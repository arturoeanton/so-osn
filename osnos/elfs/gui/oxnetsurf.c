/*
 * /bin/oxnetsurf — placeholder for the in-progress NetSurf port.
 *
 * Status: GROUNDWORK ONLY. The seven core NetSurf libraries are
 * vendored under vendor/netsurf/ but no porting work has happened
 * yet. This stub exists so:
 *
 *   1. The Ox launcher menu has a real ELF to dispatch to.
 *   2. The makefile's OXNETSURF_ENABLED=1 path has a final target
 *      to link.
 *   3. Anyone running it before the port lands gets a clear status
 *      message instead of "exec failed".
 *
 * See vendor/netsurf/PORTING_PLAN.md for the staged plan. The lib
 * archives (libwapcaplet.a, libhubbub.a, libdom.a, libcss.a, …) are
 * NOT built by `make` — they're gated on OXNETSURF_ENABLED.
 *
 * Built against musl (vendor/musl) because mini-libc lacks iconv,
 * regex.h, scandir, fnmatch, glob.h, flockfile, full wchar — all of
 * which NetSurf's core libs use. See "porting blockers" in the plan.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ox.h>

#define WIN_W 560
#define WIN_H 320

#define COL_BG       OX_RGB(245, 245, 240)
#define COL_HEADER   OX_RGB( 53, 132, 228)
#define COL_FG       OX_RGB( 20,  20,  25)
#define COL_DIM      OX_RGB(120, 120, 130)

static const char *kLines[] = {
    "NetSurf port — pending implementation.",
    "",
    "Vendored libs:  vendor/netsurf/",
    "Porting plan:   vendor/netsurf/PORTING_PLAN.md",
    "",
    "Status: groundwork only. Source trees are in place",
    "for libwapcaplet, libparserutils, libhubbub, libdom,",
    "libcss, libnsutils, libnslog, plus netsurf-core 3.11",
    "with the framebuffer frontend.",
    "",
    "To attempt a build (will fail until Stage 1 lands):",
    "    make OXNETSURF_ENABLED=1 build/elfs/gui/oxnetsurf.elf",
    "",
    "Use /bin/oxbrowser for HTTP+TLS browsing today.",
    NULL,
};

/* TODO(stage 1): replace this with libdom + libhubbub bootstrap.
 * TODO(stage 2): pull in libcss + selector engine.
 * TODO(stage 3): netsurf-core/desktop + an Ox plot table.
 * TODO(stage 4): URL bar + click-to-navigate (lift from oxbrowser.c).
 * TODO(stage 5): wire BearSSL fetcher (share with oxbrowser).        */

int main(int argc, char **argv) {
    (void) argc; (void) argv;

    if (ox_init() != 0) {
        fprintf(stderr, "oxnetsurf: cannot reach oxsrv (ox_init failed)\n");
        return 1;
    }

    ox_win_t w = ox_window_create(WIN_W, WIN_H, "NetSurf (port pending)");
    if (w <= 0) {
        fprintf(stderr, "oxnetsurf: ox_window_create failed\n");
        return 1;
    }

    /* Initial paint. */
    ox_draw_rect(w, 0, 0, WIN_W, WIN_H, COL_BG);
    ox_draw_rect(w, 0, 0, WIN_W, 28, COL_HEADER);
    ox_draw_text(w, 12, 10, "oxnetsurf", OX_RGB(255, 255, 255));

    int y = 50;
    for (int i = 0; kLines[i] != NULL; ++i) {
        uint32_t col = (i == 0) ? COL_FG : COL_DIM;
        if (kLines[i][0] != '\0') {
            ox_draw_text(w, 14, y, kLines[i], col);
        }
        y += 16;
    }
    ox_present(w);

    /* Event loop: just close on ESC / window-close. */
    for (;;) {
        ox_event_t ev;
        if (ox_wait_event(&ev) != 1) continue;
        if (ev.type == OX_EV_CLOSE) break;
        if (ev.type == OX_EV_KEY && ev.keycode == OX_KEY_ESC) break;
        if (ev.type == OX_EV_EXPOSE || ev.type == OX_EV_RESIZE) {
            /* TODO(stage 3): re-render against new dims. For now,
             * just repaint the static text. */
            int ww = WIN_W, hh = WIN_H;
            ox_window_dims(w, &ww, &hh);
            ox_draw_rect(w, 0, 0, ww, hh, COL_BG);
            ox_draw_rect(w, 0, 0, ww, 28, COL_HEADER);
            ox_draw_text(w, 12, 10, "oxnetsurf", OX_RGB(255, 255, 255));
            int yy = 50;
            for (int i = 0; kLines[i] != NULL; ++i) {
                uint32_t col = (i == 0) ? COL_FG : COL_DIM;
                if (kLines[i][0] != '\0')
                    ox_draw_text(w, 14, yy, kLines[i], col);
                yy += 16;
            }
            ox_present(w);
        }
    }
    ox_window_destroy(w);
    return 0;
}
