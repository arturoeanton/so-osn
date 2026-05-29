/*
 * /bin/oxstudio — JS playground for the Ox window system.
 *
 * Three zones:
 *   ┌─ toolbar:  [Run] [Save] [Load] [New]                    [help]
 *   ├─ editor:   multi-line code editor with syntax colouring
 *   └─ status:   path · line:col · last error
 *
 * "Run" writes the buffer to /tmp/_studio_<pid>.js and spawns
 * /bin/oxjs against that file. The script opens its own Ox window via
 * the oxjs API, so the studio stays alive and can be re-run after
 * editing.
 *
 * Files live under /home/studio/ (auto-created on first save). Load
 * accepts either a bare name (resolved relative to /home/studio) or an
 * absolute path so you can pull in the showcase JS apps under
 * /home/apps/ for editing.
 *
 * Resizable: opts into the Ox real-resize protocol. Cell metrics stay
 * fixed; growing the window shows more rows and columns.
 *
 * Storage model: one linear buffer (g_buf[g_len]). Cursor is a byte
 * offset (0..g_len). Lines are derived by walking '\n'. Same pattern
 * as oxnotepad — see that file for the rationale.
 *
 * Highlighting: a single pass over the buffer fills g_color[] with one
 * byte per source byte (a TOK_* class). The render loop reads g_color
 * per-cell and groups consecutive same-colour cells into one draw call.
 */

#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <ox.h>
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

#define INIT_W      820
#define INIT_H      560
#define MARGIN_X     8
#define CHAR_W       8
#define LINE_H      12
#define GUTTER_W    (5 * CHAR_W + 4)
#define TOOLBAR_H   28
#define STATUS_H    16
#define BUF_MAX     (32 * 1024)
#define MIN_W       420
#define MIN_H       240
#define PATH_MAX_LEN 128

/* --- palette: Adwaita dark + syntax ------------------------------- */

#define COL_BG          OX_RGB( 30,  30,  36)
#define COL_GUTTER_BG   OX_RGB( 24,  24,  28)
#define COL_GUTTER_FG   OX_RGB(110, 115, 132)
#define COL_TEXT        OX_RGB(214, 214, 220)
#define COL_CURSOR      OX_RGB(166, 226, 255)
#define COL_SEL_BG      OX_RGB( 55,  85, 130)
#define COL_SEL_FG      OX_RGB(240, 240, 245)

#define COL_TB_BG       OX_RGB( 45,  46,  56)
#define COL_TB_BORDER   OX_RGB( 18,  18,  22)
#define COL_BTN_BG      OX_RGB( 70,  72,  90)
#define COL_BTN_BG_HI   OX_RGB(100, 115, 165)
#define COL_BTN_FG      OX_RGB(228, 228, 234)

#define COL_STATUS_BG   OX_RGB( 38,  60,  92)
#define COL_STATUS_FG   OX_RGB(225, 230, 240)
#define COL_STATUS_ERR  OX_RGB(255, 150, 150)

#define COL_SYN_KW      OX_RGB(186, 222, 252)   /* function/var/let/... */
#define COL_SYN_STR     OX_RGB(166, 226, 138)
#define COL_SYN_NUM     OX_RGB(247, 200, 124)
#define COL_SYN_COMMENT OX_RGB(118, 130, 152)
#define COL_SYN_IDENT   COL_TEXT

/* --- token classes (one byte per source byte) --------------------- */
enum {
    TOK_TEXT = 0,
    TOK_KEYWORD,
    TOK_STRING,
    TOK_NUMBER,
    TOK_COMMENT,
};

/* --- state -------------------------------------------------------- */

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

/* Modal prompt (filename) — when active, absorbs keys + draws over
 * the status bar. mode: 0=off, 1=save-as, 2=load. */
static int  g_prompt_mode = 0;
static char g_prompt_buf[PATH_MAX_LEN];
static int  g_prompt_len  = 0;
static int  g_prompt_caret = 0;

/* Toolbar buttons. Set per render so click hit-testing matches the
 * current layout (it shifts on resize). */
typedef struct { int x, y, w, h; const char *label; int kind; } btn_t;
enum { BTN_RUN = 1, BTN_SAVE, BTN_LOAD, BTN_NEW };
#define BTN_MAX 4
static btn_t g_btns[BTN_MAX];
static int   g_n_btns = 0;
static int   g_hover_btn = 0;

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

static void retokenize(void) {
    int i = 0;
    while (i < g_len) {
        unsigned char c = (unsigned char)g_buf[i];

        /* line comment */
        if (c == '/' && i + 1 < g_len && g_buf[i+1] == '/') {
            int j = i;
            while (j < g_len && g_buf[j] != '\n') {
                g_color[j++] = TOK_COMMENT;
            }
            i = j;
            continue;
        }
        /* block comment */
        if (c == '/' && i + 1 < g_len && g_buf[i+1] == '*') {
            int j = i;
            g_color[j++] = TOK_COMMENT;
            g_color[j++] = TOK_COMMENT;
            while (j < g_len) {
                g_color[j] = TOK_COMMENT;
                if (g_buf[j] == '*' && j + 1 < g_len && g_buf[j+1] == '/') {
                    g_color[j+1] = TOK_COMMENT;
                    j += 2;
                    break;
                }
                j++;
            }
            i = j;
            continue;
        }
        /* string */
        if (c == '"' || c == '\'' || c == '`') {
            char q = (char)c;
            int j = i;
            g_color[j++] = TOK_STRING;
            while (j < g_len) {
                g_color[j] = TOK_STRING;
                if (g_buf[j] == '\\' && j + 1 < g_len) {
                    g_color[j+1] = TOK_STRING;
                    j += 2;
                    continue;
                }
                if (g_buf[j] == q) { j++; break; }
                /* unterminated string ends at newline for safety */
                if (g_buf[j] == '\n' && q != '`') { break; }
                j++;
            }
            i = j;
            continue;
        }
        /* number */
        if (c >= '0' && c <= '9') {
            int j = i;
            while (j < g_len) {
                unsigned char d = (unsigned char)g_buf[j];
                if ((d >= '0' && d <= '9') || d == '.' || d == 'x' ||
                    d == 'X' || (d >= 'a' && d <= 'f') ||
                    (d >= 'A' && d <= 'F')) {
                    g_color[j++] = TOK_NUMBER;
                } else break;
            }
            i = j;
            continue;
        }
        /* identifier / keyword */
        if (is_id_start(c)) {
            int j = i;
            while (j < g_len && is_id_cont((unsigned char)g_buf[j])) j++;
            int klass = is_kw(g_buf + i, j - i) ? TOK_KEYWORD : TOK_TEXT;
            for (int k = i; k < j; k++) g_color[k] = (unsigned char)klass;
            i = j;
            continue;
        }
        /* anything else (operator, whitespace, punct) */
        g_color[i++] = TOK_TEXT;
    }
}

/* --- buffer mutation: delete + insert ----------------------------- */
static void buf_delete_range(int lo, int hi) {
    if (lo < 0) lo = 0;
    if (hi > g_len) hi = g_len;
    if (lo >= hi) return;
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
    memmove(g_buf + g_cur - 1, g_buf + g_cur, (size_t)(g_len - g_cur));
    g_len--; g_cur--;
    g_dirty = 1;
}

static void forward_delete(void) {
    if (sel_active()) { delete_selection_if_any(); return; }
    if (g_cur >= g_len) return;
    memmove(g_buf + g_cur, g_buf + g_cur + 1, (size_t)(g_len - g_cur - 1));
    g_len--;
    g_dirty = 1;
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
    int prev_start = line_start_off(line - 1);
    int prev_len   = line_length(line - 1);
    g_cur = prev_start + (col < prev_len ? col : prev_len);
    ensure_cursor_visible();
}
static void cursor_down(void) {
    int line, col; byte_to_lc(g_cur, &line, &col);
    if (line + 1 >= total_lines()) return;
    int next_start = line_start_off(line + 1);
    int next_len   = line_length(line + 1);
    g_cur = next_start + (col < next_len ? col : next_len);
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

/* --- path resolution (Load): bare name → /home/studio/<name>, else
 * pass through (lets user open /home/apps/foo.js for editing). ----- */
static void resolve_path(const char *raw, char *out, size_t cap) {
    if (!raw || !raw[0]) { out[0] = 0; return; }
    if (raw[0] == '/') {
        strncpy(out, raw, cap - 1); out[cap - 1] = 0;
        return;
    }
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
    return 0;
}

static int save_file(const char *path) {
    /* Ensure /home/studio/ exists (parent of typical save). Ignore
     * errors — open() below will surface real problems. */
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

/* --- Run: dump buffer to a side file, spawn /bin/oxjs against it -- */
static void run_buffer(void) {
    /* Use a stable per-pid filename. mkdir is best-effort. */
    mkdir("/home/studio", 0755);
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
    /* osn_spawn packs args verbatim into the child's argv[1]. oxjs
     * reads argv[1] as the script path. */
    long pid = osn_spawn("/bin/oxjs", tmp, NULL, -1, -1);
    if (pid < 0) {
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "run: spawn failed (%s)", strerror(errno));
        g_status_col = COL_STATUS_ERR;
        return;
    }
    snprintf(g_status_msg, sizeof(g_status_msg),
             "▶ running %s as pid %ld", tmp, pid);
    g_status_col = COL_STATUS_FG;
}

/* --- starter snippet for "New" ------------------------------------ */
static const char *STARTER =
    "// oxstudio — JS playground (Duktape)\n"
    "// Press Run to launch. See ox.* API for drawing, fs, http, etc.\n"
    "\n"
    "ox.window(\"Hello\", 320, 200);\n"
    "\n"
    "ox.onPaint(function() {\n"
    "  ox.clear(\"#222\");\n"
    "  ox.text(20, 20, \"Hello from oxstudio!\", \"#cde\");\n"
    "});\n";

static void buffer_new(void) {
    g_len = 0;
    g_cur = 0;
    g_scroll = 0;
    sel_clear();
    g_dirty = 0;
    g_path[0] = 0;
    buf_insert_at_cur(STARTER, (int)strlen(STARTER));
    g_cur = 0;
    g_dirty = 0;
}

/* --- toolbar ------------------------------------------------------ */
static void layout_buttons(void) {
    struct { const char *label; int kind; int w; } defs[] = {
        { " \xe2\x96\xb6 Run ", BTN_RUN,  72 }, /* ▶ may render as fallback */
        { " Save ",             BTN_SAVE, 60 },
        { " Load ",             BTN_LOAD, 60 },
        { " New ",              BTN_NEW,  56 },
    };
    /* Some font tilesets don't have the ▶ glyph — use a plain
     * ASCII '>' as the label so anybody can read it. */
    static const char *RUN_LABEL = " > Run ";
    defs[0].label = RUN_LABEL;
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
        x += defs[i].w + 6;
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

/* --- render ------------------------------------------------------- */
static void render(void) {
    retokenize();

    /* Toolbar. */
    ox_draw_rect(g_win, 0, 0, g_win_w, TOOLBAR_H, COL_TB_BG);
    ox_draw_rect(g_win, 0, TOOLBAR_H - 1, g_win_w, 1, COL_TB_BORDER);
    for (int i = 0; i < g_n_btns; i++) {
        btn_t *b = &g_btns[i];
        uint32_t bg = (g_hover_btn == b->kind) ? COL_BTN_BG_HI : COL_BTN_BG;
        ox_draw_rect(g_win, b->x, b->y, b->w, b->h, bg);
        ox_draw_rect(g_win, b->x, b->y, b->w, 1, COL_TB_BORDER);
        ox_draw_rect(g_win, b->x, b->y + b->h - 1, b->w, 1, COL_TB_BORDER);
        int tx = b->x + 4;
        int ty = b->y + (b->h - 8) / 2;
        ox_draw_text(g_win, tx, ty, b->label, COL_BTN_FG);
    }

    /* Body background + gutter. */
    int by = body_y();
    int bh = body_h();
    ox_draw_rect(g_win, 0, by, g_win_w, bh, COL_BG);
    ox_draw_rect(g_win, 0, by, GUTTER_W + MARGIN_X, bh, COL_GUTTER_BG);

    /* Walk to the first visible line. */
    int line = 0;
    int i = 0;
    while (i < g_len && line < g_scroll) {
        if (g_buf[i] == '\n') line++;
        i++;
    }

    int cur_line, cur_col; byte_to_lc(g_cur, &cur_line, &cur_col);
    int slo = sel_active() ? sel_lo() : -1;
    int shi = sel_active() ? sel_hi() : -1;
    int vc = vis_cols();

    int row = 0;
    int VL = vis_lines();
    int total = total_lines();
    char *rowbuf = (char *)alloca((size_t)vc + 1);
    while (row < VL) {
        int y = by + row * LINE_H;
        int x_base = body_x();

        /* Gutter number. */
        if (line < total) {
            char numbuf[8];
            snprintf(numbuf, sizeof(numbuf), "%4d", line + 1);
            ox_draw_text(g_win, MARGIN_X, y, numbuf, COL_GUTTER_FG);
        }

        /* First, paint selection backgrounds for this line. */
        if (slo >= 0) {
            int ix = i, col = 0;
            while (ix < g_len && g_buf[ix] != '\n' && col < vc) {
                if (ix >= slo && ix < shi) {
                    ox_draw_rect(g_win, x_base + col * CHAR_W, y,
                                 CHAR_W, LINE_H, COL_SEL_BG);
                }
                ix++; col++;
            }
        }

        /* Then text — group runs of same colour into one ox_draw_text. */
        int col = 0;
        int j = i;
        while (j < g_len && g_buf[j] != '\n' && col < vc) {
            int run_col   = col;
            unsigned char klass = g_color[j];
            int in_sel = (slo >= 0 && j >= slo && j < shi);
            /* extend run while same class AND same sel state AND not newline */
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

        /* Skip rest of overlong line. */
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
    char status[200];
    if (g_status_msg[0]) {
        snprintf(status, sizeof(status), " %s", g_status_msg);
        ox_draw_text(g_win, MARGIN_X, g_win_h - 12, status, g_status_col);
    } else {
        snprintf(status, sizeof(status),
                 " %s%s  %d:%d  %d bytes  ^S save  ^O load  F5 run  ^Q quit",
                 g_dirty ? "* " : "  ",
                 g_path[0] ? g_path : "(unsaved)",
                 cur_line + 1, cur_col + 1, g_len);
        ox_draw_text(g_win, MARGIN_X, g_win_h - 12, status, COL_STATUS_FG);
    }

    /* Prompt overlay. */
    if (g_prompt_mode) {
        int dlg_h = LINE_H + 8;
        int dlg_y = g_win_h - STATUS_H - dlg_h - 2;
        ox_draw_rect(g_win, 0, dlg_y, g_win_w, dlg_h,
                     OX_RGB(70, 60, 30));
        ox_draw_rect(g_win, 0, dlg_y, g_win_w, 1, OX_RGB(0, 0, 0));
        ox_draw_rect(g_win, 0, dlg_y + dlg_h - 1, g_win_w, 1,
                     OX_RGB(0, 0, 0));
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

    ox_present(g_win);
}

/* --- prompt handling ---------------------------------------------- */
static void prompt_open(int mode, const char *initial) {
    g_prompt_mode = mode;
    g_prompt_buf[0] = 0;
    g_prompt_len = 0;
    g_prompt_caret = 0;
    if (initial && initial[0]) {
        /* If the path is /home/studio/<name>, strip the prefix so the
         * user can edit just the name. */
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
    g_prompt_mode = 0;
    g_prompt_buf[0] = 0;
    g_prompt_len = 0;
    g_prompt_caret = 0;
}

static void prompt_commit(void) {
    int mode = g_prompt_mode;
    if (mode == 0 || g_prompt_len == 0) { prompt_close(); return; }
    char resolved[PATH_MAX_LEN];
    resolve_path(g_prompt_buf, resolved, sizeof(resolved));
    if (mode == 1) {  /* save-as */
        if (save_file(resolved) == 0) {
            snprintf(g_status_msg, sizeof(g_status_msg),
                     "saved %s (%d bytes)", resolved, g_len);
            g_status_col = COL_STATUS_FG;
        } else {
            snprintf(g_status_msg, sizeof(g_status_msg),
                     "save failed: %s (%s)", resolved, strerror(errno));
            g_status_col = COL_STATUS_ERR;
        }
    } else if (mode == 2) {  /* load */
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
            g_prompt_caret--;
            g_prompt_len--;
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
        g_prompt_caret++;
        g_prompt_len++;
        g_prompt_buf[g_prompt_len] = 0;
    }
}

/* --- toolbar action dispatch -------------------------------------- */
static void on_button(int kind) {
    switch (kind) {
    case BTN_RUN:  run_buffer(); break;
    case BTN_SAVE:
        if (g_path[0])  {
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

    /* If launched with a path arg, load it; otherwise seed the
     * starter snippet so the user has something to Run. */
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
            /* Track hover for toolbar buttons. */
            int hov = (ev.y < TOOLBAR_H) ? button_at(ev.x, ev.y) : 0;
            if (hov != g_hover_btn) { g_hover_btn = hov; render(); }

            if (ev.mouse_kind == OX_MOUSE_DOWN && (ev.buttons & 0x01)) {
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

        /* Modal prompt absorbs all keys while open. */
        if (g_prompt_mode) {
            prompt_key(ev.ascii, ev.keycode);
            render();
            continue;
        }

        int ctrl = (ev.mods & OX_MOD_CTRL) != 0;
        int shift = (ev.mods & OX_MOD_SHIFT) != 0;

        /* F5 = Run (independent of Ctrl). */
        if (ev.keycode == OX_KEY_F4 || ev.keycode == 63 /*F5*/ ||
            ev.keycode == 64 /*F6*/) {
            run_buffer(); render(); continue;
        }

        if (ctrl) {
            switch (ev.ascii) {
            case 's': case 'S': on_button(BTN_SAVE); render(); continue;
            case 'o': case 'O': on_button(BTN_LOAD); render(); continue;
            case 'r': case 'R': on_button(BTN_RUN);  render(); continue;
            case 'n': case 'N': on_button(BTN_NEW);  render(); continue;
            case 'q': case 'Q':
                if (g_dirty && g_path[0]) save_file(g_path);
                g_quit = 1; continue;
            case 'a': case 'A':
                g_anchor = 0; g_cur = g_len; render(); continue;
            case 'c': case 'C':
                if (sel_active()) ox_clipboard_set(g_buf + sel_lo(),
                                                    sel_hi() - sel_lo());
                continue;
            case 'x': case 'X':
                if (sel_active()) {
                    ox_clipboard_set(g_buf + sel_lo(),
                                      sel_hi() - sel_lo());
                    delete_selection_if_any();
                    ensure_cursor_visible(); render();
                }
                continue;
            case 'v': case 'V': {
                char clip[1024];
                int n = ox_clipboard_get(clip, sizeof(clip));
                if (n > 0) {
                    delete_selection_if_any();
                    buf_insert_at_cur(clip, n);
                    ensure_cursor_visible();
                    render();
                }
                continue;
            }
            default: break;
            }
        }

        /* Navigation keys. */
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
            char nl = '\n';
            buf_insert_at_cur(&nl, 1);
            ensure_cursor_visible(); render(); continue;
        }
        if (ev.keycode == OX_KEY_TAB) {
            delete_selection_if_any();
            buf_insert_at_cur("  ", 2);  /* soft tab = 2 spaces */
            ensure_cursor_visible(); render(); continue;
        }
        if (ev.ascii >= 0x20 && ev.ascii < 0x7f) {
            delete_selection_if_any();
            char c = (char)ev.ascii;
            buf_insert_at_cur(&c, 1);
            /* Clear status only when user starts typing again. */
            g_status_msg[0] = 0;
            ensure_cursor_visible();
            render();
            continue;
        }
    }

    ox_window_destroy(g_win);
    return 0;
}
