/*
 * ox_ui.c — implementation of the Ox mini-widget toolkit.
 *
 * Each widget renders via ox_draw_rect / ox_draw_text from <ox.h>.
 * Drawing into the per-window backing buffer is the same cheap path
 * apps already use. No retained state outside the widget structs.
 */

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <dirent.h>

#include "include/ox.h"
#include "include/ox_ui.h"

/* ---- Premium text + bevel helpers (FASE 15.2) ---------------------
 *
 * All widget chrome routes through these. Text is the proportional
 * TTF when /home/.fonts/default.ttf is staged (ox_draw_text_pretty
 * lazy-loads it), bevels follow the BeOS R5 vocabulary: raised =
 * light top/left + dark bottom/right, sunken wells invert it, and
 * faces get a subtle 2-stop vertical gradient so nothing reads as a
 * flat indie rectangle. */

static void ui_font(void) {
    static int tried;
    if (!tried && !ox_text_loaded()) {
        tried = 1;
        ox_text_init("/home/.fonts/default.ttf", 14);
    }
}

static int ui_text_w(const char *s) {
    ui_font();
    return s ? ox_text_width(s) : 0;
}

static int ui_text_h(void) {
    ui_font();
    return ox_text_height();
}

static void ui_text(ox_win_t win, int x, int y, const char *s,
                    uint32_t color) {
    ui_font();
    ox_draw_text_pretty(win, x, y, s, color);
}

static uint32_t ui_lerp(uint32_t a, uint32_t b, int num, int den) {
    if (den <= 0) return a;
    int ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
    int br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
    return OX_RGB(ar + (br - ar) * num / den,
                  ag + (bg - ag) * num / den,
                  ab + (bb - ab) * num / den);
}

/* Vertical 2-stop gradient fill. */
static void ui_vgrad(ox_win_t win, int x, int y, int w, int h,
                     uint32_t top, uint32_t bot) {
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < h; i++)
        ox_draw_rect(win, x, y + i, w, 1, ui_lerp(top, bot, i, h - 1));
}

/* Raised bevel: 1px outline + light top/left, dark bottom/right. */
static void ui_bevel_raised(ox_win_t win, int x, int y, int w, int h) {
    ox_draw_rect(win, x,         y,         w, 1, OX_UI_COL_BORDER);
    ox_draw_rect(win, x,         y + h - 1, w, 1, OX_UI_COL_BORDER);
    ox_draw_rect(win, x,         y,         1, h, OX_UI_COL_BORDER);
    ox_draw_rect(win, x + w - 1, y,         1, h, OX_UI_COL_BORDER);
    ox_draw_rect(win, x + 1,     y + 1,     w - 2, 1, OX_UI_COL_BEVEL_LITE);
    ox_draw_rect(win, x + 1,     y + 1,     1, h - 2, OX_UI_COL_BEVEL_LITE);
    ox_draw_rect(win, x + 1,     y + h - 2, w - 2, 1, OX_UI_COL_BEVEL_DARK);
    ox_draw_rect(win, x + w - 2, y + 1,     1, h - 2, OX_UI_COL_BEVEL_DARK);
}

/* Sunken well: dark top/left, light bottom/right (lists, tracks). */
static void ui_bevel_sunken(ox_win_t win, int x, int y, int w, int h) {
    ox_draw_rect(win, x,         y,         w, 1, OX_UI_COL_WELL_DARK);
    ox_draw_rect(win, x,         y,         1, h, OX_UI_COL_WELL_DARK);
    ox_draw_rect(win, x,         y + h - 1, w, 1, OX_UI_COL_WELL_LITE);
    ox_draw_rect(win, x + w - 1, y,         1, h, OX_UI_COL_WELL_LITE);
}

/* ---- Button ------------------------------------------------------- */

void ox_button_draw(ox_win_t win, const ox_button_t *b) {
    if (!b) return;
    if (b->pressed) {
        /* Sunken: flat darker face, inverted bevel, label nudged. */
        ox_draw_rect(win, b->x + 1, b->y + 1, b->w - 2, b->h - 2,
                     OX_UI_COL_BTN_DOWN);
        ox_draw_rect(win, b->x,             b->y,            b->w, 1, OX_UI_COL_BORDER);
        ox_draw_rect(win, b->x,             b->y + b->h - 1, b->w, 1, OX_UI_COL_BORDER);
        ox_draw_rect(win, b->x,             b->y,            1, b->h, OX_UI_COL_BORDER);
        ox_draw_rect(win, b->x + b->w - 1,  b->y,            1, b->h, OX_UI_COL_BORDER);
        ox_draw_rect(win, b->x + 1, b->y + 1, b->w - 2, 1, OX_UI_COL_BEVEL_DARK);
        ox_draw_rect(win, b->x + 1, b->y + 1, 1, b->h - 2, OX_UI_COL_BEVEL_DARK);
    } else {
        uint32_t top = b->hover ? OX_UI_COL_BTN_TOP_HOT : OX_UI_COL_BTN_TOP;
        uint32_t bot = b->hover ? OX_UI_COL_BTN_BOT_HOT : OX_UI_COL_BTN_BOT;
        ui_vgrad(win, b->x + 2, b->y + 2, b->w - 4, b->h - 4, top, bot);
        ui_bevel_raised(win, b->x, b->y, b->w, b->h);
    }
    if (b->label) {
        int tw = ui_text_w(b->label);
        int th = ui_text_h();
        int tx = b->x + (b->w - tw) / 2 + (b->pressed ? 1 : 0);
        int ty = b->y + (b->h - th) / 2 + (b->pressed ? 1 : 0);
        ui_text(win, tx, ty, b->label, OX_UI_COL_FG);
    }
}

int ox_button_hit(const ox_button_t *b, int mx, int my) {
    if (!b) return 0;
    return mx >= b->x && mx < b->x + b->w &&
           my >= b->y && my < b->y + b->h;
}

/* ---- Label -------------------------------------------------------- */

void ox_label_draw(ox_win_t win, const ox_label_t *l) {
    if (!l) return;
    if (l->bg) ox_draw_rect(win, l->x, l->y, l->w, l->h, l->bg);
    if (!l->text) return;
    uint32_t fg = l->fg ? l->fg : OX_UI_COL_FG;
    int tw = ui_text_w(l->text);
    int tx = l->x;
    if (l->align == OX_ALIGN_CENTER)      tx = l->x + (l->w - tw) / 2;
    else if (l->align == OX_ALIGN_RIGHT)  tx = l->x + l->w - tw - 4;
    else                                   tx = l->x + 4;
    int ty = l->y + (l->h - ui_text_h()) / 2;
    ui_text(win, tx, ty, l->text, fg);
}

/* ---- ListView ----------------------------------------------------- */

static int lv_item_h(const ox_listview_t *lv) {
    return lv->item_h > 0 ? lv->item_h : 18;
}

static int lv_visible_rows(const ox_listview_t *lv) {
    int ih = lv_item_h(lv);
    return ih > 0 ? lv->h / ih : 0;
}

void ox_listview_draw(ox_win_t win, const ox_listview_t *lv) {
    if (!lv) return;
    ox_draw_rect(win, lv->x, lv->y, lv->w, lv->h, OX_UI_COL_BG_ALT);
    /* Sunken well — lists read as recessed content in R5. */
    ui_bevel_sunken(win, lv->x, lv->y, lv->w, lv->h);
    int ih = lv_item_h(lv);
    int vis = lv_visible_rows(lv);
    int th = ui_text_h();
    for (int i = 0; i < vis; i++) {
        int idx = lv->scroll + i;
        if (idx < 0 || idx >= lv->n_items) break;
        int row_y = lv->y + 1 + i * ih;
        if (row_y + ih > lv->y + lv->h - 1) break;
        uint32_t fg = OX_UI_COL_FG;
        if (idx == lv->sel) {
            ui_vgrad(win, lv->x + 1, row_y, lv->w - 2, ih,
                     OX_UI_COL_HI_TOP, OX_UI_COL_HI_BOT);
            fg = OX_UI_COL_HI_FG;
        } else if (idx == lv->hover) {
            ox_draw_rect(win, lv->x + 1, row_y, lv->w - 2, ih,
                         OX_UI_COL_HOVER_ROW);
        } else {
            ox_draw_rect(win, lv->x + 1, row_y, lv->w - 2, ih,
                         OX_UI_COL_BG_ALT);
        }
        const char *s = lv->items[idx];
        if (s) ui_text(win, lv->x + 7, row_y + (ih - th) / 2, s, fg);
    }
}

int ox_listview_hit(const ox_listview_t *lv, int mx, int my) {
    if (!lv) return -1;
    if (mx < lv->x || mx >= lv->x + lv->w) return -1;
    if (my < lv->y || my >= lv->y + lv->h) return -1;
    int ih = lv_item_h(lv);
    if (ih <= 0) return -1;
    int row = (my - lv->y) / ih;
    int idx = lv->scroll + row;
    if (idx < 0 || idx >= lv->n_items) return -1;
    return idx;
}

int ox_listview_event(ox_listview_t *lv, const ox_event_t *ev) {
    if (!lv || !ev) return 0;
    if (ev->type == OX_EV_MOUSE) {
        if (ev->mouse_kind == OX_MOUSE_DOWN && (ev->buttons & 0x01)) {
            int hit = ox_listview_hit(lv, ev->x, ev->y);
            if (hit >= 0) {
                if (lv->sel != hit) { lv->sel = hit; return 1; }
            }
            return 0;
        }
        if (ev->mouse_kind == OX_MOUSE_MOVE) {
            int hit = ox_listview_hit(lv, ev->x, ev->y);
            if (hit != lv->hover) { lv->hover = hit; return 1; }
            return 0;
        }
        if (ev->mouse_kind == OX_MOUSE_WHEEL) {
            if (ev->x < lv->x || ev->x >= lv->x + lv->w) return 0;
            if (ev->y < lv->y || ev->y >= lv->y + lv->h) return 0;
            int vis = lv_visible_rows(lv);
            int max = lv->n_items - vis;
            if (max < 0) max = 0;
            lv->scroll -= ev->wheel_delta * 3;
            if (lv->scroll < 0)   lv->scroll = 0;
            if (lv->scroll > max) lv->scroll = max;
            return 1;
        }
    }
    if (ev->type == OX_EV_KEY) {
        int vis = lv_visible_rows(lv);
        int max = lv->n_items - vis;
        if (max < 0) max = 0;
        if (ev->keycode == OX_KEY_UP   && lv->sel > 0)               { lv->sel--; if (lv->sel < lv->scroll) lv->scroll = lv->sel; return 1; }
        if (ev->keycode == OX_KEY_DOWN && lv->sel + 1 < lv->n_items) { lv->sel++; if (lv->sel >= lv->scroll + vis) lv->scroll = lv->sel - vis + 1; return 1; }
        if (ev->keycode == OX_KEY_HOME) { lv->sel = 0; lv->scroll = 0; return 1; }
        if (ev->keycode == OX_KEY_END)  { lv->sel = lv->n_items - 1; lv->scroll = max; return 1; }
    }
    return 0;
}

/* ---- ScrollView --------------------------------------------------- */

static int sv_bar_w(const ox_scrollview_t *sv) {
    return sv->bar_w > 0 ? sv->bar_w : 12;
}

static int sv_thumb_geom(const ox_scrollview_t *sv, int *out_y, int *out_h) {
    int bar_h = sv->h;
    if (sv->content_h <= sv->h || sv->content_h <= 0) {
        if (out_y) *out_y = sv->y;
        if (out_h) *out_h = bar_h;
        return 0;
    }
    int th = (sv->h * sv->h) / sv->content_h;
    if (th < 24) th = 24;
    if (th > bar_h) th = bar_h;
    int max_scroll = sv->content_h - sv->h;
    int max_top = bar_h - th;
    int ty = (max_scroll > 0 ? (sv->scroll_y * max_top) / max_scroll : 0);
    if (out_y) *out_y = sv->y + ty;
    if (out_h) *out_h = th;
    return 1;   /* bar visible */
}

void ox_scrollview_draw_bg(ox_win_t win, const ox_scrollview_t *sv) {
    if (!sv) return;
    int bw = sv_bar_w(sv);
    ox_draw_rect(win, sv->x, sv->y, sv->w - bw, sv->h, OX_UI_COL_BG_ALT);
}

void ox_scrollview_draw_bar(ox_win_t win, const ox_scrollview_t *sv) {
    if (!sv) return;
    int bw = sv_bar_w(sv);
    int bar_x = sv->x + sv->w - bw;
    /* Sunken track. */
    ox_draw_rect(win, bar_x, sv->y, bw, sv->h, OX_UI_COL_SCROLL_TR);
    ui_bevel_sunken(win, bar_x, sv->y, bw, sv->h);
    int ty, th;
    if (!sv_thumb_geom(sv, &ty, &th)) return;
    /* Raised thumb with BeOS grip lines. */
    int thx = bar_x + 2, thw = bw - 4;
    if (ty < sv->y + 2) ty = sv->y + 2;
    if (ty + th > sv->y + sv->h - 2) th = sv->y + sv->h - 2 - ty;
    if (th < 8) th = 8;
    uint32_t face = sv->drag_active ? OX_RGB(196, 212, 232)
                                    : OX_RGB(222, 222, 222);
    ox_draw_rect(win, thx, ty, thw, th, face);
    ox_draw_rect(win, thx,           ty,          thw, 1, OX_UI_COL_BEVEL_LITE);
    ox_draw_rect(win, thx,           ty,          1, th,  OX_UI_COL_BEVEL_LITE);
    ox_draw_rect(win, thx,           ty + th - 1, thw, 1, OX_UI_COL_BEVEL_DARK);
    ox_draw_rect(win, thx + thw - 1, ty,          1, th,  OX_UI_COL_BEVEL_DARK);
    if (th >= 18 && thw >= 6) {
        int gy = ty + th / 2 - 3;
        for (int g = 0; g < 3; g++) {
            ox_draw_rect(win, thx + 2, gy + g * 3,     thw - 4, 1,
                         OX_UI_COL_BEVEL_DARK);
            ox_draw_rect(win, thx + 2, gy + g * 3 + 1, thw - 4, 1,
                         OX_UI_COL_BEVEL_LITE);
        }
    }
}

void ox_scrollview_clamp(ox_scrollview_t *sv) {
    if (!sv) return;
    int max = sv->content_h - sv->h;
    if (max < 0) max = 0;
    if (sv->scroll_y < 0)  sv->scroll_y = 0;
    if (sv->scroll_y > max) sv->scroll_y = max;
}

int ox_scrollview_event(ox_scrollview_t *sv, const ox_event_t *ev) {
    if (!sv || !ev) return 0;
    int bw = sv_bar_w(sv);
    int bar_x = sv->x + sv->w - bw;

    if (ev->type == OX_EV_MOUSE) {
        if (ev->mouse_kind == OX_MOUSE_WHEEL) {
            if (ev->x < sv->x || ev->x >= sv->x + sv->w) return 0;
            if (ev->y < sv->y || ev->y >= sv->y + sv->h) return 0;
            int step = sv->wheel_step > 0 ? sv->wheel_step : 30;
            sv->scroll_y -= ev->wheel_delta * step;
            ox_scrollview_clamp(sv);
            return 1;
        }
        if (ev->mouse_kind == OX_MOUSE_DOWN && (ev->buttons & 0x01)) {
            /* Click on scrollbar starts drag. */
            if (ev->x >= bar_x && ev->x < bar_x + bw &&
                ev->y >= sv->y && ev->y < sv->y + sv->h) {
                int ty, th;
                if (sv_thumb_geom(sv, &ty, &th)) {
                    if (ev->y >= ty && ev->y < ty + th) {
                        sv->drag_active   = 1;
                        sv->drag_offset_y = ev->y - ty;
                    } else {
                        /* Click outside thumb: page up/down. */
                        sv->scroll_y += (ev->y < ty ? -sv->h : sv->h);
                        ox_scrollview_clamp(sv);
                    }
                }
                return 1;
            }
        }
        if (ev->mouse_kind == OX_MOUSE_UP) {
            if (sv->drag_active) { sv->drag_active = 0; return 1; }
        }
        if (ev->mouse_kind == OX_MOUSE_MOVE && sv->drag_active) {
            int ty_target = ev->y - sv->drag_offset_y - sv->y;
            int ty, th;
            sv_thumb_geom(sv, &ty, &th);
            int max_top = sv->h - th;
            if (max_top <= 0) return 1;
            if (ty_target < 0) ty_target = 0;
            if (ty_target > max_top) ty_target = max_top;
            int max_scroll = sv->content_h - sv->h;
            sv->scroll_y = (ty_target * max_scroll) / max_top;
            ox_scrollview_clamp(sv);
            return 1;
        }
    }
    if (ev->type == OX_EV_KEY) {
        int line = 30;
        int step = sv->h - 20;
        if (step < line) step = line;
        if (ev->keycode == OX_KEY_UP)   { sv->scroll_y -= line; ox_scrollview_clamp(sv); return 1; }
        if (ev->keycode == OX_KEY_DOWN) { sv->scroll_y += line; ox_scrollview_clamp(sv); return 1; }
        if (ev->keycode == OX_KEY_PGUP) { sv->scroll_y -= step; ox_scrollview_clamp(sv); return 1; }
        if (ev->keycode == OX_KEY_PGDN) { sv->scroll_y += step; ox_scrollview_clamp(sv); return 1; }
        if (ev->keycode == OX_KEY_HOME) { sv->scroll_y = 0; return 1; }
        if (ev->keycode == OX_KEY_END)  { sv->scroll_y = sv->content_h - sv->h; ox_scrollview_clamp(sv); return 1; }
    }
    return 0;
}

/* ---- Dialogs ------------------------------------------------------ */

#define COL_DLG_BACKDROP     OX_RGB(  0,   0,   0)   /* drawn at ~40% alpha approximation by lighter fill */
#define COL_DLG_BG           OX_RGB(248, 248, 248)
#define COL_DLG_TITLE_BG     OX_RGB(102, 152, 203)
#define COL_DLG_TITLE_FG     OX_RGB(255, 255, 255)
#define COL_DLG_BORDER       OX_RGB( 60,  60,  60)

static void dlg_frame(ox_win_t win, int x, int y, int w, int h, const char *title) {
    /* Body + raised bevel so the dialog visibly floats over the
     * caller's content (cheap modal hint, no alpha pass). */
    ox_draw_rect(win, x + 2, y + 2, w - 4, h - 4, COL_DLG_BG);
    ox_draw_rect(win, x,         y,         w, 1, COL_DLG_BORDER);
    ox_draw_rect(win, x,         y + h - 1, w, 1, COL_DLG_BORDER);
    ox_draw_rect(win, x,         y,         1, h, COL_DLG_BORDER);
    ox_draw_rect(win, x + w - 1, y,         1, h, COL_DLG_BORDER);
    ox_draw_rect(win, x + 1,     y + 1,     w - 2, 1, OX_UI_COL_BEVEL_LITE);
    ox_draw_rect(win, x + 1,     y + 1,     1, h - 2, OX_UI_COL_BEVEL_LITE);
    ox_draw_rect(win, x + 1,     y + h - 2, w - 2, 1, OX_UI_COL_BEVEL_DARK);
    ox_draw_rect(win, x + w - 2, y + 1,     1, h - 2, OX_UI_COL_BEVEL_DARK);
    /* Title bar — gradient blue, proportional text. */
    int th_bar = 20;
    ui_vgrad(win, x + 2, y + 2, w - 4, th_bar,
             OX_UI_COL_HI_TOP, OX_UI_COL_HI_BOT);
    if (title) {
        int ty = y + 2 + (th_bar - ui_text_h()) / 2;
        ui_text(win, x + 10, ty, title, COL_DLG_TITLE_FG);
    }
}

/* ---- Message box -------------------------------------------------- */

void ox_msgbox_open(ox_msgbox_t *m, int sx, int sy, int sw, int sh,
                     const char *title, const char *msg) {
    if (!m) return;
    int w = 380, h = 140;
    m->x = sx + (sw - w) / 2;
    m->y = sy + (sh - h) / 2;
    m->w = w;
    m->h = h;
    m->title  = title ? title : "Message";
    m->msg    = msg   ? msg   : "";
    m->result = OX_DLG_OPEN;
    m->btn_ok.x = m->x + w - 90;
    m->btn_ok.y = m->y + h - 36;
    m->btn_ok.w = 78;
    m->btn_ok.h = 26;
    m->btn_ok.label = "OK";
    m->btn_ok.hover = 0;
    m->btn_ok.pressed = 0;
}

void ox_msgbox_draw(ox_win_t win, const ox_msgbox_t *m) {
    if (!m) return;
    dlg_frame(win, m->x, m->y, m->w, m->h, m->title);
    /* Wrap the message naively across lines using '\n'. */
    if (m->msg) {
        const char *p = m->msg;
        int line_y = m->y + 34;
        int lh = ox_text_line_height();
        char buf[256];
        int n = 0;
        while (*p) {
            if (*p == '\n' || n >= (int)sizeof(buf) - 1) {
                buf[n] = 0;
                ui_text(win, m->x + 12, line_y, buf, OX_UI_COL_FG);
                line_y += lh;
                n = 0;
                if (*p == '\n') p++;
                continue;
            }
            buf[n++] = *p++;
        }
        if (n > 0) {
            buf[n] = 0;
            ui_text(win, m->x + 12, line_y, buf, OX_UI_COL_FG);
        }
    }
    ox_button_draw(win, &m->btn_ok);
}

void ox_msgbox_event(ox_msgbox_t *m, const ox_event_t *ev) {
    if (!m || !ev || m->result != OX_DLG_OPEN) return;
    if (ev->type == OX_EV_MOUSE) {
        m->btn_ok.hover = ox_button_hit(&m->btn_ok, ev->x, ev->y);
        if (ev->mouse_kind == OX_MOUSE_DOWN && (ev->buttons & 0x01) &&
            m->btn_ok.hover) {
            m->result = OX_DLG_OK;
        }
        return;
    }
    if (ev->type == OX_EV_KEY) {
        if (ev->ascii == '\r' || ev->ascii == '\n' ||
            ev->keycode == OX_KEY_ENTER) {
            m->result = OX_DLG_OK;
        } else if (ev->keycode == OX_KEY_ESC) {
            m->result = OX_DLG_CANCEL;
        }
    }
}

/* ---- File picker -------------------------------------------------- */

/* qsort comparator: dirs first (alpha), then files (alpha). */
static int fp_cmp_names(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void fp_reload(ox_filepicker_t *fp) {
    fp->n_entries = 0;
    DIR *d = opendir(fp->path);
    if (!d) {
        /* Couldn't open — show only ".." so the user can back out. */
        memcpy(fp->names[0], "..", 3);
        fp->is_dir[0] = 1;
        fp->n_entries = 1;
    } else {
        struct dirent *de;
        while ((de = readdir(d)) != NULL &&
               fp->n_entries < OX_FP_MAX_ENTRIES) {
            const char *nm = de->d_name;
            if (!nm[0]) continue;
            if (nm[0] == '.' && nm[1] == 0) continue;   /* skip "." */
            int n = (int)strlen(nm);
            if (n >= OX_FP_NAME_MAX) n = OX_FP_NAME_MAX - 1;
            memcpy(fp->names[fp->n_entries], nm, (size_t)n);
            fp->names[fp->n_entries][n] = 0;
            fp->is_dir[fp->n_entries] = (de->d_type == DT_DIR) ? 1 : 0;
            fp->n_entries++;
        }
        closedir(d);
    }
    /* Sort: directories first, then files, alpha within each. */
    for (int i = 0; i < fp->n_entries; i++) {
        int best = i;
        for (int j = i + 1; j < fp->n_entries; j++) {
            int a_is_d = fp->is_dir[best];
            int b_is_d = fp->is_dir[j];
            if (b_is_d > a_is_d) { best = j; continue; }
            if (b_is_d < a_is_d) continue;
            if (fp_cmp_names(fp->names[j], fp->names[best]) < 0) best = j;
        }
        if (best != i) {
            char tmp[OX_FP_NAME_MAX];
            memcpy(tmp, fp->names[i], OX_FP_NAME_MAX);
            memcpy(fp->names[i], fp->names[best], OX_FP_NAME_MAX);
            memcpy(fp->names[best], tmp, OX_FP_NAME_MAX);
            char td = fp->is_dir[i];
            fp->is_dir[i] = fp->is_dir[best];
            fp->is_dir[best] = td;
        }
    }
    /* Prefix dirs with a marker so the listview shows them clearly. */
    for (int i = 0; i < fp->n_entries; i++) {
        fp->items[i] = fp->names[i];
    }
    fp->lv.items = fp->items;
    fp->lv.n_items = fp->n_entries;
    fp->lv.scroll = 0;
    fp->lv.sel = fp->n_entries > 0 ? 0 : -1;
}

static void fp_layout(ox_filepicker_t *fp) {
    int x = fp->x, y = fp->y, w = fp->w, h = fp->h;
    int title_h = 24;
    int path_h  = 22;
    int btn_h   = 26;
    int btn_gap = 8;

    fp->btn_up.x = x + w - 80 - 8;
    fp->btn_up.y = y + title_h + 4;
    fp->btn_up.w = 80;
    fp->btn_up.h = path_h - 2;
    fp->btn_up.label = "Up ..";

    fp->lv.x      = x + 8;
    fp->lv.y      = y + title_h + path_h + 8;
    fp->lv.w      = w - 16;
    fp->lv.h      = h - title_h - path_h - 8 - btn_h - 16;
    fp->lv.item_h = 18;

    fp->btn_cancel.x = x + w - 90;
    fp->btn_cancel.y = y + h - btn_h - 8;
    fp->btn_cancel.w = 80;
    fp->btn_cancel.h = btn_h;
    fp->btn_cancel.label = "Cancel";

    fp->btn_ok.x = fp->btn_cancel.x - 80 - btn_gap;
    fp->btn_ok.y = fp->btn_cancel.y;
    fp->btn_ok.w = 80;
    fp->btn_ok.h = btn_h;
    fp->btn_ok.label = "Open";
}

void ox_filepicker_open(ox_filepicker_t *fp, int sx, int sy, int sw, int sh,
                         const char *start_path) {
    if (!fp) return;
    int w = 460, h = 360;
    if (w > sw - 20) w = sw - 20;
    if (h > sh - 20) h = sh - 20;
    fp->x = sx + (sw - w) / 2;
    fp->y = sy + (sh - h) / 2;
    fp->w = w;
    fp->h = h;
    fp->result = OX_DLG_OPEN;
    fp->chosen[0] = 0;
    if (start_path && start_path[0]) {
        size_t L = strlen(start_path);
        if (L >= sizeof(fp->path)) L = sizeof(fp->path) - 1;
        memcpy(fp->path, start_path, L);
        fp->path[L] = 0;
    } else {
        memcpy(fp->path, "/home", 6);
    }
    fp_layout(fp);
    fp->lv.sel = -1;
    fp->lv.hover = -1;
    fp_reload(fp);
}

static void fp_join(char *out, size_t cap, const char *dir, const char *name) {
    int dl = (int)strlen(dir);
    int nl = (int)strlen(name);
    int slash = (dl == 0 || dir[dl - 1] != '/') ? 1 : 0;
    int total = dl + slash + nl;
    if (total + 1 > (int)cap) { /* truncate */
        if (cap == 0) return;
        out[0] = 0;
        return;
    }
    memcpy(out, dir, (size_t)dl);
    if (slash) out[dl] = '/';
    memcpy(out + dl + slash, name, (size_t)nl);
    out[total] = 0;
}

static void fp_go_up(ox_filepicker_t *fp) {
    int n = (int)strlen(fp->path);
    if (n <= 1) return;                 /* already at root */
    while (n > 1 && fp->path[n - 1] == '/') n--;
    while (n > 1 && fp->path[n - 1] != '/') n--;
    if (n == 0) { fp->path[0] = '/'; fp->path[1] = 0; }
    else        { fp->path[n] = 0; }
    /* Trim trailing slash unless root. */
    int m = (int)strlen(fp->path);
    if (m > 1 && fp->path[m - 1] == '/') fp->path[m - 1] = 0;
    fp_reload(fp);
}

static void fp_pick_or_cd(ox_filepicker_t *fp) {
    if (fp->lv.sel < 0 || fp->lv.sel >= fp->n_entries) return;
    const char *nm = fp->names[fp->lv.sel];
    int is_dir = fp->is_dir[fp->lv.sel];
    if (nm[0] == '.' && nm[1] == '.' && nm[2] == 0) {
        fp_go_up(fp);
        return;
    }
    if (is_dir) {
        char next[OX_FP_PATH_MAX];
        fp_join(next, sizeof(next), fp->path, nm);
        memcpy(fp->path, next, sizeof(next));
        fp_reload(fp);
        return;
    }
    /* File chosen — build full path and finish. */
    fp_join(fp->chosen, sizeof(fp->chosen), fp->path, nm);
    fp->result = OX_DLG_CHOSEN;
}

void ox_filepicker_draw(ox_win_t win, const ox_filepicker_t *fp) {
    if (!fp) return;
    dlg_frame(win, fp->x, fp->y, fp->w, fp->h, "Open File");
    /* Path strip. */
    int title_h = 24;
    ox_draw_rect(win, fp->x + 2, fp->y + title_h + 1,
                  fp->w - 4, 22, OX_RGB(232, 232, 232));
    char shown[OX_FP_PATH_MAX + 8];
    snprintf(shown, sizeof(shown), "Path: %s", fp->path);
    ui_text(win, fp->x + 10,
            fp->y + title_h + 1 + (22 - ui_text_h()) / 2, shown,
            OX_UI_COL_FG);
    ox_button_draw(win, &fp->btn_up);
    /* List. */
    ox_listview_draw(win, &fp->lv);
    /* Mark dirs with a trailing slash so they read distinctly even
     * though we don't have an icon column. */
    int ih = fp->lv.item_h > 0 ? fp->lv.item_h : 18;
    int vis = fp->lv.h / ih;
    for (int i = 0; i < vis; i++) {
        int idx = fp->lv.scroll + i;
        if (idx < 0 || idx >= fp->n_entries) break;
        if (!fp->is_dir[idx]) continue;
        int row_y = fp->lv.y + 1 + i * ih;
        uint32_t fg = (idx == fp->lv.sel) ? OX_UI_COL_HI_FG : OX_RGB(50, 80, 160);
        ui_text(win,
                fp->lv.x + fp->lv.w - 14,
                row_y + (ih - ui_text_h()) / 2,
                "/", fg);
    }
    /* Buttons. */
    ox_button_draw(win, &fp->btn_ok);
    ox_button_draw(win, &fp->btn_cancel);
}

void ox_filepicker_event(ox_filepicker_t *fp, const ox_event_t *ev) {
    if (!fp || !ev || fp->result != OX_DLG_OPEN) return;
    if (ev->type == OX_EV_MOUSE) {
        fp->btn_ok.hover     = ox_button_hit(&fp->btn_ok,     ev->x, ev->y);
        fp->btn_cancel.hover = ox_button_hit(&fp->btn_cancel, ev->x, ev->y);
        fp->btn_up.hover     = ox_button_hit(&fp->btn_up,     ev->x, ev->y);
        if (ev->mouse_kind == OX_MOUSE_DOWN && (ev->buttons & 0x01)) {
            if (fp->btn_ok.hover)     { fp_pick_or_cd(fp); return; }
            if (fp->btn_cancel.hover) { fp->result = OX_DLG_CANCEL; return; }
            if (fp->btn_up.hover)     { fp_go_up(fp); return; }
        }
        ox_listview_event(&fp->lv, ev);
        return;
    }
    if (ev->type == OX_EV_KEY) {
        if (ev->ascii == '\r' || ev->ascii == '\n' ||
            ev->keycode == OX_KEY_ENTER) {
            fp_pick_or_cd(fp);
            return;
        }
        if (ev->keycode == OX_KEY_ESC) {
            fp->result = OX_DLG_CANCEL;
            return;
        }
        if (ev->keycode == OX_KEY_BACKSPACE) {
            fp_go_up(fp);
            return;
        }
        ox_listview_event(&fp->lv, ev);
    }
}
