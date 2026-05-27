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

static int char_w(void) { return 8; }

/* ---- Button ------------------------------------------------------- */

void ox_button_draw(ox_win_t win, const ox_button_t *b) {
    if (!b) return;
    uint32_t bg = OX_UI_COL_BTN;
    if (b->pressed)      bg = OX_UI_COL_BTN_DOWN;
    else if (b->hover)   bg = OX_UI_COL_BTN_HOT;
    ox_draw_rect(win, b->x, b->y, b->w, b->h, bg);
    /* Hard 1px frame. */
    ox_draw_rect(win, b->x,             b->y,             b->w, 1, OX_UI_COL_BORDER);
    ox_draw_rect(win, b->x,             b->y + b->h - 1,  b->w, 1, OX_UI_COL_BORDER);
    ox_draw_rect(win, b->x,             b->y,             1, b->h, OX_UI_COL_BORDER);
    ox_draw_rect(win, b->x + b->w - 1,  b->y,             1, b->h, OX_UI_COL_BORDER);
    if (b->label) {
        int tw = (int)strlen(b->label) * char_w();
        int tx = b->x + (b->w - tw) / 2;
        int ty = b->y + (b->h - 8) / 2;
        ox_draw_text(win, tx, ty, b->label, OX_UI_COL_FG);
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
    int tw = (int)strlen(l->text) * char_w();
    int tx = l->x;
    if (l->align == OX_ALIGN_CENTER)      tx = l->x + (l->w - tw) / 2;
    else if (l->align == OX_ALIGN_RIGHT)  tx = l->x + l->w - tw - 4;
    else                                   tx = l->x + 4;
    int ty = l->y + (l->h - 8) / 2;
    ox_draw_text(win, tx, ty, l->text, fg);
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
    /* 1px outline. */
    ox_draw_rect(win, lv->x,            lv->y,            lv->w, 1, OX_UI_COL_BORDER_LITE);
    ox_draw_rect(win, lv->x,            lv->y + lv->h-1,  lv->w, 1, OX_UI_COL_BORDER_LITE);
    ox_draw_rect(win, lv->x,            lv->y,            1, lv->h, OX_UI_COL_BORDER_LITE);
    ox_draw_rect(win, lv->x + lv->w-1,  lv->y,            1, lv->h, OX_UI_COL_BORDER_LITE);
    int ih = lv_item_h(lv);
    int vis = lv_visible_rows(lv);
    for (int i = 0; i < vis; i++) {
        int idx = lv->scroll + i;
        if (idx < 0 || idx >= lv->n_items) break;
        int row_y = lv->y + i * ih;
        uint32_t bg = OX_UI_COL_BG_ALT;
        uint32_t fg = OX_UI_COL_FG;
        if (idx == lv->sel) {
            bg = OX_UI_COL_HI;
            fg = OX_UI_COL_HI_FG;
        } else if (idx == lv->hover) {
            bg = OX_UI_COL_BG;
        }
        ox_draw_rect(win, lv->x + 1, row_y, lv->w - 2, ih, bg);
        const char *s = lv->items[idx];
        if (s) ox_draw_text(win, lv->x + 6, row_y + (ih - 8) / 2, s, fg);
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
    ox_draw_rect(win, bar_x, sv->y, bw, sv->h, OX_UI_COL_SCROLL_TR);
    int ty, th;
    if (!sv_thumb_geom(sv, &ty, &th)) return;
    uint32_t fg = sv->drag_active ? OX_UI_COL_SCROLL_THUMB_HOT
                                  : OX_UI_COL_SCROLL_THUMB;
    ox_draw_rect(win, bar_x + 2, ty, bw - 4, th, fg);
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
    /* Solid dim backdrop around the dialog (cheap modal hint). */
    /* (Skipped — we draw the dialog on top of the caller's existing
     *  render, which is already a useful visual cue without an
     *  expensive alpha-blend pass.) */
    /* Body. */
    ox_draw_rect(win, x, y, w, h, COL_DLG_BG);
    /* 1px frame. */
    ox_draw_rect(win, x,         y,         w, 1, COL_DLG_BORDER);
    ox_draw_rect(win, x,         y + h - 1, w, 1, COL_DLG_BORDER);
    ox_draw_rect(win, x,         y,         1, h, COL_DLG_BORDER);
    ox_draw_rect(win, x + w - 1, y,         1, h, COL_DLG_BORDER);
    /* Title bar. */
    ox_draw_rect(win, x + 1, y + 1, w - 2, 18, COL_DLG_TITLE_BG);
    if (title) ox_draw_text(win, x + 8, y + 6, title, COL_DLG_TITLE_FG);
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
        int line_y = m->y + 32;
        char buf[256];
        int n = 0;
        while (*p) {
            if (*p == '\n' || n >= (int)sizeof(buf) - 1) {
                buf[n] = 0;
                ox_draw_text(win, m->x + 12, line_y, buf, OX_UI_COL_FG);
                line_y += 12;
                n = 0;
                if (*p == '\n') p++;
                continue;
            }
            buf[n++] = *p++;
        }
        if (n > 0) {
            buf[n] = 0;
            ox_draw_text(win, m->x + 12, line_y, buf, OX_UI_COL_FG);
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
    int title_h = 18;
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
    int title_h = 18;
    ox_draw_rect(win, fp->x + 1, fp->y + title_h + 1,
                  fp->w - 2, 22, OX_RGB(232, 232, 232));
    char shown[OX_FP_PATH_MAX + 8];
    snprintf(shown, sizeof(shown), "Path: %s", fp->path);
    ox_draw_text(win, fp->x + 8, fp->y + title_h + 7, shown,
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
        int row_y = fp->lv.y + i * ih;
        uint32_t fg = (idx == fp->lv.sel) ? OX_UI_COL_HI_FG : OX_RGB(50, 80, 160);
        ox_draw_text(win,
                      fp->lv.x + fp->lv.w - 14,
                      row_y + (ih - 8) / 2,
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
