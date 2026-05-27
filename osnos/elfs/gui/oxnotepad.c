/*
 * /bin/oxnotepad — full-featured text editor for the Ox window system.
 *
 * Features (FASE 12.3 rewrite):
 *   - Positional cursor with arrow keys, Home/End, PgUp/PgDn, Ctrl+Home/End
 *   - Click to position cursor; mouse wheel scroll
 *   - Delete key removes char AT cursor; Backspace removes char BEFORE
 *   - Auto-scroll when cursor moves off-screen
 *   - 64 KiB buffer (was 4 KiB)
 *   - Ctrl+S save, Ctrl+Q quit-without-save, status bar shows
 *     line:col + dirty marker
 *
 * Storage model: a single linear byte buffer (g_buf[g_len]). Cursor is
 * a byte offset (0..g_len). Lines are computed on demand by walking
 * the buffer and tracking '\n' boundaries — simpler than a piece table
 * for sub-MB files. No undo.
 */

#include <errno.h>
#include <fcntl.h>
#include <ox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WIN_W       640
#define WIN_H       440
#define MARGIN_X    10
#define MARGIN_Y    8
#define LINE_H      12
#define CHAR_W      8
#define STATUS_H    16
#define DEFAULT_PATH "/home/notepad.txt"
#define BUF_MAX     (64 * 1024)

#define COL_BG       OX_RGB(250, 250, 245)
#define COL_TEXT     OX_RGB( 20,  20,  30)
#define COL_SEL_BG   OX_RGB(180, 210, 255)   /* light blue for selection */
#define COL_SEL_FG   OX_RGB( 10,  10,  20)
#define COL_GUTTER   OX_RGB(232, 232, 222)
#define COL_GUTTER_FG OX_RGB(140, 140, 130)
#define COL_CURSOR   OX_RGB( 53, 132, 228)   /* Adwaita blue */
#define COL_STATUS_BG OX_RGB( 60,  80, 130)
#define COL_STATUS_FG OX_RGB(240, 240, 255)
#define COL_STATUS_DIRTY OX_RGB(255, 200, 100)

static char     g_buf[BUF_MAX];
static int      g_len = 0;
static int      g_cur = 0;        /* cursor: byte offset 0..g_len */
static int      g_anchor = -1;    /* selection anchor; -1 = no selection */
static int      g_scroll = 0;     /* topmost visible line index */
static int      g_dirty = 0;      /* unsaved changes flag */
static int      g_dragging = 0;   /* mouse drag-selecting flag */
static char     g_path[256] = DEFAULT_PATH;
static ox_win_t g_win;

/* Find / Replace modal state.
 *   g_find_mode 0 = no dialog
 *   g_find_mode 1 = find dialog (Ctrl+F)
 *   g_find_mode 2 = replace dialog (Ctrl+H), 2 fields
 *   g_find_field tracks which of {find, replace} has the caret. */
#define FIND_MAX  128
static int  g_find_mode  = 0;
static int  g_find_field = 0;     /* 0=find, 1=replace */
static char g_find_buf[FIND_MAX];
static int  g_find_len = 0;
static int  g_find_caret = 0;
static char g_replace_buf[FIND_MAX];
static int  g_replace_len = 0;
static int  g_replace_caret = 0;

/* Selection helpers: anchor + cur form a range. The range is
 * normalized to [lo, hi) for ops. anchor == cur means empty selection
 * (treated as "no selection"). anchor == -1 means user never started
 * a selection (also no selection). */
static int sel_active(void) { return g_anchor >= 0 && g_anchor != g_cur; }
static int sel_lo(void) { return g_anchor < g_cur ? g_anchor : g_cur; }
static int sel_hi(void) { return g_anchor < g_cur ? g_cur : g_anchor; }
static void sel_clear(void) { g_anchor = -1; }
/* Start a selection if none active; subsequent shift+arrow extends it.
 * Call BEFORE moving the cursor. */
static void sel_begin_if_needed(void) { if (g_anchor < 0) g_anchor = g_cur; }

/* Layout-derived constants (computed once in main once we know WIN_W).
 * Gutter width = digits-for-max-line * CHAR_W + padding; visible cols
 * accounts for it. For simplicity we use a fixed gutter of 5 chars. */
#define GUTTER_W   (5 * CHAR_W + 4)
#define BODY_X     (MARGIN_X + GUTTER_W)
#define BODY_W     (WIN_W - BODY_X - MARGIN_X)
#define BODY_Y     MARGIN_Y
#define BODY_H     (WIN_H - STATUS_H - BODY_Y)
#define VIS_LINES  (BODY_H / LINE_H)
#define VIS_COLS   (BODY_W / CHAR_W)

/* ---------------- buffer helpers ----------------------------------- */

/* Walk the buffer counting newlines up to offset; returns line index
 * (0-based) and out_col = bytes since previous '\n' (or buffer start). */
static void byte_to_lc(int off, int *out_line, int *out_col) {
    int line = 0, col = 0;
    for (int i = 0; i < off && i < g_len; i++) {
        if (g_buf[i] == '\n') { line++; col = 0; }
        else col++;
    }
    *out_line = line;
    *out_col  = col;
}

/* Count total lines in buffer (always >= 1). */
static int total_lines(void) {
    int n = 1;
    for (int i = 0; i < g_len; i++)
        if (g_buf[i] == '\n') n++;
    return n;
}

/* Returns byte offset of the START of `line` (0-based). If line is
 * past EOF, returns g_len. */
static int line_start_off(int line) {
    if (line <= 0) return 0;
    int cur = 0;
    for (int i = 0; i < g_len; i++) {
        if (g_buf[i] == '\n') {
            cur++;
            if (cur == line) return i + 1;
        }
    }
    return g_len;
}

/* Length of `line` in bytes (excludes the trailing '\n'). */
static int line_length(int line) {
    int start = line_start_off(line);
    int i = start;
    while (i < g_len && g_buf[i] != '\n') i++;
    return i - start;
}

/* ---------------- undo / redo ring --------------------------------- */

/* Each op records a single insertion or deletion. payload[] holds the
 * actual bytes affected so we can apply the inverse. Op chains aren't
 * built — each keystroke is its own op (simple, slightly verbose). */
#define UNDO_RING       128
#define UNDO_PAYLOAD_MAX 1024
typedef enum { OP_NONE = 0, OP_INSERT = 1, OP_DELETE = 2 } undo_op_t;
typedef struct {
    undo_op_t kind;
    int       off;
    int       len;
    char      data[UNDO_PAYLOAD_MAX];
} undo_entry_t;

static undo_entry_t g_undo[UNDO_RING];
static int          g_undo_top = 0;   /* next slot to push into */
static int          g_undo_count = 0; /* how many entries are live */
static undo_entry_t g_redo[UNDO_RING];
static int          g_redo_top = 0;
static int          g_redo_count = 0;
static int          g_in_undo = 0;    /* re-entrancy guard */

static void redo_clear(void) {
    g_redo_top = 0;
    g_redo_count = 0;
}

static void undo_push(undo_op_t kind, int off, const char *data, int len) {
    if (g_in_undo) return;
    if (len > UNDO_PAYLOAD_MAX) len = UNDO_PAYLOAD_MAX;
    undo_entry_t *e = &g_undo[g_undo_top];
    e->kind = kind;
    e->off  = off;
    e->len  = len;
    if (data && len > 0) memcpy(e->data, data, (size_t)len);
    g_undo_top = (g_undo_top + 1) % UNDO_RING;
    if (g_undo_count < UNDO_RING) g_undo_count++;
    redo_clear();    /* fresh edit invalidates redo stack */
}

static void redo_push(undo_op_t kind, int off, const char *data, int len) {
    if (len > UNDO_PAYLOAD_MAX) len = UNDO_PAYLOAD_MAX;
    undo_entry_t *e = &g_redo[g_redo_top];
    e->kind = kind;
    e->off  = off;
    e->len  = len;
    if (data && len > 0) memcpy(e->data, data, (size_t)len);
    g_redo_top = (g_redo_top + 1) % UNDO_RING;
    if (g_redo_count < UNDO_RING) g_redo_count++;
}

static int undo_pop(undo_entry_t *out) {
    if (g_undo_count == 0) return 0;
    g_undo_top = (g_undo_top + UNDO_RING - 1) % UNDO_RING;
    *out = g_undo[g_undo_top];
    g_undo_count--;
    return 1;
}

static int redo_pop(undo_entry_t *out) {
    if (g_redo_count == 0) return 0;
    g_redo_top = (g_redo_top + UNDO_RING - 1) % UNDO_RING;
    *out = g_redo[g_redo_top];
    g_redo_count--;
    return 1;
}

/* Forward refs. */
static void buf_insert_n(const char *src, int n);
static void buf_delete_range(int lo, int hi);
/* Internal: raw insert/delete WITHOUT pushing an undo entry. */
static void raw_insert_at(int off, const char *src, int n) {
    if (n <= 0) return;
    if (g_len + n > BUF_MAX - 1) n = BUF_MAX - 1 - g_len;
    if (n <= 0) return;
    memmove(g_buf + off + n, g_buf + off, (size_t)(g_len - off));
    memcpy(g_buf + off, src, (size_t)n);
    g_len += n;
    g_dirty = 1;
}
static void raw_delete_range(int lo, int hi) {
    if (lo < 0) lo = 0;
    if (hi > g_len) hi = g_len;
    if (lo >= hi) return;
    memmove(g_buf + lo, g_buf + hi, (size_t)(g_len - hi));
    g_len -= (hi - lo);
    g_dirty = 1;
}

static void do_undo(void) {
    undo_entry_t e;
    if (!undo_pop(&e)) return;
    g_in_undo = 1;
    if (e.kind == OP_INSERT) {
        /* Undo of insert = delete those bytes. */
        redo_push(OP_INSERT, e.off, e.data, e.len);
        raw_delete_range(e.off, e.off + e.len);
        g_cur = e.off;
    } else if (e.kind == OP_DELETE) {
        /* Undo of delete = re-insert the recorded bytes. */
        redo_push(OP_DELETE, e.off, e.data, e.len);
        raw_insert_at(e.off, e.data, e.len);
        g_cur = e.off + e.len;
    }
    g_in_undo = 0;
}

static void do_redo(void) {
    undo_entry_t e;
    if (!redo_pop(&e)) return;
    g_in_undo = 1;
    if (e.kind == OP_INSERT) {
        /* Re-apply the insert. */
        g_undo[g_undo_top].kind = OP_INSERT;
        g_undo[g_undo_top].off  = e.off;
        g_undo[g_undo_top].len  = e.len;
        memcpy(g_undo[g_undo_top].data, e.data, (size_t)e.len);
        g_undo_top = (g_undo_top + 1) % UNDO_RING;
        if (g_undo_count < UNDO_RING) g_undo_count++;
        raw_insert_at(e.off, e.data, e.len);
        g_cur = e.off + e.len;
    } else if (e.kind == OP_DELETE) {
        g_undo[g_undo_top].kind = OP_DELETE;
        g_undo[g_undo_top].off  = e.off;
        g_undo[g_undo_top].len  = e.len;
        memcpy(g_undo[g_undo_top].data, e.data, (size_t)e.len);
        g_undo_top = (g_undo_top + 1) % UNDO_RING;
        if (g_undo_count < UNDO_RING) g_undo_count++;
        raw_delete_range(e.off, e.off + e.len);
        g_cur = e.off;
    }
    g_in_undo = 0;
}

/* ---------------- insertion / deletion ----------------------------- */

/* Delete [lo, hi) from the buffer. Leaves cursor at lo. Used to
 * implement "replace selection" (any insertion or backspace while a
 * selection is active wipes it first). */
static void buf_delete_range(int lo, int hi) {
    if (lo < 0) lo = 0;
    if (hi > g_len) hi = g_len;
    if (lo >= hi) return;
    /* Push undo entry with the bytes we're about to drop. */
    undo_push(OP_DELETE, lo, g_buf + lo, hi - lo);
    memmove(g_buf + lo, g_buf + hi, (size_t)(g_len - hi));
    g_len -= (hi - lo);
    g_cur = lo;
    g_dirty = 1;
}

/* Insert bytes at cursor. Used by single-char typing AND paste. */
static void buf_insert_n(const char *src, int n) {
    if (n <= 0) return;
    if (g_len + n > BUF_MAX - 1) n = BUF_MAX - 1 - g_len;
    if (n <= 0) return;
    /* Record what we just inserted so undo can re-delete. */
    undo_push(OP_INSERT, g_cur, src, n);
    memmove(g_buf + g_cur + n, g_buf + g_cur, (size_t)(g_len - g_cur));
    memcpy(g_buf + g_cur, src, (size_t)n);
    g_len += n;
    g_cur += n;
    g_dirty = 1;
}

/* Convenience: insert single char. If a selection is active, replace
 * it (standard editor behavior — typing over a selection deletes it
 * first). */
static void buf_insert(char c) {
    if (sel_active()) { buf_delete_range(sel_lo(), sel_hi()); sel_clear(); }
    buf_insert_n(&c, 1);
}

static void buf_backspace(void) {
    if (sel_active()) { buf_delete_range(sel_lo(), sel_hi()); sel_clear(); return; }
    if (g_cur == 0) return;
    /* Record the byte being deleted so undo can restore it. */
    undo_push(OP_DELETE, g_cur - 1, g_buf + g_cur - 1, 1);
    memmove(g_buf + g_cur - 1, g_buf + g_cur, (size_t)(g_len - g_cur));
    g_len--;
    g_cur--;
    g_dirty = 1;
}

static void buf_delete(void) {
    if (sel_active()) { buf_delete_range(sel_lo(), sel_hi()); sel_clear(); return; }
    if (g_cur >= g_len) return;
    undo_push(OP_DELETE, g_cur, g_buf + g_cur, 1);
    memmove(g_buf + g_cur, g_buf + g_cur + 1, (size_t)(g_len - g_cur - 1));
    g_len--;
    g_dirty = 1;
}

/* ---------------- clipboard ops ------------------------------------ */

static void do_copy(void) {
    if (!sel_active()) return;
    int lo = sel_lo(), hi = sel_hi();
    ox_clipboard_set(g_buf + lo, hi - lo);
}

static void do_cut(void) {
    if (!sel_active()) return;
    int lo = sel_lo(), hi = sel_hi();
    ox_clipboard_set(g_buf + lo, hi - lo);
    buf_delete_range(lo, hi);
    sel_clear();
}

static void do_paste(void) {
    char clip[1024];
    int n = ox_clipboard_get(clip, sizeof(clip));
    if (n <= 0) return;
    if (sel_active()) { buf_delete_range(sel_lo(), sel_hi()); sel_clear(); }
    buf_insert_n(clip, n);
}

static void do_select_all(void) {
    if (g_len == 0) return;
    g_anchor = 0;
    g_cur    = g_len;
}

/* ---------------- find / replace ----------------------------------- */

/* Naive substring search inside g_buf, starting at `from`. Returns
 * offset of match or -1. Case-sensitive. */
static int find_next(int from, const char *needle, int nlen) {
    if (nlen <= 0 || nlen > g_len) return -1;
    if (from < 0) from = 0;
    int end = g_len - nlen;
    for (int i = from; i <= end; i++) {
        int j;
        for (j = 0; j < nlen; j++)
            if (g_buf[i + j] != needle[j]) break;
        if (j == nlen) return i;
    }
    return -1;
}

/* Search starting after the current cursor; wrap to start on miss.
 * Sets selection over the match and scrolls into view. Returns 1 if
 * a match was found anywhere. */
static int find_action(void) {
    if (g_find_len == 0) return 0;
    int from = g_cur;
    if (sel_active() && sel_lo() == g_cur - g_find_len) from = g_cur;
    int hit = find_next(from, g_find_buf, g_find_len);
    if (hit < 0) hit = find_next(0, g_find_buf, g_find_len);   /* wrap */
    if (hit < 0) { sel_clear(); return 0; }
    g_anchor = hit;
    g_cur    = hit + g_find_len;
    return 1;
}

/* Replace the current selection (assumed to be a match of g_find_buf)
 * with g_replace_buf, then find next. */
static void replace_action(void) {
    if (g_find_len == 0) return;
    if (sel_active() &&
        sel_hi() - sel_lo() == g_find_len) {
        /* Replace this match. */
        buf_delete_range(sel_lo(), sel_hi());
        sel_clear();
        if (g_replace_len > 0) buf_insert_n(g_replace_buf, g_replace_len);
    }
    find_action();
}

static void replace_all_action(void) {
    if (g_find_len == 0) return;
    int from = 0;
    int count_guard = 0;
    while (count_guard++ < 10000) {
        int hit = find_next(from, g_find_buf, g_find_len);
        if (hit < 0) break;
        g_cur = hit;
        buf_delete_range(hit, hit + g_find_len);
        sel_clear();
        if (g_replace_len > 0) {
            buf_insert_n(g_replace_buf, g_replace_len);
            from = hit + g_replace_len;
        } else {
            from = hit;
        }
    }
}

static void open_find(int with_replace) {
    g_find_mode = with_replace ? 2 : 1;
    g_find_field = 0;
    /* Pre-fill find buffer from current selection if any (~vim style). */
    if (sel_active()) {
        int lo = sel_lo(), hi = sel_hi();
        int n = hi - lo;
        if (n > FIND_MAX - 1) n = FIND_MAX - 1;
        memcpy(g_find_buf, g_buf + lo, (size_t)n);
        g_find_len = n;
        g_find_caret = n;
        g_find_buf[g_find_len] = 0;
    }
}

static void close_find(void) {
    g_find_mode = 0;
    g_find_field = 0;
}

/* Field-text editing helpers used by the modal. fld 0 = find, 1 = replace. */
static char *fld_buf(int fld)   { return fld ? g_replace_buf : g_find_buf; }
static int  *fld_len_p(int fld) { return fld ? &g_replace_len : &g_find_len; }
static int  *fld_car_p(int fld) { return fld ? &g_replace_caret : &g_find_caret; }

static void fld_insert(int fld, char c) {
    int *len = fld_len_p(fld); int *car = fld_car_p(fld);
    if (*len + 1 >= FIND_MAX) return;
    char *b = fld_buf(fld);
    memmove(b + *car + 1, b + *car, (size_t)(*len - *car));
    b[*car] = c;
    (*len)++;
    (*car)++;
    b[*len] = 0;
}
static void fld_backspace(int fld) {
    int *len = fld_len_p(fld); int *car = fld_car_p(fld);
    if (*car == 0) return;
    char *b = fld_buf(fld);
    memmove(b + *car - 1, b + *car, (size_t)(*len - *car));
    (*len)--;
    (*car)--;
    b[*len] = 0;
}

/* ---------------- cursor movement ---------------------------------- */

/* Auto-scroll: if cursor is above or below the visible window,
 * adjust g_scroll so the cursor is back in view. */
static void ensure_cursor_visible(void) {
    int line, col;
    byte_to_lc(g_cur, &line, &col);
    if (line < g_scroll)               g_scroll = line;
    if (line >= g_scroll + VIS_LINES)  g_scroll = line - VIS_LINES + 1;
    if (g_scroll < 0)                  g_scroll = 0;
    (void)col;
}

static void cursor_left(void) {
    if (g_cur > 0) g_cur--;
}

static void cursor_right(void) {
    if (g_cur < g_len) g_cur++;
}

static void cursor_up(void) {
    int line, col;
    byte_to_lc(g_cur, &line, &col);
    if (line == 0) { g_cur = 0; return; }
    int prev_len = line_length(line - 1);
    int target_col = col < prev_len ? col : prev_len;
    g_cur = line_start_off(line - 1) + target_col;
}

static void cursor_down(void) {
    int line, col;
    byte_to_lc(g_cur, &line, &col);
    int last = total_lines() - 1;
    if (line >= last) { g_cur = g_len; return; }
    int next_len = line_length(line + 1);
    int target_col = col < next_len ? col : next_len;
    g_cur = line_start_off(line + 1) + target_col;
}

static void cursor_home(void) {
    int line, col;
    byte_to_lc(g_cur, &line, &col);
    g_cur = line_start_off(line);
}

static void cursor_end(void) {
    int line, col;
    byte_to_lc(g_cur, &line, &col);
    g_cur = line_start_off(line) + line_length(line);
}

static void cursor_pgup(void) {
    for (int i = 0; i < VIS_LINES - 1; i++) cursor_up();
}

static void cursor_pgdn(void) {
    for (int i = 0; i < VIS_LINES - 1; i++) cursor_down();
}

/* Map click (x, y) in window coords to a byte offset. */
static void cursor_from_click(int x, int y) {
    if (y < BODY_Y) y = BODY_Y;
    if (y >= BODY_Y + BODY_H) y = BODY_Y + BODY_H - 1;
    int row = (y - BODY_Y) / LINE_H;
    int line = g_scroll + row;
    int total = total_lines();
    if (line >= total) line = total - 1;
    if (line < 0) line = 0;
    int col = 0;
    if (x > BODY_X) col = (x - BODY_X) / CHAR_W;
    int line_len = line_length(line);
    if (col > line_len) col = line_len;
    g_cur = line_start_off(line) + col;
}

/* ---------------- file I/O ---------------------------------------- */

static void load_file(void) {
    int fd = open(g_path, O_RDONLY);
    if (fd < 0) return;
    int n = (int)read(fd, g_buf, BUF_MAX - 1);
    close(fd);
    if (n > 0) g_len = n;
}

static int save_file(void) {
    int fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int wrote = (int)write(fd, g_buf, (size_t)g_len);
    close(fd);
    if (wrote != g_len) return -1;
    g_dirty = 0;
    return 0;
}

/* ---------------- render ------------------------------------------ */

static void render(void) {
    /* Background. */
    ox_draw_rect(g_win, 0, 0, WIN_W, WIN_H - STATUS_H, COL_BG);
    /* Gutter strip. */
    ox_draw_rect(g_win, 0, 0, GUTTER_W + MARGIN_X, WIN_H - STATUS_H, COL_GUTTER);

    /* Body — render visible lines. Walk to first visible line, then
     * stream chars line by line until we run out of vis lines or buf. */
    int line = 0;
    int i = 0;
    while (i < g_len && line < g_scroll) {
        if (g_buf[i] == '\n') line++;
        i++;
    }

    /* Cursor location precomputed so we draw it at the right cell. */
    int cur_line, cur_col;
    byte_to_lc(g_cur, &cur_line, &cur_col);

    /* Selection range in byte coords (only used if sel_active). */
    int slo = sel_active() ? sel_lo() : -1;
    int shi = sel_active() ? sel_hi() : -1;

    int row = 0;
    while (row < VIS_LINES) {
        int y = BODY_Y + row * LINE_H;
        int x = BODY_X;

        /* Gutter line number — only for lines that exist. */
        if (line < total_lines()) {
            char numbuf[8];
            snprintf(numbuf, sizeof(numbuf), "%4d", line + 1);
            ox_draw_text(g_win, MARGIN_X, y, numbuf, COL_GUTTER_FG);
        }

        int col = 0;
        while (i < g_len && g_buf[i] != '\n') {
            if (col >= VIS_COLS) {
                /* Skip the rest of the line — no horizontal scroll. */
                while (i < g_len && g_buf[i] != '\n') i++;
                break;
            }
            int selected = (slo >= 0 && i >= slo && i < shi);
            if (selected) {
                ox_draw_rect(g_win, x, y, CHAR_W, LINE_H, COL_SEL_BG);
            }
            char s[2] = { g_buf[i], 0 };
            ox_draw_text(g_win, x, y, s, selected ? COL_SEL_FG : COL_TEXT);
            x += CHAR_W;
            col++;
            i++;
        }
        /* If the newline itself is part of the selection, paint a
         * little tail at end-of-line so the user sees the line break
         * is included. */
        if (slo >= 0 && i < g_len && g_buf[i] == '\n' &&
            i >= slo && i < shi) {
            ox_draw_rect(g_win, x, y, CHAR_W / 2, LINE_H, COL_SEL_BG);
        }
        /* Draw cursor caret on this row if it lives here. */
        if (line == cur_line) {
            int cx = BODY_X + cur_col * CHAR_W;
            if (cur_col > VIS_COLS) cx = BODY_X + VIS_COLS * CHAR_W;
            ox_draw_rect(g_win, cx, y, 2, LINE_H - 2, COL_CURSOR);
        }
        if (i < g_len && g_buf[i] == '\n') i++;
        line++;
        row++;
    }

    /* Status bar. */
    ox_draw_rect(g_win, 0, WIN_H - STATUS_H, WIN_W, STATUS_H, COL_STATUS_BG);
    char status[160];
    if (sel_active()) {
        int n = sel_hi() - sel_lo();
        snprintf(status, sizeof(status),
                 " %s%s  %d:%d  [%d sel]  ^X/^C/^V/^A  ^S save  ^Q quit",
                 g_dirty ? "* " : "  ",
                 g_path, cur_line + 1, cur_col + 1, n);
    } else {
        snprintf(status, sizeof(status),
                 " %s%s  %d:%d  %d bytes  ^S save  ^V paste  ^A all  ^Q quit",
                 g_dirty ? "* " : "  ",
                 g_path, cur_line + 1, cur_col + 1, g_len);
    }
    ox_draw_text(g_win, MARGIN_X, WIN_H - 12, status,
                 g_dirty ? COL_STATUS_DIRTY : COL_STATUS_FG);

    /* Find/Replace modal — fixed strip above the status bar. */
    if (g_find_mode) {
        int rows = (g_find_mode == 2) ? 2 : 1;
        int dlg_h = rows * LINE_H + 8;
        int dlg_y = WIN_H - STATUS_H - dlg_h - 2;
        ox_draw_rect(g_win, 0, dlg_y, WIN_W, dlg_h,
                     OX_RGB(238, 232, 200));    /* pale yellow */
        ox_draw_rect(g_win, 0, dlg_y, WIN_W, 1, OX_RGB(0, 0, 0));
        ox_draw_rect(g_win, 0, dlg_y + dlg_h - 1, WIN_W, 1, OX_RGB(0, 0, 0));
        /* Find: label + text */
        const char *flbl = "Find:";
        ox_draw_text(g_win, 8, dlg_y + 3, flbl, OX_RGB(0, 0, 0));
        char fbuf[FIND_MAX + 4];
        snprintf(fbuf, sizeof(fbuf), "%s", g_find_buf);
        ox_draw_text(g_win, 8 + 6 * CHAR_W, dlg_y + 3, fbuf,
                     OX_RGB(20, 20, 30));
        if (g_find_field == 0) {
            int cx = 8 + 6 * CHAR_W + g_find_caret * CHAR_W;
            ox_draw_rect(g_win, cx, dlg_y + 2, 2, LINE_H - 2,
                         COL_CURSOR);
        }
        if (g_find_mode == 2) {
            ox_draw_text(g_win, 8, dlg_y + 3 + LINE_H, "Repl:",
                         OX_RGB(0, 0, 0));
            char rbuf[FIND_MAX + 4];
            snprintf(rbuf, sizeof(rbuf), "%s", g_replace_buf);
            ox_draw_text(g_win, 8 + 6 * CHAR_W,
                         dlg_y + 3 + LINE_H, rbuf,
                         OX_RGB(20, 20, 30));
            if (g_find_field == 1) {
                int cx = 8 + 6 * CHAR_W + g_replace_caret * CHAR_W;
                ox_draw_rect(g_win, cx, dlg_y + 2 + LINE_H, 2,
                             LINE_H - 2, COL_CURSOR);
            }
        }
        /* Hint right-aligned. */
        const char *hint = g_find_mode == 2
            ? "Enter=replace+next  Tab=field  ^Enter=all  Esc"
            : "Enter=next  F3=next  Esc";
        int hlen = (int)strlen(hint);
        ox_draw_text(g_win, WIN_W - 8 - hlen * CHAR_W,
                     dlg_y + 3, hint, OX_RGB(80, 80, 80));
    }

    ox_present(g_win);
}

/* ---------------- main ------------------------------------------- */

int main(int argc, char **argv) {
    if (argc > 1 && argv[1] && argv[1][0]) {
        size_t L = strlen(argv[1]);
        if (L >= sizeof(g_path)) L = sizeof(g_path) - 1;
        memcpy(g_path, argv[1], L);
        g_path[L] = 0;
    }
    ox_log("oxnotepad: starting\n");
    if (ox_init() < 0) return 1;

    char title[80];
    snprintf(title, sizeof(title), "Notepad — %s", g_path);
    g_win = ox_window_create(WIN_W, WIN_H, title);
    if (g_win < 0) return 1;

    load_file();
    render();

    int quit = 0;
    while (!quit) {
        ox_event_t ev;
        if (!ox_wait_event(&ev)) continue;

        if (ev.type == OX_EV_CLOSE) {
            if (g_dirty) save_file();
            break;
        }

        if (ev.type == OX_EV_MOUSE) {
            if (ev.mouse_kind == OX_MOUSE_DOWN && (ev.buttons & 0x01)) {
                /* Left-click positions cursor + starts a fresh
                 * selection anchor here; drag (MOVE while button down)
                 * extends the selection. Shift-click would extend the
                 * existing anchor but we don't track mods on mouse yet. */
                cursor_from_click(ev.x, ev.y);
                g_anchor = g_cur;        /* fresh anchor */
                g_dragging = 1;
                ensure_cursor_visible();
                render();
                continue;
            }
            if (ev.mouse_kind == OX_MOUSE_MOVE && g_dragging &&
                (ev.buttons & 0x01)) {
                cursor_from_click(ev.x, ev.y);
                ensure_cursor_visible();
                render();
                continue;
            }
            if (ev.mouse_kind == OX_MOUSE_UP) {
                g_dragging = 0;
                /* If user clicked without dragging, anchor == cur →
                 * selection collapses to nothing. Leave it. */
                if (g_anchor == g_cur) sel_clear();
                continue;
            }
            if (ev.mouse_kind == OX_MOUSE_WHEEL) {
                g_scroll -= ev.wheel_delta * 3;
                int max_scroll = total_lines() - VIS_LINES;
                if (max_scroll < 0) max_scroll = 0;
                if (g_scroll < 0)            g_scroll = 0;
                if (g_scroll > max_scroll)   g_scroll = max_scroll;
                render();
                continue;
            }
            continue;
        }

        if (ev.type != OX_EV_KEY) continue;

        /* Modal find/replace dialog absorbs all keys while open. */
        if (g_find_mode) {
            int kc = ev.keycode;
            int a  = ev.ascii;
            if (kc == OX_KEY_ESC || a == 27) {
                close_find();
                render();
                continue;
            }
            if (kc == OX_KEY_TAB && g_find_mode == 2) {
                g_find_field ^= 1;
                render();
                continue;
            }
            /* Find next on Enter (or F3). For replace mode + active
             * match, Enter replaces this match + advances. */
            if (a == '\r' || a == '\n' || kc == OX_KEY_ENTER ||
                kc == 61 /* F3 */) {
                if (g_find_mode == 2 &&
                    (ev.mods & OX_MOD_CTRL)) {
                    replace_all_action();
                } else if (g_find_mode == 2) {
                    replace_action();
                } else {
                    find_action();
                }
                ensure_cursor_visible();
                render();
                continue;
            }
            int fld = g_find_field;
            if (a == '\b' || kc == OX_KEY_BACKSPACE) {
                fld_backspace(fld);
                render();
                continue;
            }
            if (kc == OX_KEY_LEFT) {
                int *c = fld_car_p(fld);
                if (*c > 0) (*c)--;
                render();
                continue;
            }
            if (kc == OX_KEY_RIGHT) {
                int *c = fld_car_p(fld);
                int *L = fld_len_p(fld);
                if (*c < *L) (*c)++;
                render();
                continue;
            }
            if (a >= 0x20 && a < 0x7f) {
                fld_insert(fld, (char)a);
                render();
                continue;
            }
            /* Ignore everything else while modal is up. */
            continue;
        }

        /* Ctrl combos — keyboard driver cooks Ctrl+<letter> to the
         * control-char value (0x01..0x1f). Match on those directly;
         * OX_MOD_CTRL bit isn't reliable on this layer. Some keys
         * conflict with their literal control character:
         *   0x08 = Ctrl+H AND Backspace
         *   0x09 = Ctrl+I AND Tab
         *   0x0a = Ctrl+J AND \n
         *   0x0d = Ctrl+M AND Enter
         * Those four we route via dedicated keycodes elsewhere. */
        {
            int a = ev.ascii;
            if (a == 0x13) { save_file(); render(); continue; }   /* ^S */
            if (a == 0x11) {                                      /* ^Q */
                if (g_dirty) save_file(); quit = 1; continue;
            }
            if (a == 0x01) {                                      /* ^A */
                do_select_all(); ensure_cursor_visible(); render(); continue;
            }
            if (a == 0x03) { do_copy(); continue; }               /* ^C */
            if (a == 0x18) {                                      /* ^X */
                do_cut(); ensure_cursor_visible(); render(); continue;
            }
            if (a == 0x16) {                                      /* ^V */
                do_paste(); ensure_cursor_visible(); render(); continue;
            }
            if (a == 0x1a) {                                      /* ^Z */
                do_undo(); ensure_cursor_visible(); render(); continue;
            }
            if (a == 0x19) {                                      /* ^Y */
                do_redo(); ensure_cursor_visible(); render(); continue;
            }
            if (a == 0x06) { open_find(0); render(); continue; }  /* ^F */
            if (a == 0x12) { open_find(1); render(); continue; }  /* ^R (replace, avoid ^H conflict) */
        }
        /* F3 = find next (only if a find pattern is set). */
        if (ev.keycode == 61 /* F3 */ && g_find_len > 0) {
            find_action(); ensure_cursor_visible(); render(); continue;
        }
        if (ev.ascii == 0x13) {   /* legacy Ctrl+S without mods bit */
            save_file();
            render();
            continue;
        }

        /* Navigation. Shift+arrow extends selection: start anchor if
         * none and DON'T clear it. Plain arrow collapses selection. */
        int shift = (ev.mods & OX_MOD_SHIFT) != 0;
        int kc = ev.keycode;
        if (kc == OX_KEY_LEFT || kc == OX_KEY_RIGHT ||
            kc == OX_KEY_UP   || kc == OX_KEY_DOWN  ||
            kc == OX_KEY_HOME || kc == OX_KEY_END   ||
            kc == OX_KEY_PGUP || kc == OX_KEY_PGDN) {
            if (shift) sel_begin_if_needed();
            else       sel_clear();
            switch (kc) {
            case OX_KEY_LEFT:  cursor_left();  break;
            case OX_KEY_RIGHT: cursor_right(); break;
            case OX_KEY_UP:    cursor_up();    break;
            case OX_KEY_DOWN:  cursor_down();  break;
            case OX_KEY_HOME:  cursor_home();  break;
            case OX_KEY_END:   cursor_end();   break;
            case OX_KEY_PGUP:  cursor_pgup();  break;
            case OX_KEY_PGDN:  cursor_pgdn();  break;
            }
            ensure_cursor_visible();
            render();
            continue;
        }
        if (ev.keycode == OX_KEY_DELETE)     { buf_delete();   ensure_cursor_visible(); render(); continue; }

        /* Edits. */
        if (ev.ascii == '\b' || ev.keycode == OX_KEY_BACKSPACE) {
            buf_backspace();
            ensure_cursor_visible();
            render();
            continue;
        }
        int ch = ev.ascii;
        if (ch == '\r') ch = '\n';
        if (ch == '\n' || (ch >= 0x20 && ch < 0x7f) || ch == '\t') {
            buf_insert((char)ch);
            ensure_cursor_visible();
            render();
            continue;
        }
    }

    ox_window_destroy(g_win);
    return 0;
}
