#pragma once
/*
 * ox_ui.h — minimal widget toolkit for Ox apps.
 *
 * Mini-Xt for osnos. The four widgets here cover ~90% of the UI work
 * the Ox apps currently reinvent every time: a clickable button, a
 * static label, a vertically-scrollable list of strings, and a clip
 * region with a scrollbar + wheel/drag handling for arbitrary content.
 *
 * Widgets are POD structs — caller owns the storage. There is no
 * retained-mode tree; every event is dispatched explicitly and every
 * widget is drawn on demand. Apps stay in their existing event loop
 * and just delegate to these helpers.
 */

#include <stdint.h>
#include "ox.h"

/* ---- Theme tokens — keep in sync with oxsrv's BeOS palette ------- */
#define OX_UI_COL_BG          OX_RGB(232, 232, 232)
#define OX_UI_COL_BG_ALT      OX_RGB(244, 244, 244)
#define OX_UI_COL_BORDER      OX_RGB( 80,  80,  80)
#define OX_UI_COL_BORDER_LITE OX_RGB(180, 180, 180)
#define OX_UI_COL_FG          OX_RGB( 20,  20,  20)
#define OX_UI_COL_FG_DIM      OX_RGB(110, 110, 110)
#define OX_UI_COL_HI          OX_RGB(102, 152, 203)   /* Haiku-blue */
#define OX_UI_COL_HI_FG       OX_RGB(255, 255, 255)
#define OX_UI_COL_BTN         OX_RGB(220, 220, 220)
#define OX_UI_COL_BTN_HOT     OX_RGB(238, 238, 238)
#define OX_UI_COL_BTN_DOWN    OX_RGB(200, 200, 200)
#define OX_UI_COL_SCROLL_TR   OX_RGB(216, 216, 216)
#define OX_UI_COL_SCROLL_THUMB OX_RGB(160, 160, 160)
#define OX_UI_COL_SCROLL_THUMB_HOT OX_RGB(110, 130, 170)

/* ---- ox_button_t -------------------------------------------------- */

typedef struct {
    int        x, y, w, h;
    const char *label;
    int        hover;      /* mouse currently over */
    int        pressed;    /* mouse button down on this widget */
} ox_button_t;

void ox_button_draw(ox_win_t win, const ox_button_t *b);

/* Returns 1 if (mx,my) hits the button. */
int  ox_button_hit(const ox_button_t *b, int mx, int my);

/* ---- ox_label_t --------------------------------------------------- */

#define OX_ALIGN_LEFT   0
#define OX_ALIGN_CENTER 1
#define OX_ALIGN_RIGHT  2

typedef struct {
    int        x, y, w, h;
    const char *text;
    uint32_t   fg;       /* 0 → OX_UI_COL_FG */
    uint32_t   bg;       /* 0 → transparent */
    int        align;    /* OX_ALIGN_* */
} ox_label_t;

void ox_label_draw(ox_win_t win, const ox_label_t *l);

/* ---- ox_listview_t ------------------------------------------------ */

typedef struct {
    int          x, y, w, h;
    const char **items;
    int          n_items;
    int          item_h;       /* px per row (default 18) */
    int          scroll;       /* topmost visible index */
    int          sel;          /* -1 = none */
    int          hover;        /* -1 = none */
} ox_listview_t;

void ox_listview_draw(ox_win_t win, const ox_listview_t *lv);

/* Returns row index under (mx,my), or -1 if outside the rows. */
int  ox_listview_hit(const ox_listview_t *lv, int mx, int my);

/* Convenience: handle a single ox_event_t. Returns 1 if the event
 * was consumed (selection or scroll changed) and the caller should
 * redraw, 0 otherwise. */
int  ox_listview_event(ox_listview_t *lv, const ox_event_t *ev);

/* ---- ox_scrollview_t --------------------------------------------- */
/*
 * A clip rect with a vertical scrollbar. The caller's "content" has a
 * logical height in pixels (content_h); only `h` of it is visible at
 * a time, with `scroll_y` controlling the top.
 *
 * Typical use:
 *   ox_scrollview_t sv = { ... };
 *   for each event: if (ox_scrollview_event(&sv, &ev)) render();
 *   in render: ox_scrollview_draw_bg + draw your content offset by
 *     -sv.scroll_y + ox_scrollview_draw_bar.
 */
typedef struct {
    int x, y, w, h;
    int content_h;
    int scroll_y;
    int wheel_step;   /* px per wheel notch (default 3 lines = 30) */
    int bar_w;        /* default 12 */
    int drag_active;
    int drag_offset_y;
} ox_scrollview_t;

void ox_scrollview_draw_bg (ox_win_t win, const ox_scrollview_t *sv);
void ox_scrollview_draw_bar(ox_win_t win, const ox_scrollview_t *sv);

/* Returns 1 if event caused scroll to change. */
int  ox_scrollview_event(ox_scrollview_t *sv, const ox_event_t *ev);

/* Clamp scroll_y to the legal range. Call after changing content_h. */
void ox_scrollview_clamp(ox_scrollview_t *sv);

/* ---- Dialogs ------------------------------------------------------ *
 *
 * Modal-ish overlays drawn into the caller's window. They're "modal"
 * in spirit — while one is active, callers should funnel every event
 * through ox_dialog_*_event() instead of doing their own thing, and
 * skip rendering their normal UI under the dialog rect.
 *
 * Dialogs are stack-allocated by the caller. State machine:
 *   result == OX_DLG_OPEN     while waiting for user input
 *   result == OX_DLG_OK       user pressed OK / Enter
 *   result == OX_DLG_CANCEL   user pressed Cancel / Esc
 *   result == OX_DLG_CHOSEN   filepicker only — `chosen` has the path
 */
#define OX_DLG_OPEN   0
#define OX_DLG_OK     1
#define OX_DLG_CANCEL 2
#define OX_DLG_CHOSEN 3

/* Simple message box with an OK button. `msg` may include newlines. */
typedef struct {
    int        x, y, w, h;
    const char *title;
    const char *msg;
    int        result;
    ox_button_t btn_ok;
} ox_msgbox_t;

void ox_msgbox_open (ox_msgbox_t *m, int sx, int sy, int sw, int sh,
                      const char *title, const char *msg);
void ox_msgbox_draw (ox_win_t win, const ox_msgbox_t *m);
void ox_msgbox_event(ox_msgbox_t *m, const ox_event_t *ev);

/* File picker — browse a directory tree, pick a file. */
#define OX_FP_MAX_ENTRIES 256
#define OX_FP_PATH_MAX    256
#define OX_FP_NAME_MAX     96

typedef struct {
    int          x, y, w, h;
    char         path[OX_FP_PATH_MAX];      /* current directory */
    char         chosen[OX_FP_PATH_MAX];    /* result on OX_DLG_CHOSEN */
    int          result;
    char         names[OX_FP_MAX_ENTRIES][OX_FP_NAME_MAX];
    char         is_dir[OX_FP_MAX_ENTRIES];
    int          n_entries;
    const char  *items[OX_FP_MAX_ENTRIES];  /* points into names[]    */
    ox_listview_t lv;
    ox_button_t   btn_ok;
    ox_button_t   btn_cancel;
    ox_button_t   btn_up;
} ox_filepicker_t;

void ox_filepicker_open (ox_filepicker_t *fp, int sx, int sy, int sw, int sh,
                          const char *start_path);
void ox_filepicker_draw (ox_win_t win, const ox_filepicker_t *fp);
void ox_filepicker_event(ox_filepicker_t *fp, const ox_event_t *ev);
