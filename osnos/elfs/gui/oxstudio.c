/*
 * /bin/oxstudio — JS playground for the Ox window system.
 *
 * Layout
 *   ┌─ toolbar:  [Run] [Stop] [Save] [Load] [New] [Templates]
 *   ├─ editor:   multi-line code editor with syntax colouring,
 *   │            current-line band, indent guides, bracket match
 *   └─ status:   path · line:col · last error
 *
 * Editing features
 *   - Cursor + selection (shift-arrows extend, click+drag selects)
 *   - Word-jump (Ctrl+Left/Right, Ctrl+Shift+arrows extends, Ctrl+Backspace)
 *   - Undo/Redo (Ctrl+Z / Ctrl+Y) — 32-slot snapshot ring
 *   - Auto-indent on Enter (copies leading ws, +2 spaces after `{`)
 *   - Bracket / quote auto-close ({ [ ( " ' `) + smart overtype of close
 *   - Find (Ctrl+F): modal text field, Enter=next, wraps on miss
 *   - Soft tab = 2 spaces
 *   - Standard clipboard (Ctrl+A/C/X/V)
 *
 * Run flow
 *   - F5 / Ctrl+R / Run button: auto-saves the buffer first (to its path
 *     if set, else to /home/studio/.unsaved.js so nothing is ever lost)
 *     then spawns /bin/oxjs against the file. The child pid is shown in
 *     the status bar.
 *   - Stop button: SIGTERM the last-spawned oxjs child. Useful when a
 *     script enters an infinite loop and its Ox window stops repainting.
 *
 * Storage model
 *   - Linear byte buffer g_buf[g_len], cursor as offset (0..g_len),
 *     lines computed on-the-fly by walking '\n'. Same pattern as
 *     oxnotepad; trade-off documented there.
 *   - Files live under /home/studio/ (created on first save). Load
 *     accepts a bare name (resolved against /home/studio/) or an
 *     absolute path so /home/apps/<showcase>.js can be opened too.
 *
 * Highlighting
 *   - Single pass over the buffer fills g_color[] with one byte per
 *     source byte (TOK_*). Render groups runs of same colour into one
 *     ox_draw_text per run for cheap drawing.
 *
 * Resizable
 *   - Opts into the Ox real-resize protocol. Cell metrics stay fixed;
 *     growing the window shows more rows and columns.
 */

#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <ox.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <osnos_ipc.h>

extern char **environ;

/* --- layout ------------------------------------------------------- */

#define INIT_W      900
#define INIT_H      600
#define MARGIN_X     8
#define CHAR_W       8
#define LINE_H      12
#define GUTTER_W    (5 * CHAR_W + 4)
#define TOOLBAR_H   28
#define STATUS_H    16
#define BUF_MAX     (32 * 1024)
#define MIN_W       460
#define MIN_H       260
#define PATH_MAX_LEN 128
#define UNDO_LEVELS  32
#define FIND_MAX    128
#define TAB_WIDTH    2

/* --- palette: Adwaita dark + syntax ------------------------------- */

#define COL_BG          OX_RGB( 30,  30,  36)
#define COL_LINE_HI     OX_RGB( 40,  42,  52)
#define COL_GUTTER_BG   OX_RGB( 24,  24,  28)
#define COL_GUTTER_FG   OX_RGB(110, 115, 132)
#define COL_GUTTER_CUR  OX_RGB(186, 222, 252)
#define COL_TEXT        OX_RGB(214, 214, 220)
#define COL_CURSOR      OX_RGB(166, 226, 255)
#define COL_SEL_BG      OX_RGB( 55,  85, 130)
#define COL_SEL_FG      OX_RGB(240, 240, 245)
#define COL_INDENT_GUIDE OX_RGB( 52,  54,  64)
#define COL_BRACKET_BOX  OX_RGB(170, 130,  90)
#define COL_FIND_HI_BG   OX_RGB(120,  95,  35)

#define COL_TB_BG       OX_RGB( 45,  46,  56)
#define COL_TB_BORDER   OX_RGB( 18,  18,  22)
#define COL_BTN_BG      OX_RGB( 70,  72,  90)
#define COL_BTN_BG_HI   OX_RGB(100, 115, 165)
#define COL_BTN_BG_RUN  OX_RGB( 70, 130,  90)
#define COL_BTN_BG_STOP OX_RGB(130,  70,  80)
#define COL_BTN_BG_DIS  OX_RGB( 52,  54,  62)
#define COL_BTN_FG      OX_RGB(228, 228, 234)
#define COL_BTN_FG_DIS  OX_RGB(120, 122, 130)

#define COL_STATUS_BG   OX_RGB( 38,  60,  92)
#define COL_STATUS_FG   OX_RGB(225, 230, 240)
#define COL_STATUS_ERR  OX_RGB(255, 150, 150)

#define COL_SYN_KW      OX_RGB(186, 222, 252)
#define COL_SYN_STR     OX_RGB(166, 226, 138)
#define COL_SYN_NUM     OX_RGB(247, 200, 124)
#define COL_SYN_COMMENT OX_RGB(118, 130, 152)

#define COL_MENU_BG     OX_RGB( 50,  52,  62)
#define COL_MENU_BORDER OX_RGB( 18,  18,  22)
#define COL_MENU_HI     OX_RGB(100, 115, 165)
#define COL_MENU_FG     OX_RGB(225, 225, 230)
#define COL_PROMPT_BG   OX_RGB( 70,  60,  30)

/* --- token classes (one byte per source byte) --------------------- */
enum {
    TOK_TEXT = 0,
    TOK_KEYWORD,
    TOK_STRING,
    TOK_NUMBER,
    TOK_COMMENT,
};

/* --- core state --------------------------------------------------- */

static char     g_buf[BUF_MAX];
static unsigned char g_color[BUF_MAX];
static int      g_len = 0;
static int      g_cur = 0;
static int      g_anchor = -1;
static int      g_scroll = 0;
static int      g_dirty = 0;
static int      g_dragging = 0;

static int      g_win_w = INIT_W;
static int      g_win_h = INIT_H;
static ox_win_t g_win;

static char     g_path[PATH_MAX_LEN] = "";
static char     g_status_msg[160] = "";
static uint32_t g_status_col = COL_STATUS_FG;
static int      g_quit = 0;

/* Tracking the currently-running JS child so the Stop button can kill
 * it. Set by run_buffer; cleared whenever the user clicks Stop. We
 * don't auto-clear on child exit (no SIGCHLD wired in oxstudio) so the
 * status line may keep saying "running pid N" after N is already gone
 * — harmless; Stop will then fail with ESRCH and we'll reset. */
static long     g_run_pid = -1;

/* --- prompt (Save As / Load filename input) ----------------------- */
static int  g_prompt_mode = 0;
static char g_prompt_buf[PATH_MAX_LEN];
static int  g_prompt_len  = 0;
static int  g_prompt_caret = 0;

/* --- find dialog -------------------------------------------------- */
static int  g_find_mode = 0;        /* 0=off, 1=open */
static char g_find_buf[FIND_MAX];
static int  g_find_len = 0;
static int  g_find_caret = 0;

/* --- bracket match (computed each render) ------------------------- */
static int  g_brk_a = -1, g_brk_b = -1;   /* byte offsets of the pair */

/* --- toolbar buttons --------------------------------------------- */
typedef struct { int x, y, w, h; const char *label; int kind; } btn_t;
enum { BTN_RUN = 1, BTN_STOP, BTN_SAVE, BTN_LOAD, BTN_NEW, BTN_TEMPLATES };
#define BTN_MAX 8
static btn_t g_btns[BTN_MAX];
static int   g_n_btns = 0;
static int   g_hover_btn = 0;

/* --- templates menu ---------------------------------------------- */
typedef struct { const char *label; const char *body; } tpl_t;
static const char TPL_EMPTY[] =
    "\n";
static const char TPL_HELLO[] =
    "// Hello window — minimal Ox JS app.\n"
    "ox.window(\"Hello\", 360, 220);\n"
    "ox.onPaint(function() {\n"
    "  ox.clear(\"#1e1e22\");\n"
    "  ox.text(20, 24, \"Hello from oxstudio!\", \"#cdf\");\n"
    "  ox.text(20, 44, \"Edit, then press F5 to re-run.\", \"#9aa\");\n"
    "});\n";
static const char TPL_TICK[] =
    "// Tick loop — animates on a 16ms cadence.\n"
    "ox.window(\"Tick\", 400, 260);\n"
    "var t = 0;\n"
    "ox.onPaint(function() {\n"
    "  ox.clear(\"#101018\");\n"
    "  var cx = 200 + Math.cos(t * 0.05) * 80;\n"
    "  var cy = 130 + Math.sin(t * 0.07) * 60;\n"
    "  ox.draw.circle(cx, cy, 20, \"#a6e2ff\");\n"
    "  ox.text(10, 10, \"t = \" + t, \"#aaa\");\n"
    "});\n"
    "ox.onTick(function() { t++; });\n";
static const char TPL_HTTP[] =
    "// HTTP fetch — pulls index.html from the in-guest lighttpd.\n"
    "ox.window(\"HTTP\", 480, 320);\n"
    "var body = \"\";\n"
    "try {\n"
    "  var r = ox.http.get(\"http://10.0.2.2:8080/\");\n"
    "  body = r && r.body ? r.body : \"(empty)\";\n"
    "} catch (e) { body = \"error: \" + e; }\n"
    "ox.onPaint(function() {\n"
    "  ox.clear(\"#16161d\");\n"
    "  ox.text(10, 10, body.substr(0, 1024), \"#cde\");\n"
    "});\n";
static const char TPL_PAINT[] =
    "// Paint canvas — left-drag draws, right-click clears.\n"
    "ox.window(\"Paint\", 480, 320);\n"
    "var strokes = [];\n"
    "var last = null;\n"
    "ox.onMouse(function(e) {\n"
    "  if ((e.buttons & 1) && e.kind === \"down\") last = [e.x, e.y];\n"
    "  if ((e.buttons & 1) && e.kind === \"move\" && last) {\n"
    "    strokes.push([last[0], last[1], e.x, e.y]);\n"
    "    last = [e.x, e.y];\n"
    "  }\n"
    "  if (e.kind === \"up\") last = null;\n"
    "  if (e.buttons & 2) strokes = [];\n"
    "});\n"
    "ox.onPaint(function() {\n"
    "  ox.clear(\"#fafafa\");\n"
    "  for (var i = 0; i < strokes.length; i++) {\n"
    "    var s = strokes[i];\n"
    "    ox.draw.line(s[0], s[1], s[2], s[3], \"#222\");\n"
    "  }\n"
    "});\n";

static const tpl_t TEMPLATES[] = {
    { "Empty",         TPL_EMPTY },
    { "Hello window",  TPL_HELLO },
    { "Tick loop",     TPL_TICK  },
    { "HTTP fetch",    TPL_HTTP  },
    { "Paint canvas",  TPL_PAINT },
};
#define TPL_N (int)(sizeof(TEMPLATES) / sizeof(TEMPLATES[0]))

static int g_tpl_open = 0;            /* 0=closed, 1=open */
static int g_tpl_hover = -1;          /* hovered idx, -1 = none */

/* --- undo / redo: ring of full-buffer snapshots ------------------ */
typedef struct {
    char *data;   /* malloc'd; NULL = empty slot */
    int   len;
    int   cur;
} snap_t;
static snap_t g_undo[UNDO_LEVELS];
static int    g_undo_head = 0;        /* next slot to write */
static int    g_undo_count = 0;       /* live undo entries (<=UNDO_LEVELS) */
static snap_t g_redo[UNDO_LEVELS];
static int    g_redo_head = 0;
static int    g_redo_count = 0;

static void snap_free_at(snap_t *s) {
    if (s->data) { free(s->data); s->data = NULL; }
    s->len = 0; s->cur = 0;
}
static void snap_take(snap_t *s) {
    snap_free_at(s);
    s->data = (char *)malloc((size_t)g_len + 1);
    if (s->data) {
        if (g_len > 0) memcpy(s->data, g_buf, (size_t)g_len);
        s->data[g_len] = 0;
    }
    s->len = g_len;
    s->cur = g_cur;
}
static void snap_apply(const snap_t *s) {
    if (!s->data) return;
    int n = s->len;
    if (n > BUF_MAX - 1) n = BUF_MAX - 1;
    if (n > 0) memcpy(g_buf, s->data, (size_t)n);
    g_len = n;
    g_cur = s->cur;
    if (g_cur > g_len) g_cur = g_len;
    /* Selection is cleared by the caller (undo_pop / redo_pop). */
}
static void redo_drop_all(void) {
    for (int i = 0; i < UNDO_LEVELS; i++) snap_free_at(&g_redo[i]);
    g_redo_head = 0; g_redo_count = 0;
}
static void undo_push(void) {
    /* Push current state onto the undo ring. Any pending redo path is
     * invalidated (standard editor behaviour). */
    snap_take(&g_undo[g_undo_head]);
    g_undo_head = (g_undo_head + 1) % UNDO_LEVELS;
    if (g_undo_count < UNDO_LEVELS) g_undo_count++;
    redo_drop_all();
}
static int undo_pop(void) {
    if (g_undo_count == 0) return 0;
    /* Move current to redo stack so Ctrl+Y can come back. */
    snap_take(&g_redo[g_redo_head]);
    g_redo_head = (g_redo_head + 1) % UNDO_LEVELS;
    if (g_redo_count < UNDO_LEVELS) g_redo_count++;
    /* Pop the latest undo snapshot. */
    g_undo_head = (g_undo_head - 1 + UNDO_LEVELS) % UNDO_LEVELS;
    g_undo_count--;
    snap_apply(&g_undo[g_undo_head]);
    g_anchor = -1;
    snap_free_at(&g_undo[g_undo_head]);
    return 1;
}
static int redo_pop(void) {
    if (g_redo_count == 0) return 0;
    /* Push current state back to undo, then apply latest redo. */
    snap_take(&g_undo[g_undo_head]);
    g_undo_head = (g_undo_head + 1) % UNDO_LEVELS;
    if (g_undo_count < UNDO_LEVELS) g_undo_count++;
    g_redo_head = (g_redo_head - 1 + UNDO_LEVELS) % UNDO_LEVELS;
    g_redo_count--;
    snap_apply(&g_redo[g_redo_head]);
    g_anchor = -1;
    snap_free_at(&g_redo[g_redo_head]);
    return 1;
}

/* --- selection helpers -------------------------------------------- */
static int sel_active(void) { return g_anchor >= 0 && g_anchor != g_cur; }
static int sel_lo(void) { return g_anchor < g_cur ? g_anchor : g_cur; }
static int sel_hi(void) { return g_anchor < g_cur ? g_cur : g_anchor; }
static void sel_clear(void) { g_anchor = -1; }
static void sel_begin_if_needed(void) { if (g_anchor < 0) g_anchor = g_cur; }

/* --- layout helpers (depend on g_win_w / g_win_h) ----------------- */
static int body_y(void)     { return TOOLBAR_H; }
static int body_h(void)     { return g_win_h - TOOLBAR_H - STATUS_H; }
static int body_x(void)     { return MARGIN_X + GUTTER_W; }
static int body_w(void)     { return g_win_w - body_x() - MARGIN_X; }
static int vis_lines(void)  { int n = body_h() / LINE_H; return n < 1 ? 1 : n; }
static int vis_cols(void)   { int n = body_w() / CHAR_W;  return n < 1 ? 1 : n; }

/* --- buffer line walk --------------------------------------------- */
static void byte_to_lc(int off, int *out_line, int *out_col) {
    int line = 0, col = 0;
    for (int i = 0; i < off && i < g_len; i++) {
        if (g_buf[i] == '\n') { line++; col = 0; }
        else col++;
    }
    *out_line = line; *out_col = col;
}
static int total_lines(void) {
    int n = 1;
    for (int i = 0; i < g_len; i++) if (g_buf[i] == '\n') n++;
    return n;
}
static int line_start_off(int line) {
    int cur = 0;
    for (int i = 0; i < g_len; i++) {
        if (cur == line) return i;
        if (g_buf[i] == '\n') cur++;
    }
    return g_len;
}
static int line_length(int line) {
    int start = line_start_off(line);
    int i = start;
    while (i < g_len && g_buf[i] != '\n') i++;
    return i - start;
}

/* --- tokenizer: classify every byte of g_buf into g_color[] ------- */
static const char *KEYWORDS[] = {
    "function","var","let","const","if","else","for","while",
    "do","return","break","continue","new","this","typeof","delete",
    "in","of","instanceof","try","catch","finally","throw","switch",
    "case","default","null","undefined","true","false","void","yield",
    "async","await","class","extends","super","import","export",
    0
};
static int is_kw(const char *p, int n) {
    char buf[24];
    if (n <= 0 || n >= (int)sizeof(buf)) return 0;
    memcpy(buf, p, n); buf[n] = 0;
    for (int i = 0; KEYWORDS[i]; i++)
        if (strcmp(buf, KEYWORDS[i]) == 0) return 1;
    return 0;
}
static int is_id_start(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            c == '_' || c == '$';
}
static int is_id_cont(unsigned char c) {
    return is_id_start(c) || (c >= '0' && c <= '9');
}
static int is_word(unsigned char c) { return is_id_cont(c); }

static void retokenize(void) {
    int i = 0;
    while (i < g_len) {
        unsigned char c = (unsigned char)g_buf[i];
        if (c == '/' && i + 1 < g_len && g_buf[i+1] == '/') {
            int j = i;
            while (j < g_len && g_buf[j] != '\n') g_color[j++] = TOK_COMMENT;
            i = j; continue;
        }
        if (c == '/' && i + 1 < g_len && g_buf[i+1] == '*') {
            int j = i;
            g_color[j++] = TOK_COMMENT;
            g_color[j++] = TOK_COMMENT;
            while (j < g_len) {
                g_color[j] = TOK_COMMENT;
                if (g_buf[j] == '*' && j + 1 < g_len && g_buf[j+1] == '/') {
                    g_color[j+1] = TOK_COMMENT; j += 2; break;
                }
                j++;
            }
            i = j; continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            char q = (char)c;
            int j = i;
            g_color[j++] = TOK_STRING;
            while (j < g_len) {
                g_color[j] = TOK_STRING;
                if (g_buf[j] == '\\' && j + 1 < g_len) {
                    g_color[j+1] = TOK_STRING; j += 2; continue;
                }
                if (g_buf[j] == q) { j++; break; }
                if (g_buf[j] == '\n' && q != '`') break;
                j++;
            }
            i = j; continue;
        }
        if (c >= '0' && c <= '9') {
            int j = i;
            while (j < g_len) {
                unsigned char d = (unsigned char)g_buf[j];
                if ((d >= '0' && d <= '9') || d == '.' || d == 'x' ||
                    d == 'X' || (d >= 'a' && d <= 'f') ||
                    (d >= 'A' && d <= 'F')) g_color[j++] = TOK_NUMBER;
                else break;
            }
            i = j; continue;
        }
        if (is_id_start(c)) {
            int j = i;
            while (j < g_len && is_id_cont((unsigned char)g_buf[j])) j++;
            int klass = is_kw(g_buf + i, j - i) ? TOK_KEYWORD : TOK_TEXT;
            for (int k = i; k < j; k++) g_color[k] = (unsigned char)klass;
            i = j; continue;
        }
        g_color[i++] = TOK_TEXT;
    }
}

/* --- bracket match: nearest pair around the cursor --------------- */
static int matching_close(char c) {
    switch (c) {
    case '{': return '}'; case '[': return ']'; case '(': return ')';
    }
    return 0;
}
static int matching_open(char c) {
    switch (c) {
    case '}': return '{'; case ']': return '['; case ')': return '(';
    }
    return 0;
}
static int is_in_string_or_comment(int off) {
    if (off < 0 || off >= g_len) return 0;
    unsigned char k = g_color[off];
    return k == TOK_STRING || k == TOK_COMMENT;
}
static int find_matching(int start, int dir) {
    /* dir +1 = scan forward for matching close; -1 = backward for open.
     * Skips strings/comments via g_color. Returns -1 on miss. */
    char open_c = g_buf[start];
    char close_c;
    if (dir > 0) close_c = (char)matching_close(open_c);
    else         close_c = (char)matching_open(open_c);
    if (!close_c) return -1;
    int depth = 1;
    int i = start + dir;
    while (i >= 0 && i < g_len) {
        if (!is_in_string_or_comment(i)) {
            if (g_buf[i] == open_c)  depth++;
            else if (g_buf[i] == close_c) {
                depth--;
                if (depth == 0) return i;
            }
        }
        i += dir;
    }
    return -1;
}
static void update_bracket_match(void) {
    g_brk_a = g_brk_b = -1;
    /* Look at the char to the LEFT of the cursor first (vim-like). */
    int p = g_cur - 1;
    if (p >= 0 && p < g_len && !is_in_string_or_comment(p)) {
        char c = g_buf[p];
        if (matching_close(c)) {
            int m = find_matching(p, +1);
            if (m >= 0) { g_brk_a = p; g_brk_b = m; return; }
        } else if (matching_open(c)) {
            int m = find_matching(p, -1);
            if (m >= 0) { g_brk_a = p; g_brk_b = m; return; }
        }
    }
    /* Fall back to the char AT the cursor. */
    p = g_cur;
    if (p >= 0 && p < g_len && !is_in_string_or_comment(p)) {
        char c = g_buf[p];
        if (matching_close(c)) {
            int m = find_matching(p, +1);
            if (m >= 0) { g_brk_a = p; g_brk_b = m; return; }
        } else if (matching_open(c)) {
            int m = find_matching(p, -1);
            if (m >= 0) { g_brk_a = p; g_brk_b = m; return; }
        }
    }
}

/* --- buffer mutation: delete + insert ----------------------------- */
static void buf_delete_range(int lo, int hi) {
    if (lo < 0) lo = 0;
    if (hi > g_len) hi = g_len;
    if (lo >= hi) return;
    undo_push();
    memmove(g_buf + lo, g_buf + hi, (size_t)(g_len - hi));
    g_len -= (hi - lo);
    g_cur = lo;
    sel_clear();
    g_dirty = 1;
}
static void buf_insert_at_cur(const char *src, int n) {
    if (n <= 0) return;
    if (g_len + n >= BUF_MAX) {
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "buffer full (%d KiB max)", BUF_MAX / 1024);
        g_status_col = COL_STATUS_ERR;
        return;
    }
    undo_push();
    memmove(g_buf + g_cur + n, g_buf + g_cur, (size_t)(g_len - g_cur));
    memcpy(g_buf + g_cur, src, (size_t)n);
    g_len += n;
    g_cur += n;
    g_dirty = 1;
}
static void delete_selection_if_any(void) {
    if (!sel_active()) return;
    buf_delete_range(sel_lo(), sel_hi());
}
static void backspace(void) {
    if (sel_active()) { delete_selection_if_any(); return; }
    if (g_cur == 0) return;
    undo_push();
    memmove(g_buf + g_cur - 1, g_buf + g_cur, (size_t)(g_len - g_cur));
    g_len--; g_cur--;
    g_dirty = 1;
}
static void forward_delete(void) {
    if (sel_active()) { delete_selection_if_any(); return; }
    if (g_cur >= g_len) return;
    undo_push();
    memmove(g_buf + g_cur, g_buf + g_cur + 1, (size_t)(g_len - g_cur - 1));
    g_len--;
    g_dirty = 1;
}

/* --- word jump ---------------------------------------------------- */
static int next_word_boundary(int from) {
    int i = from;
    if (i >= g_len) return g_len;
    /* If currently in a word, skip the rest of it. */
    while (i < g_len && is_word((unsigned char)g_buf[i])) i++;
    /* Then skip the run of non-word chars to reach the next word start. */
    while (i < g_len && !is_word((unsigned char)g_buf[i]) && g_buf[i] != '\n') i++;
    return i;
}
static int prev_word_boundary(int from) {
    int i = from;
    if (i <= 0) return 0;
    i--;
    /* Skip backward over non-word. */
    while (i > 0 && !is_word((unsigned char)g_buf[i]) && g_buf[i] != '\n') i--;
    /* Then skip backward over the word. */
    while (i > 0 && is_word((unsigned char)g_buf[i - 1])) i--;
    return i;
}

/* --- cursor movement ---------------------------------------------- */
static void ensure_cursor_visible(void) {
    int line, col; byte_to_lc(g_cur, &line, &col);
    if (line < g_scroll) g_scroll = line;
    if (line >= g_scroll + vis_lines()) g_scroll = line - vis_lines() + 1;
    if (g_scroll < 0) g_scroll = 0;
}
static void cursor_left(void)  { if (g_cur > 0)     g_cur--; ensure_cursor_visible(); }
static void cursor_right(void) { if (g_cur < g_len) g_cur++; ensure_cursor_visible(); }
static void cursor_up(void) {
    int line, col; byte_to_lc(g_cur, &line, &col);
    if (line == 0) return;
    int ps = line_start_off(line - 1);
    int pl = line_length(line - 1);
    g_cur = ps + (col < pl ? col : pl);
    ensure_cursor_visible();
}
static void cursor_down(void) {
    int line, col; byte_to_lc(g_cur, &line, &col);
    if (line + 1 >= total_lines()) return;
    int ns = line_start_off(line + 1);
    int nl = line_length(line + 1);
    g_cur = ns + (col < nl ? col : nl);
    ensure_cursor_visible();
}
static void cursor_home(void) {
    int line, col; byte_to_lc(g_cur, &line, &col);
    g_cur = line_start_off(line);
    ensure_cursor_visible();
}
static void cursor_end(void) {
    int line, col; byte_to_lc(g_cur, &line, &col);
    g_cur = line_start_off(line) + line_length(line);
    ensure_cursor_visible();
}
static void cursor_pgup(void) { for (int i = 0; i < vis_lines() - 1; i++) cursor_up(); }
static void cursor_pgdn(void) { for (int i = 0; i < vis_lines() - 1; i++) cursor_down(); }
static void cursor_word_left(void)  { g_cur = prev_word_boundary(g_cur); ensure_cursor_visible(); }
static void cursor_word_right(void) { g_cur = next_word_boundary(g_cur); ensure_cursor_visible(); }
static void backspace_word(void) {
    if (sel_active()) { delete_selection_if_any(); return; }
    int target = prev_word_boundary(g_cur);
    if (target < g_cur) buf_delete_range(target, g_cur);
}

static void cursor_from_click(int x, int y) {
    int line = g_scroll + (y - body_y()) / LINE_H;
    int total = total_lines();
    if (line >= total) line = total - 1;
    if (line < 0) line = 0;
    int col = 0;
    if (x > body_x()) col = (x - body_x()) / CHAR_W;
    int ll = line_length(line);
    if (col > ll) col = ll;
    g_cur = line_start_off(line) + col;
}

/* --- path resolution ---------------------------------------------- */
static void resolve_path(const char *raw, char *out, size_t cap) {
    if (!raw || !raw[0]) { out[0] = 0; return; }
    if (raw[0] == '/') { strncpy(out, raw, cap - 1); out[cap - 1] = 0; return; }
    snprintf(out, cap, "/home/studio/%s", raw);
}

/* --- file I/O ----------------------------------------------------- */
static int load_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = (int)read(fd, g_buf, BUF_MAX - 1);
    close(fd);
    if (n < 0) return -1;
    g_len = n;
    g_cur = 0; g_scroll = 0; sel_clear();
    g_dirty = 0;
    strncpy(g_path, path, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = 0;
    /* Loading wipes editor history (a new document). */
    for (int i = 0; i < UNDO_LEVELS; i++) snap_free_at(&g_undo[i]);
    g_undo_head = 0; g_undo_count = 0;
    redo_drop_all();
    return 0;
}
static int save_file(const char *path) {
    mkdir("/home/studio", 0755);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int wrote = (int)write(fd, g_buf, (size_t)g_len);
    close(fd);
    if (wrote != g_len) return -1;
    g_dirty = 0;
    if (path != g_path) {
        strncpy(g_path, path, sizeof(g_path) - 1);
        g_path[sizeof(g_path) - 1] = 0;
    }
    return 0;
}

/* --- Run / Stop --------------------------------------------------- */
static void run_buffer(void) {
    mkdir("/home/studio", 0755);
    /* Auto-save before run: if the buffer has a path, save it there so
     * the user's source on disk matches what's running. If it's still
     * unsaved, dump to /home/studio/.unsaved.js so the work survives
     * a power cycle even before the user explicitly saves. */
    char target[PATH_MAX_LEN];
    if (g_path[0]) {
        if (g_dirty) save_file(g_path);
        strncpy(target, g_path, sizeof(target) - 1);
        target[sizeof(target) - 1] = 0;
    } else {
        snprintf(target, sizeof(target), "/home/studio/.unsaved.js");
        int fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { write(fd, g_buf, (size_t)g_len); close(fd); }
    }

    /* Stable per-pid scratch — separate from `target` so re-Running
     * never races with the user's open editor saves. */
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "/home/studio/.last_run_%d.js", (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "run: cannot create %s (%s)", tmp, strerror(errno));
        g_status_col = COL_STATUS_ERR;
        return;
    }
    int wrote = (int)write(fd, g_buf, (size_t)g_len);
    close(fd);
    if (wrote != g_len) {
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "run: short write (%d/%d)", wrote, g_len);
        g_status_col = COL_STATUS_ERR;
        return;
    }
    long pid = osn_spawn("/bin/oxjs", tmp, NULL, -1, -1);
    if (pid < 0) {
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "run: spawn failed (%s)", strerror(errno));
        g_status_col = COL_STATUS_ERR;
        return;
    }
    g_run_pid = pid;
    snprintf(g_status_msg, sizeof(g_status_msg),
             "running pid %ld (%s)", pid, target);
    g_status_col = COL_STATUS_FG;
}

static void stop_run(void) {
    if (g_run_pid <= 0) {
        snprintf(g_status_msg, sizeof(g_status_msg), "stop: no run in flight");
        g_status_col = COL_STATUS_ERR;
        return;
    }
    int r = kill((int)g_run_pid, 15);   /* SIGTERM */
    if (r < 0 && errno == ESRCH) {
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "stop: pid %ld already gone", g_run_pid);
        g_status_col = COL_STATUS_FG;
        g_run_pid = -1;
        return;
    }
    if (r < 0) {
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "stop: kill(%ld) failed: %s", g_run_pid, strerror(errno));
        g_status_col = COL_STATUS_ERR;
        return;
    }
    snprintf(g_status_msg, sizeof(g_status_msg),
             "sent SIGTERM to pid %ld", g_run_pid);
    g_status_col = COL_STATUS_FG;
    g_run_pid = -1;
}

/* --- starter snippet for "New" ------------------------------------ */
static const char *STARTER =
    "// oxstudio — JS playground (Duktape)\n"
    "// Press F5 / Ctrl+R / Run to launch.\n"
    "// Templates button has more starting points.\n"
    "\n"
    "ox.window(\"Hello\", 320, 200);\n"
    "ox.onPaint(function() {\n"
    "  ox.clear(\"#222\");\n"
    "  ox.text(20, 20, \"Hello from oxstudio!\", \"#cde\");\n"
    "});\n";

static void replace_buffer_with(const char *body) {
    /* Pushes one undo so the user can come back. */
    undo_push();
    int n = (int)strlen(body);
    if (n > BUF_MAX - 1) n = BUF_MAX - 1;
    memcpy(g_buf, body, (size_t)n);
    g_len = n;
    g_cur = 0; g_scroll = 0; sel_clear();
    g_dirty = 1;
}

static void buffer_new(void) {
    replace_buffer_with(STARTER);
    g_dirty = 0;
    g_path[0] = 0;
}

/* --- toolbar layout + hit-test ----------------------------------- */
static int btn_disabled(int kind) {
    if (kind == BTN_STOP) return g_run_pid <= 0;
    return 0;
}
static uint32_t btn_color(int kind) {
    if (btn_disabled(kind)) return COL_BTN_BG_DIS;
    if (g_hover_btn == kind) {
        if (kind == BTN_RUN)  return COL_BTN_BG_RUN;
        if (kind == BTN_STOP) return COL_BTN_BG_STOP;
        return COL_BTN_BG_HI;
    }
    return COL_BTN_BG;
}

static void layout_buttons(void) {
    struct { const char *label; int kind; int w; } defs[] = {
        { " > Run ",     BTN_RUN,       70 },
        { " ■ Stop ",    BTN_STOP,      66 },
        { " Save ",      BTN_SAVE,      56 },
        { " Load ",      BTN_LOAD,      56 },
        { " New ",       BTN_NEW,       52 },
        { " Templates ", BTN_TEMPLATES, 90 },
    };
    int n = (int)(sizeof(defs) / sizeof(defs[0]));
    int x = MARGIN_X;
    int y = 4;
    int h = TOOLBAR_H - 8;
    g_n_btns = 0;
    for (int i = 0; i < n; i++) {
        g_btns[g_n_btns].x = x;
        g_btns[g_n_btns].y = y;
        g_btns[g_n_btns].w = defs[i].w;
        g_btns[g_n_btns].h = h;
        g_btns[g_n_btns].label = defs[i].label;
        g_btns[g_n_btns].kind  = defs[i].kind;
        g_n_btns++;
        x += defs[i].w + 4;
    }
}
static int button_at(int mx, int my) {
    for (int i = 0; i < g_n_btns; i++) {
        btn_t *b = &g_btns[i];
        if (mx >= b->x && mx < b->x + b->w &&
            my >= b->y && my < b->y + b->h) return b->kind;
    }
    return 0;
}
static int find_button_rect(int kind, btn_t *out) {
    for (int i = 0; i < g_n_btns; i++) {
        if (g_btns[i].kind == kind) { *out = g_btns[i]; return 1; }
    }
    return 0;
}

/* --- render ------------------------------------------------------- */
#define TPL_MENU_W 160
#define TPL_ITEM_H 18
static int tpl_menu_geom(int *out_x, int *out_y, int *out_w, int *out_h) {
    btn_t b;
    if (!find_button_rect(BTN_TEMPLATES, &b)) return 0;
    *out_x = b.x;
    *out_y = b.y + b.h + 2;
    *out_w = TPL_MENU_W;
    *out_h = TPL_N * TPL_ITEM_H + 6;
    return 1;
}
static int tpl_index_at(int mx, int my) {
    int mx0, my0, mw, mh;
    if (!tpl_menu_geom(&mx0, &my0, &mw, &mh)) return -1;
    if (mx < mx0 || mx >= mx0 + mw || my < my0 || my >= my0 + mh) return -1;
    int idx = (my - my0 - 3) / TPL_ITEM_H;
    if (idx < 0 || idx >= TPL_N) return -1;
    return idx;
}

static void render(void) {
    retokenize();
    update_bracket_match();

    /* Toolbar. */
    ox_draw_rect(g_win, 0, 0, g_win_w, TOOLBAR_H, COL_TB_BG);
    ox_draw_rect(g_win, 0, TOOLBAR_H - 1, g_win_w, 1, COL_TB_BORDER);
    for (int i = 0; i < g_n_btns; i++) {
        btn_t *b = &g_btns[i];
        uint32_t bg = btn_color(b->kind);
        uint32_t fg = btn_disabled(b->kind) ? COL_BTN_FG_DIS : COL_BTN_FG;
        ox_draw_rect(g_win, b->x, b->y, b->w, b->h, bg);
        ox_draw_rect(g_win, b->x, b->y, b->w, 1, COL_TB_BORDER);
        ox_draw_rect(g_win, b->x, b->y + b->h - 1, b->w, 1, COL_TB_BORDER);
        int tx = b->x + 4;
        int ty = b->y + (b->h - 8) / 2;
        ox_draw_text(g_win, tx, ty, b->label, fg);
    }

    /* Body background + gutter. */
    int by = body_y();
    int bh = body_h();
    ox_draw_rect(g_win, 0, by, g_win_w, bh, COL_BG);
    ox_draw_rect(g_win, 0, by, GUTTER_W + MARGIN_X, bh, COL_GUTTER_BG);

    /* Pre-compute visible-line metadata. */
    int cur_line, cur_col; byte_to_lc(g_cur, &cur_line, &cur_col);
    int slo = sel_active() ? sel_lo() : -1;
    int shi = sel_active() ? sel_hi() : -1;
    int VL = vis_lines();
    int vc = vis_cols();
    int total = total_lines();

    /* Walk to first visible line. */
    int line = 0;
    int i = 0;
    while (i < g_len && line < g_scroll) {
        if (g_buf[i] == '\n') line++;
        i++;
    }

    char *rowbuf = (char *)alloca((size_t)vc + 1);

    int row = 0;
    while (row < VL) {
        int y = by + row * LINE_H;
        int x_base = body_x();

        /* Current-line band (subtle). */
        if (line == cur_line) {
            ox_draw_rect(g_win, GUTTER_W + MARGIN_X, y,
                         g_win_w - (GUTTER_W + MARGIN_X), LINE_H,
                         COL_LINE_HI);
        }

        /* Gutter number — accent on the current line. */
        if (line < total) {
            char numbuf[8];
            snprintf(numbuf, sizeof(numbuf), "%4d", line + 1);
            uint32_t gc = (line == cur_line) ? COL_GUTTER_CUR : COL_GUTTER_FG;
            ox_draw_text(g_win, MARGIN_X, y, numbuf, gc);
        }

        /* Selection band. */
        if (slo >= 0) {
            int ix = i, col = 0;
            while (ix < g_len && g_buf[ix] != '\n' && col < vc) {
                if (ix >= slo && ix < shi)
                    ox_draw_rect(g_win, x_base + col * CHAR_W, y,
                                 CHAR_W, LINE_H, COL_SEL_BG);
                ix++; col++;
            }
        }

        /* Indent guides — vertical dotted lines at columns 2, 4, ... up
         * to where the leading whitespace ends. */
        {
            int ix = i, col = 0;
            int last_ws_col = 0;
            while (ix < g_len && (g_buf[ix] == ' ' || g_buf[ix] == '\t') &&
                   col < vc) {
                col++; ix++;
                last_ws_col = col;
            }
            for (int gcol = TAB_WIDTH; gcol < last_ws_col; gcol += TAB_WIDTH) {
                int gx = x_base + gcol * CHAR_W;
                /* Skip drawing a guide line if the user is selecting
                 * that column — would smear with COL_SEL_BG. */
                ox_draw_rect(g_win, gx, y, 1, LINE_H, COL_INDENT_GUIDE);
            }
        }

        /* Bracket-match box (per-cell). */
        if (g_brk_a >= 0 || g_brk_b >= 0) {
            int ix = i, col = 0;
            while (ix < g_len && g_buf[ix] != '\n' && col < vc) {
                if (ix == g_brk_a || ix == g_brk_b) {
                    int bx = x_base + col * CHAR_W;
                    /* hollow box */
                    ox_draw_rect(g_win, bx,              y,           CHAR_W, 1, COL_BRACKET_BOX);
                    ox_draw_rect(g_win, bx,              y + LINE_H - 1, CHAR_W, 1, COL_BRACKET_BOX);
                    ox_draw_rect(g_win, bx,              y,           1, LINE_H, COL_BRACKET_BOX);
                    ox_draw_rect(g_win, bx + CHAR_W - 1, y,           1, LINE_H, COL_BRACKET_BOX);
                }
                ix++; col++;
            }
        }

        /* Glyphs grouped by colour + selection state. */
        int col = 0;
        int j = i;
        while (j < g_len && g_buf[j] != '\n' && col < vc) {
            int run_col = col;
            unsigned char klass = g_color[j];
            int in_sel = (slo >= 0 && j >= slo && j < shi) ? 1 : 0;
            while (j < g_len && g_buf[j] != '\n' && col < vc &&
                   g_color[j] == klass &&
                   ((slo >= 0 && j >= slo && j < shi) ? 1 : 0) == in_sel) {
                rowbuf[col - run_col] = g_buf[j];
                j++; col++;
            }
            int run_len = col - run_col;
            rowbuf[run_len] = 0;
            uint32_t fg;
            if (in_sel) fg = COL_SEL_FG;
            else switch (klass) {
                case TOK_KEYWORD: fg = COL_SYN_KW;      break;
                case TOK_STRING:  fg = COL_SYN_STR;     break;
                case TOK_NUMBER:  fg = COL_SYN_NUM;     break;
                case TOK_COMMENT: fg = COL_SYN_COMMENT; break;
                default:          fg = COL_TEXT;        break;
            }
            ox_draw_text(g_win, x_base + run_col * CHAR_W, y, rowbuf, fg);
        }
        i = j;
        while (i < g_len && g_buf[i] != '\n') i++;

        /* Cursor caret. */
        if (line == cur_line) {
            int cx = x_base + cur_col * CHAR_W;
            int max_x = x_base + vc * CHAR_W;
            if (cx > max_x) cx = max_x;
            ox_draw_rect(g_win, cx, y, 2, LINE_H - 2, COL_CURSOR);
        }
        if (i < g_len && g_buf[i] == '\n') i++;
        line++; row++;
    }

    /* Status bar. */
    ox_draw_rect(g_win, 0, g_win_h - STATUS_H, g_win_w, STATUS_H,
                 COL_STATUS_BG);
    char status[220];
    if (g_status_msg[0]) {
        snprintf(status, sizeof(status), " %s", g_status_msg);
        ox_draw_text(g_win, MARGIN_X, g_win_h - 12, status, g_status_col);
    } else {
        snprintf(status, sizeof(status),
                 " %s%s  %d:%d  %d bytes  ^S save  ^O load  ^F find  F5 run  ^Q quit",
                 g_dirty ? "* " : "  ",
                 g_path[0] ? g_path : "(unsaved)",
                 cur_line + 1, cur_col + 1, g_len);
        ox_draw_text(g_win, MARGIN_X, g_win_h - 12, status, COL_STATUS_FG);
    }

    /* Find dialog overlay (above status bar). */
    if (g_find_mode) {
        int dlg_h = LINE_H + 8;
        int dlg_y = g_win_h - STATUS_H - dlg_h - 2;
        ox_draw_rect(g_win, 0, dlg_y, g_win_w, dlg_h, COL_PROMPT_BG);
        ox_draw_rect(g_win, 0, dlg_y, g_win_w, 1, OX_RGB(0, 0, 0));
        ox_draw_rect(g_win, 0, dlg_y + dlg_h - 1, g_win_w, 1, OX_RGB(0, 0, 0));
        const char *lbl = "Find:";
        ox_draw_text(g_win, 8, dlg_y + 4, lbl, OX_RGB(230, 220, 180));
        int lbl_w = (int)strlen(lbl) * CHAR_W;
        char vbuf[FIND_MAX + 4];
        snprintf(vbuf, sizeof(vbuf), "%s", g_find_buf);
        ox_draw_text(g_win, 8 + lbl_w + 4, dlg_y + 4, vbuf,
                     OX_RGB(245, 245, 230));
        int cx = 8 + lbl_w + 4 + g_find_caret * CHAR_W;
        ox_draw_rect(g_win, cx, dlg_y + 3, 2, LINE_H - 2, COL_CURSOR);
        const char *hint = "Enter=next   Esc=close";
        int hlen = (int)strlen(hint) * CHAR_W;
        ox_draw_text(g_win, g_win_w - 8 - hlen, dlg_y + 4, hint,
                     OX_RGB(180, 180, 140));
    }

    /* Save/Load prompt overlay. */
    if (g_prompt_mode) {
        int dlg_h = LINE_H + 8;
        int dlg_y = g_win_h - STATUS_H - dlg_h - 2;
        ox_draw_rect(g_win, 0, dlg_y, g_win_w, dlg_h, COL_PROMPT_BG);
        ox_draw_rect(g_win, 0, dlg_y, g_win_w, 1, OX_RGB(0, 0, 0));
        ox_draw_rect(g_win, 0, dlg_y + dlg_h - 1, g_win_w, 1, OX_RGB(0, 0, 0));
        const char *lbl = g_prompt_mode == 1 ? "Save as:" : "Load:";
        ox_draw_text(g_win, 8, dlg_y + 4, lbl, OX_RGB(230, 220, 180));
        int lbl_w = (int)strlen(lbl) * CHAR_W;
        char vbuf[PATH_MAX_LEN + 4];
        snprintf(vbuf, sizeof(vbuf), "%s", g_prompt_buf);
        ox_draw_text(g_win, 8 + lbl_w + 4, dlg_y + 4, vbuf,
                     OX_RGB(245, 245, 230));
        int cx = 8 + lbl_w + 4 + g_prompt_caret * CHAR_W;
        ox_draw_rect(g_win, cx, dlg_y + 3, 2, LINE_H - 2, COL_CURSOR);
        const char *hint = "Enter=ok   Esc=cancel";
        int hlen = (int)strlen(hint) * CHAR_W;
        ox_draw_text(g_win, g_win_w - 8 - hlen, dlg_y + 4, hint,
                     OX_RGB(180, 180, 140));
    }

    /* Templates dropdown (drawn AFTER body so it sits on top). */
    if (g_tpl_open) {
        int mx, my, mw, mh;
        if (tpl_menu_geom(&mx, &my, &mw, &mh)) {
            ox_draw_rect(g_win, mx, my, mw, mh, COL_MENU_BG);
            ox_draw_rect(g_win, mx, my, mw, 1, COL_MENU_BORDER);
            ox_draw_rect(g_win, mx, my + mh - 1, mw, 1, COL_MENU_BORDER);
            ox_draw_rect(g_win, mx, my, 1, mh, COL_MENU_BORDER);
            ox_draw_rect(g_win, mx + mw - 1, my, 1, mh, COL_MENU_BORDER);
            for (int t = 0; t < TPL_N; t++) {
                int iy = my + 3 + t * TPL_ITEM_H;
                if (t == g_tpl_hover) {
                    ox_draw_rect(g_win, mx + 1, iy, mw - 2,
                                 TPL_ITEM_H, COL_MENU_HI);
                }
                ox_draw_text(g_win, mx + 8, iy + (TPL_ITEM_H - 8) / 2,
                             TEMPLATES[t].label, COL_MENU_FG);
            }
        }
    }

    ox_present(g_win);
}

/* --- prompt handling ---------------------------------------------- */
static void prompt_open(int mode, const char *initial) {
    g_prompt_mode = mode;
    g_prompt_buf[0] = 0; g_prompt_len = 0; g_prompt_caret = 0;
    if (initial && initial[0]) {
        const char *p = initial;
        if (strncmp(p, "/home/studio/", 13) == 0) p += 13;
        size_t L = strlen(p);
        if (L >= sizeof(g_prompt_buf)) L = sizeof(g_prompt_buf) - 1;
        memcpy(g_prompt_buf, p, L);
        g_prompt_buf[L] = 0;
        g_prompt_len = (int)L;
        g_prompt_caret = (int)L;
    }
}
static void prompt_close(void) {
    g_prompt_mode = 0; g_prompt_buf[0] = 0;
    g_prompt_len = 0; g_prompt_caret = 0;
}
static void prompt_commit(void) {
    int mode = g_prompt_mode;
    if (mode == 0 || g_prompt_len == 0) { prompt_close(); return; }
    char resolved[PATH_MAX_LEN];
    resolve_path(g_prompt_buf, resolved, sizeof(resolved));
    if (mode == 1) {
        if (save_file(resolved) == 0) {
            snprintf(g_status_msg, sizeof(g_status_msg),
                     "saved %s (%d bytes)", resolved, g_len);
            g_status_col = COL_STATUS_FG;
        } else {
            snprintf(g_status_msg, sizeof(g_status_msg),
                     "save failed: %s (%s)", resolved, strerror(errno));
            g_status_col = COL_STATUS_ERR;
        }
    } else if (mode == 2) {
        if (load_file(resolved) == 0) {
            snprintf(g_status_msg, sizeof(g_status_msg),
                     "loaded %s (%d bytes)", resolved, g_len);
            g_status_col = COL_STATUS_FG;
        } else {
            snprintf(g_status_msg, sizeof(g_status_msg),
                     "load failed: %s (%s)", resolved, strerror(errno));
            g_status_col = COL_STATUS_ERR;
        }
    }
    prompt_close();
}
static void prompt_key(int ascii, int keycode) {
    if (keycode == OX_KEY_ESC) { prompt_close(); return; }
    if (keycode == OX_KEY_ENTER || ascii == '\n' || ascii == '\r') {
        prompt_commit(); return;
    }
    if (keycode == OX_KEY_BACKSPACE || ascii == 0x08 || ascii == 0x7f) {
        if (g_prompt_caret > 0) {
            memmove(g_prompt_buf + g_prompt_caret - 1,
                    g_prompt_buf + g_prompt_caret,
                    (size_t)(g_prompt_len - g_prompt_caret));
            g_prompt_caret--; g_prompt_len--;
            g_prompt_buf[g_prompt_len] = 0;
        }
        return;
    }
    if (keycode == OX_KEY_LEFT)  { if (g_prompt_caret > 0) g_prompt_caret--; return; }
    if (keycode == OX_KEY_RIGHT) { if (g_prompt_caret < g_prompt_len) g_prompt_caret++; return; }
    if (keycode == OX_KEY_HOME)  { g_prompt_caret = 0; return; }
    if (keycode == OX_KEY_END)   { g_prompt_caret = g_prompt_len; return; }
    if (ascii >= 0x20 && ascii < 0x7f &&
        g_prompt_len + 1 < (int)sizeof(g_prompt_buf)) {
        memmove(g_prompt_buf + g_prompt_caret + 1,
                g_prompt_buf + g_prompt_caret,
                (size_t)(g_prompt_len - g_prompt_caret));
        g_prompt_buf[g_prompt_caret] = (char)ascii;
        g_prompt_caret++; g_prompt_len++;
        g_prompt_buf[g_prompt_len] = 0;
    }
}

/* --- find dialog -------------------------------------------------- */
static void find_open(void) {
    g_find_mode = 1; g_find_caret = g_find_len;
}
static void find_close(void) { g_find_mode = 0; }
static int find_next_from(int from) {
    if (g_find_len == 0) return -1;
    for (int i = from; i + g_find_len <= g_len; i++) {
        int ok = 1;
        for (int j = 0; j < g_find_len; j++) {
            if (g_buf[i + j] != g_find_buf[j]) { ok = 0; break; }
        }
        if (ok) return i;
    }
    return -1;
}
static void find_jump_next(void) {
    if (g_find_len == 0) return;
    int hit = find_next_from(g_cur + 1);
    if (hit < 0) hit = find_next_from(0);
    if (hit < 0) {
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "not found: %s", g_find_buf);
        g_status_col = COL_STATUS_ERR;
        return;
    }
    g_anchor = hit;
    g_cur = hit + g_find_len;
    ensure_cursor_visible();
    snprintf(g_status_msg, sizeof(g_status_msg),
             "found '%s' at offset %d", g_find_buf, hit);
    g_status_col = COL_STATUS_FG;
}
static void find_key(int ascii, int keycode) {
    if (keycode == OX_KEY_ESC) { find_close(); return; }
    if (keycode == OX_KEY_ENTER || ascii == '\n' || ascii == '\r') {
        find_jump_next(); return;
    }
    if (keycode == OX_KEY_BACKSPACE || ascii == 0x08 || ascii == 0x7f) {
        if (g_find_caret > 0) {
            memmove(g_find_buf + g_find_caret - 1,
                    g_find_buf + g_find_caret,
                    (size_t)(g_find_len - g_find_caret));
            g_find_caret--; g_find_len--;
            g_find_buf[g_find_len] = 0;
        }
        return;
    }
    if (keycode == OX_KEY_LEFT)  { if (g_find_caret > 0) g_find_caret--; return; }
    if (keycode == OX_KEY_RIGHT) { if (g_find_caret < g_find_len) g_find_caret++; return; }
    if (keycode == OX_KEY_HOME)  { g_find_caret = 0; return; }
    if (keycode == OX_KEY_END)   { g_find_caret = g_find_len; return; }
    if (ascii >= 0x20 && ascii < 0x7f &&
        g_find_len + 1 < FIND_MAX) {
        memmove(g_find_buf + g_find_caret + 1,
                g_find_buf + g_find_caret,
                (size_t)(g_find_len - g_find_caret));
        g_find_buf[g_find_caret] = (char)ascii;
        g_find_caret++; g_find_len++;
        g_find_buf[g_find_len] = 0;
    }
}

/* --- auto-indent + auto-pair primitives -------------------------- */
static int auto_indent_string(char *out, size_t cap) {
    /* Compute the indent string that should follow a newline inserted
     * at the current cursor: leading whitespace of the cursor's line +
     * 2 extra spaces if the last non-whitespace char before cursor on
     * that line is '{'. */
    int line, col; byte_to_lc(g_cur, &line, &col);
    int start = line_start_off(line);
    int i = start;
    int wsn = 0;
    while (i < g_cur && (g_buf[i] == ' ' || g_buf[i] == '\t') &&
           wsn + 1 < (int)cap) {
        out[wsn++] = g_buf[i++];
    }
    /* Look for last non-ws char before cursor on this line. */
    int last_nonws = -1;
    for (int k = g_cur - 1; k >= start; k--) {
        if (g_buf[k] != ' ' && g_buf[k] != '\t') { last_nonws = k; break; }
    }
    if (last_nonws >= 0 && g_buf[last_nonws] == '{' && wsn + 2 < (int)cap) {
        out[wsn++] = ' ';
        out[wsn++] = ' ';
    }
    out[wsn] = 0;
    return wsn;
}

/* --- Ctrl+letter dispatch ---------------------------------------- */
static void on_button(int kind);   /* fwd */

static int handle_ctrl_letter(int ascii) {
    switch (ascii) {
    case 's': case 'S': on_button(BTN_SAVE);  return 1;
    case 'o': case 'O': on_button(BTN_LOAD);  return 1;
    case 'r': case 'R': on_button(BTN_RUN);   return 1;
    case 'n': case 'N': on_button(BTN_NEW);   return 1;
    case 'f': case 'F': find_open();          return 1;
    case 'z': case 'Z': if (undo_pop()) g_dirty = 1; return 1;
    case 'y': case 'Y': if (redo_pop()) g_dirty = 1; return 1;
    case 'q': case 'Q':
        if (g_dirty && g_path[0]) save_file(g_path);
        g_quit = 1; return 1;
    case 'a': case 'A': g_anchor = 0; g_cur = g_len; return 1;
    case 'c': case 'C':
        if (sel_active()) ox_clipboard_set(g_buf + sel_lo(),
                                            sel_hi() - sel_lo());
        return 1;
    case 'x': case 'X':
        if (sel_active()) {
            ox_clipboard_set(g_buf + sel_lo(), sel_hi() - sel_lo());
            delete_selection_if_any();
            ensure_cursor_visible();
        }
        return 1;
    case 'v': case 'V': {
        char clip[1024];
        int n = ox_clipboard_get(clip, sizeof(clip));
        if (n > 0) {
            delete_selection_if_any();
            buf_insert_at_cur(clip, n);
            ensure_cursor_visible();
        }
        return 1;
    }
    }
    return 0;
}

/* --- toolbar action dispatch -------------------------------------- */
static void on_button(int kind) {
    if (btn_disabled(kind)) return;
    switch (kind) {
    case BTN_RUN:  run_buffer(); break;
    case BTN_STOP: stop_run();   break;
    case BTN_SAVE:
        if (g_path[0]) {
            if (save_file(g_path) == 0) {
                snprintf(g_status_msg, sizeof(g_status_msg),
                         "saved %s (%d bytes)", g_path, g_len);
                g_status_col = COL_STATUS_FG;
            } else {
                snprintf(g_status_msg, sizeof(g_status_msg),
                         "save failed (%s)", strerror(errno));
                g_status_col = COL_STATUS_ERR;
            }
        } else {
            prompt_open(1, NULL);
        }
        break;
    case BTN_LOAD: prompt_open(2, g_path); break;
    case BTN_NEW:  buffer_new();
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "new buffer (starter snippet)");
        g_status_col = COL_STATUS_FG;
        break;
    case BTN_TEMPLATES:
        g_tpl_open = !g_tpl_open;
        g_tpl_hover = -1;
        break;
    }
}

/* --- event loop --------------------------------------------------- */
int main(int argc, char **argv) {
    ox_log("oxstudio: starting argc=%d argv1=%s\n",
           argc, argc > 1 && argv[1] ? argv[1] : "(none)");
    if (ox_init() < 0) return 1;

    g_win = ox_window_create_resizable(INIT_W, INIT_H, "JS Studio");
    if (g_win < 0) return 1;
    g_win_w = INIT_W; g_win_h = INIT_H;
    layout_buttons();

    if (argc > 1 && argv[1] && argv[1][0]) {
        char resolved[PATH_MAX_LEN];
        resolve_path(argv[1], resolved, sizeof(resolved));
        if (load_file(resolved) < 0) {
            snprintf(g_status_msg, sizeof(g_status_msg),
                     "cannot open %s — starting empty", resolved);
            g_status_col = COL_STATUS_ERR;
            buffer_new();
            g_path[0] = 0;
        }
    } else {
        buffer_new();
    }

    render();
    while (!g_quit) {
        ox_event_t ev;
        if (!ox_wait_event(&ev)) continue;

        if (ev.type == OX_EV_CLOSE) break;

        if (ev.type == OX_EV_RESIZE) {
            g_win_w = ev.new_w;
            g_win_h = ev.new_h;
            if (g_win_w < MIN_W) g_win_w = MIN_W;
            if (g_win_h < MIN_H) g_win_h = MIN_H;
            layout_buttons();
            ensure_cursor_visible();
            render();
            continue;
        }

        if (ev.type == OX_EV_MOUSE) {
            int hov = (ev.y < TOOLBAR_H) ? button_at(ev.x, ev.y) : 0;
            if (hov != g_hover_btn) { g_hover_btn = hov; render(); }

            /* Hover over the templates dropdown updates the highlight. */
            if (g_tpl_open) {
                int idx = tpl_index_at(ev.x, ev.y);
                if (idx != g_tpl_hover) { g_tpl_hover = idx; render(); }
            }

            if (ev.mouse_kind == OX_MOUSE_DOWN && (ev.buttons & 0x01)) {
                /* Templates menu click takes priority while open. */
                if (g_tpl_open) {
                    int idx = tpl_index_at(ev.x, ev.y);
                    if (idx >= 0) {
                        replace_buffer_with(TEMPLATES[idx].body);
                        g_path[0] = 0;
                        snprintf(g_status_msg, sizeof(g_status_msg),
                                 "loaded template: %s",
                                 TEMPLATES[idx].label);
                        g_status_col = COL_STATUS_FG;
                    }
                    g_tpl_open = 0;
                    g_tpl_hover = -1;
                    render();
                    continue;
                }
                if (ev.y < TOOLBAR_H) {
                    int k = button_at(ev.x, ev.y);
                    if (k) { on_button(k); render(); continue; }
                }
                if (ev.y >= body_y() && ev.y < body_y() + body_h()) {
                    cursor_from_click(ev.x, ev.y);
                    g_anchor = g_cur;
                    g_dragging = 1;
                    ensure_cursor_visible();
                    render();
                }
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
                if (g_anchor == g_cur) sel_clear();
                continue;
            }
            if (ev.mouse_kind == OX_MOUSE_WHEEL) {
                g_scroll -= ev.wheel_delta * 3;
                int total = total_lines();
                int max_scroll = total - vis_lines();
                if (max_scroll < 0) max_scroll = 0;
                if (g_scroll < 0)        g_scroll = 0;
                if (g_scroll > max_scroll) g_scroll = max_scroll;
                render();
                continue;
            }
            continue;
        }

        if (ev.type != OX_EV_KEY) continue;

        if (g_prompt_mode) { prompt_key(ev.ascii, ev.keycode); render(); continue; }
        if (g_find_mode)   { find_key  (ev.ascii, ev.keycode); render(); continue; }

        int ctrl  = (ev.mods & OX_MOD_CTRL)  != 0;
        int shift = (ev.mods & OX_MOD_SHIFT) != 0;

        /* F5 = Run; F3 = find-next when find dialog already had a buf. */
        if (ev.keycode == 63 /*F5*/) {
            on_button(BTN_RUN); render(); continue;
        }
        if (ev.keycode == 61 /*F3*/) {
            if (g_find_len > 0) { find_jump_next(); render(); continue; }
        }

        /* Ctrl+letter shortcuts. */
        if (ctrl && ev.ascii) {
            if (handle_ctrl_letter(ev.ascii)) {
                ensure_cursor_visible();
                render();
                continue;
            }
        }

        /* Ctrl+arrow word jump (Shift extends selection). */
        if (ctrl && ev.keycode == OX_KEY_LEFT) {
            if (shift) sel_begin_if_needed(); else sel_clear();
            cursor_word_left(); render(); continue;
        }
        if (ctrl && ev.keycode == OX_KEY_RIGHT) {
            if (shift) sel_begin_if_needed(); else sel_clear();
            cursor_word_right(); render(); continue;
        }
        if (ctrl && (ev.keycode == OX_KEY_BACKSPACE ||
                     ev.ascii == 0x08 || ev.ascii == 0x7f)) {
            backspace_word(); ensure_cursor_visible(); render(); continue;
        }

        /* Plain navigation. */
        if (ev.keycode == OX_KEY_LEFT)   { if (shift) sel_begin_if_needed(); else sel_clear(); cursor_left();  render(); continue; }
        if (ev.keycode == OX_KEY_RIGHT)  { if (shift) sel_begin_if_needed(); else sel_clear(); cursor_right(); render(); continue; }
        if (ev.keycode == OX_KEY_UP)     { if (shift) sel_begin_if_needed(); else sel_clear(); cursor_up();    render(); continue; }
        if (ev.keycode == OX_KEY_DOWN)   { if (shift) sel_begin_if_needed(); else sel_clear(); cursor_down();  render(); continue; }
        if (ev.keycode == OX_KEY_HOME)   { if (shift) sel_begin_if_needed(); else sel_clear(); cursor_home();  render(); continue; }
        if (ev.keycode == OX_KEY_END)    { if (shift) sel_begin_if_needed(); else sel_clear(); cursor_end();   render(); continue; }
        if (ev.keycode == OX_KEY_PGUP)   { if (shift) sel_begin_if_needed(); else sel_clear(); cursor_pgup();  render(); continue; }
        if (ev.keycode == OX_KEY_PGDN)   { if (shift) sel_begin_if_needed(); else sel_clear(); cursor_pgdn();  render(); continue; }

        if (ev.keycode == OX_KEY_BACKSPACE ||
            ev.ascii == 0x08 || ev.ascii == 0x7f) {
            backspace(); ensure_cursor_visible(); render(); continue;
        }
        if (ev.keycode == OX_KEY_DELETE) {
            forward_delete(); ensure_cursor_visible(); render(); continue;
        }
        if (ev.keycode == OX_KEY_ENTER || ev.ascii == '\n' || ev.ascii == '\r') {
            delete_selection_if_any();
            char indent[32];
            int n = auto_indent_string(indent, sizeof(indent));
            char tmp[40];
            tmp[0] = '\n';
            if (n > 0) memcpy(tmp + 1, indent, (size_t)n);
            buf_insert_at_cur(tmp, n + 1);
            ensure_cursor_visible(); render(); continue;
        }
        if (ev.keycode == OX_KEY_TAB) {
            delete_selection_if_any();
            buf_insert_at_cur("  ", TAB_WIDTH);
            ensure_cursor_visible(); render(); continue;
        }
        if (ev.ascii >= 0x20 && ev.ascii < 0x7f) {
            delete_selection_if_any();
            char c = (char)ev.ascii;
            /* Smart over-type: if user types the SAME closing char and
             * the cursor is already on it, just skip forward. Keeps
             * round-trips natural after an auto-pair. */
            if ((c == '}' || c == ']' || c == ')' || c == '"' ||
                 c == '\'' || c == '`') &&
                g_cur < g_len && g_buf[g_cur] == c) {
                g_cur++;
                ensure_cursor_visible();
                g_status_msg[0] = 0;
                render();
                continue;
            }
            /* Auto-pair: insert the matching close and leave caret in
             * the middle. For quotes only do it when not already in a
             * string (avoid doubling a string-end quote that the user
             * just typed). */
            char close_c = 0;
            if      (c == '{') close_c = '}';
            else if (c == '[') close_c = ']';
            else if (c == '(') close_c = ')';
            else if (c == '"' || c == '\'' || c == '`') {
                /* Skip auto-pair if the cursor is inside a string
                 * literal already — typing a quote there should just
                 * insert one. */
                int in_str = (g_cur > 0 && g_color[g_cur - 1] == TOK_STRING);
                if (!in_str) close_c = c;
            }
            char pair[2] = { c, close_c };
            if (close_c) {
                buf_insert_at_cur(pair, 2);
                /* Move cursor back to between the two chars. */
                g_cur--;
            } else {
                buf_insert_at_cur(&c, 1);
            }
            g_status_msg[0] = 0;
            ensure_cursor_visible();
            render();
            continue;
        }
    }

    ox_window_destroy(g_win);
    return 0;
}
