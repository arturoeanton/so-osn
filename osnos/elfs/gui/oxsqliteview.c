/*
 * /bin/oxsqliteview — SQLite browser/runner for the Ox window system.
 *
 * Layout:
 *   ┌───────────────────────────────────────────────────────────────┐
 *   │ DB: /home/demo.db                              [Run]   status │ ← toolbar
 *   ├─────────────┬─────────────────────────────────────────────────┤
 *   │ Tables      │ SELECT * FROM books LIMIT 10;                   │ ← query editor (multi-line)
 *   │  books      │                                                 │
 *   │  users      │                                                 │
 *   │  ...        ├─────────────────────────────────────────────────┤
 *   │             │ id | title         | author     | year          │ ← header row
 *   │             │  1 | Dune          | Herbert    | 1965          │ ← result grid
 *   │             │  2 | Foundation    | Asimov     | 1951          │
 *   │             │ ...                                             │
 *   └─────────────┴─────────────────────────────────────────────────┘
 *
 * Execution model: spawn `/bin/sqlite3 -header -separator '|' <db>`
 * via popen("r") with stdin pointed at a tempfile holding the query.
 * Read tab-separated rows from stdout, parse into a 2-D table, render.
 *
 * No SQLite library link — we just shell out. Simpler integration and
 * gets us /bin/sqlite3's full feature set (.tables, .schema, joins,
 * subqueries, UDFs, ...) for free.
 */

#include <errno.h>
#include <fcntl.h>
#include <ox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------- geometry / palette ------------------------------ */
#define WIN_W       880
#define WIN_H       620
#define TOOLBAR_H    36
#define STATUS_H     16
#define LEFT_W      180
#define QUERY_H     140
#define MARGIN_X      8
#define LINE_H       12
#define CHAR_W        8

#define BODY_Y       TOOLBAR_H
#define LEFT_X        0
#define RIGHT_X      LEFT_W
#define RIGHT_W     (WIN_W - LEFT_W)
#define QUERY_Y      BODY_Y
#define QUERY_X      RIGHT_X
#define GRID_Y      (BODY_Y + QUERY_H + 6)
#define GRID_X       RIGHT_X
#define GRID_W      (RIGHT_W)
#define GRID_H      (WIN_H - GRID_Y - STATUS_H)

/* Adwaita-ish palette. */
#define COL_TOOLBAR     OX_RGB( 30,  30,  35)
#define COL_TOOLBAR_FG  OX_RGB(230, 230, 230)
#define COL_LEFT_BG     OX_RGB( 42,  42,  48)
#define COL_LEFT_FG     OX_RGB(220, 220, 220)
#define COL_LEFT_HOV    OX_RGB( 60,  60,  72)
#define COL_LEFT_SEL    OX_RGB( 53, 132, 228)
#define COL_QUERY_BG    OX_RGB(255, 255, 255)
#define COL_QUERY_FG    OX_RGB( 20,  20,  25)
#define COL_GRID_BG     OX_RGB(252, 252, 250)
#define COL_GRID_FG     OX_RGB( 20,  20,  25)
#define COL_GRID_HEAD   OX_RGB(230, 232, 240)
#define COL_GRID_ALT    OX_RGB(244, 245, 250)
#define COL_GRID_LINE   OX_RGB(200, 200, 210)
#define COL_RUN_BTN     OX_RGB( 53, 132, 228)
#define COL_RUN_BTN_HOV OX_RGB( 80, 158, 255)
#define COL_RUN_BTN_FG  OX_RGB(255, 255, 255)
#define COL_STATUS_BG   OX_RGB( 40,  40,  50)
#define COL_STATUS_FG   OX_RGB(220, 220, 220)
#define COL_STATUS_ERR  OX_RGB(255, 120, 120)
#define COL_CARET       OX_RGB( 53, 132, 228)
#define COL_DIVIDER     OX_RGB( 18,  18,  22)

/* ---------------- limits ---------------------------------------- */
#define DBPATH_MAX     256
#define QUERY_MAX     8192
#define TABLES_MAX      64
#define TABNAME_MAX     64
#define RESULT_MAX  (64 * 1024)
#define COLS_MAX        24
#define ROWS_MAX       512
#define CELL_MAX       128

/* ---------------- state ----------------------------------------- */
static ox_win_t g_win;
static char     g_db[DBPATH_MAX] = "/home/demo.db";

/* Query buffer + cursor (notepad-lite). */
static char g_query[QUERY_MAX];
static int  g_qlen = 0;
static int  g_qcur = 0;
static int  g_q_scroll = 0;
static int  g_focus_query = 1;   /* keyboard focus: query=1, none=0 */

/* Table list. */
static char g_tables[TABLES_MAX][TABNAME_MAX];
static int  g_ntables = 0;
static int  g_hover_table = -1;
static int  g_sel_table = -1;

/* Raw sqlite3 stdout buffer. */
static char g_raw[RESULT_MAX];
static int  g_raw_len = 0;

/* Parsed result grid. */
static int  g_nrows = 0;      /* including header row index 0 */
static int  g_ncols = 0;
static int  g_col_w[COLS_MAX];          /* width in chars */
static char g_cells[ROWS_MAX][COLS_MAX][CELL_MAX];
static int  g_grid_scroll = 0;     /* topmost data row visible (0-based, excludes header) */
static int  g_grid_xscroll = 0;    /* horizontal offset in cells */
static int  g_run_hover = 0;

static char g_status[160] = "ready";
static int  g_status_err = 0;

/* ---------------- query editor helpers -------------------------- */

static void q_insert(char c) {
    if (g_qlen + 1 >= QUERY_MAX) return;
    memmove(g_query + g_qcur + 1, g_query + g_qcur,
            (size_t)(g_qlen - g_qcur));
    g_query[g_qcur++] = c;
    g_qlen++;
    g_query[g_qlen] = 0;
}

static void q_backspace(void) {
    if (g_qcur == 0) return;
    memmove(g_query + g_qcur - 1, g_query + g_qcur,
            (size_t)(g_qlen - g_qcur));
    g_qcur--; g_qlen--;
    g_query[g_qlen] = 0;
}

static void q_delete(void) {
    if (g_qcur >= g_qlen) return;
    memmove(g_query + g_qcur, g_query + g_qcur + 1,
            (size_t)(g_qlen - g_qcur - 1));
    g_qlen--;
    g_query[g_qlen] = 0;
}

static int q_visible_lines(void) {
    return (QUERY_H - 8) / LINE_H;
}
static int q_visible_cols(void) {
    return (RIGHT_W - 12) / CHAR_W;
}

static void q_byte_to_lc(int off, int *line, int *col) {
    int L = 0, C = 0;
    for (int i = 0; i < off && i < g_qlen; i++) {
        if (g_query[i] == '\n') { L++; C = 0; }
        else C++;
    }
    *line = L; *col = C;
}

static int q_total_lines(void) {
    int n = 1;
    for (int i = 0; i < g_qlen; i++) if (g_query[i] == '\n') n++;
    return n;
}

static int q_line_start(int line) {
    if (line <= 0) return 0;
    int cur = 0;
    for (int i = 0; i < g_qlen; i++) {
        if (g_query[i] == '\n') {
            cur++;
            if (cur == line) return i + 1;
        }
    }
    return g_qlen;
}

static int q_line_length(int line) {
    int s = q_line_start(line);
    int e = s;
    while (e < g_qlen && g_query[e] != '\n') e++;
    return e - s;
}

static void q_ensure_visible(void) {
    int line, col;
    q_byte_to_lc(g_qcur, &line, &col);
    int vis = q_visible_lines();
    if (line < g_q_scroll)        g_q_scroll = line;
    if (line >= g_q_scroll + vis) g_q_scroll = line - vis + 1;
    if (g_q_scroll < 0)           g_q_scroll = 0;
    (void)col;
}

static void q_left(void)  { if (g_qcur > 0) g_qcur--; }
static void q_right(void) { if (g_qcur < g_qlen) g_qcur++; }
static void q_up(void) {
    int line, col; q_byte_to_lc(g_qcur, &line, &col);
    if (line == 0) { g_qcur = 0; return; }
    int prev_len = q_line_length(line - 1);
    int target = col < prev_len ? col : prev_len;
    g_qcur = q_line_start(line - 1) + target;
}
static void q_down(void) {
    int line, col; q_byte_to_lc(g_qcur, &line, &col);
    int last = q_total_lines() - 1;
    if (line >= last) { g_qcur = g_qlen; return; }
    int next_len = q_line_length(line + 1);
    int target = col < next_len ? col : next_len;
    g_qcur = q_line_start(line + 1) + target;
}
static void q_home(void) {
    int line, col; q_byte_to_lc(g_qcur, &line, &col);
    g_qcur = q_line_start(line);
}
static void q_end(void) {
    int line, col; q_byte_to_lc(g_qcur, &line, &col);
    g_qcur = q_line_start(line) + q_line_length(line);
}

/* ---------------- sqlite3 spawn ------------------------------------ */

/* Run a SQL string against the configured DB. Writes the raw
 * sqlite3 stdout into g_raw[]. Returns -1 on spawn error. With
 * `with_header = 1` we get a header row in the first line of output. */
static int sqlite_run(const char *sql, int with_header) {
    const char *tmp = "/tmp/oxsqv.sql";
    FILE *qf = fopen(tmp, "w");
    if (!qf) {
        snprintf(g_status, sizeof(g_status),
                 "fopen tmp: %s", strerror(errno));
        g_status_err = 1;
        return -1;
    }
    /* Write the actual SQL we received. Ensure it terminates in `;`
     * before the `.exit` dot-command, otherwise sqlite3 parses the
     * two as a single (invalid) statement and emits a syntax error.
     * Looking at strlen(sql) — NOT the editor globals — is what
     * matters here. */
    size_t sql_len = strlen(sql);
    fputs(sql, qf);
    if (sql_len == 0 || sql[sql_len - 1] != ';') fputc(';', qf);
    fputc('\n', qf);
    fputs(".exit\n", qf);
    fclose(qf);

    char cmd[512];
    if (with_header) {
        snprintf(cmd, sizeof(cmd),
            "sqlite3 -header -separator '|' '%s' < %s 2>&1",
            g_db, tmp);
    } else {
        snprintf(cmd, sizeof(cmd),
            "sqlite3 -separator '|' '%s' < %s 2>&1",
            g_db, tmp);
    }

    FILE *p = popen(cmd, "r");
    if (!p) {
        snprintf(g_status, sizeof(g_status),
                 "popen sqlite3: %s", strerror(errno));
        g_status_err = 1;
        return -1;
    }
    g_raw_len = 0;
    while (g_raw_len < RESULT_MAX - 1) {
        int r = (int)fread(g_raw + g_raw_len, 1,
                            (size_t)(RESULT_MAX - 1 - g_raw_len), p);
        if (r <= 0) break;
        g_raw_len += r;
    }
    g_raw[g_raw_len] = 0;
    pclose(p);
    return 0;
}

/* Parse g_raw into g_cells[rows][cols][CELL_MAX], filling g_nrows,
 * g_ncols, g_col_w[]. First row is the header. */
static void parse_result(void) {
    g_nrows = 0;
    g_ncols = 0;
    for (int i = 0; i < COLS_MAX; i++) g_col_w[i] = 4;

    int i = 0;
    while (i < g_raw_len && g_nrows < ROWS_MAX) {
        /* Skip blank lines that sqlite3 sometimes emits. */
        if (g_raw[i] == '\n') { i++; continue; }
        int col = 0, ci = 0;
        char cell[CELL_MAX];
        while (i < g_raw_len && g_raw[i] != '\n') {
            char c = g_raw[i++];
            if (c == '|' && col + 1 < COLS_MAX) {
                cell[ci] = 0;
                memcpy(g_cells[g_nrows][col], cell,
                       (size_t)(ci + 1));
                int w = ci;
                if (w > g_col_w[col]) g_col_w[col] = w;
                col++;
                ci = 0;
            } else if (ci + 1 < CELL_MAX) {
                cell[ci++] = c;
            }
        }
        cell[ci] = 0;
        memcpy(g_cells[g_nrows][col], cell, (size_t)(ci + 1));
        if (ci > g_col_w[col]) g_col_w[col] = ci;
        col++;
        if (col > g_ncols) g_ncols = col;
        g_nrows++;
        if (i < g_raw_len && g_raw[i] == '\n') i++;
    }
    /* Cap any single column to a sensible width. */
    for (int c = 0; c < g_ncols; c++) {
        if (g_col_w[c] > 40) g_col_w[c] = 40;
        if (g_col_w[c] <  4) g_col_w[c] =  4;
    }
    g_grid_scroll = 0;
    g_grid_xscroll = 0;
}

/* Plausibility check for a SQL identifier: starts with [A-Za-z_],
 * rest are [A-Za-z0-9_]. Rejects sqlite3 error output ("Parse error
 * near line 1: ...") that would otherwise sneak in as table names. */
static int looks_like_ident(const char *s, int len) {
    if (len == 0 || len >= TABNAME_MAX) return 0;
    char c0 = s[0];
    if (!((c0 >= 'A' && c0 <= 'Z') ||
          (c0 >= 'a' && c0 <= 'z') || c0 == '_')) return 0;
    for (int i = 1; i < len; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '_')) return 0;
    }
    return 1;
}

/* Load list of tables. Filters non-ident lines so a sqlite3 error
 * doesn't end up populating the sidebar as fake tables. */
static void load_tables(void) {
    if (sqlite_run("SELECT name FROM sqlite_master "
                   "WHERE type='table' ORDER BY name;", 0) < 0) {
        g_ntables = 0;
        return;
    }
    g_ntables = 0;
    int i = 0;
    while (i < g_raw_len && g_ntables < TABLES_MAX) {
        int s = i;
        while (i < g_raw_len && g_raw[i] != '\n') i++;
        int n = i - s;
        if (n > 0 && looks_like_ident(g_raw + s, n)) {
            memcpy(g_tables[g_ntables], g_raw + s, (size_t)n);
            g_tables[g_ntables][n] = 0;
            g_ntables++;
        }
        if (i < g_raw_len && g_raw[i] == '\n') i++;
    }
}

/* Execute the query in g_query[]. */
static void run_query(void) {
    if (g_qlen == 0) {
        snprintf(g_status, sizeof(g_status), "Empty query");
        g_status_err = 1;
        return;
    }
    if (sqlite_run(g_query, 1) < 0) return;
    parse_result();
    /* If sqlite3 emitted "Error:" lines, surface as status_err. */
    if (g_raw_len >= 6 && memcmp(g_raw, "Error:", 6) == 0) {
        g_status_err = 1;
        char err[160];
        snprintf(err, sizeof(err), "%s", g_raw);
        /* Trim newline. */
        for (size_t k = 0; k < sizeof(err); k++)
            if (err[k] == '\n') { err[k] = 0; break; }
        snprintf(g_status, sizeof(g_status), "%s", err);
    } else {
        g_status_err = 0;
        snprintf(g_status, sizeof(g_status),
                 "%d row%s × %d col%s",
                 g_nrows > 0 ? g_nrows - 1 : 0,
                 (g_nrows - 1) == 1 ? "" : "s",
                 g_ncols, g_ncols == 1 ? "" : "s");
    }
}

/* ---------------- toolbar layout helpers --------------------------- */
#define RUN_W   80
#define RUN_H   24
#define RUN_X  (WIN_W - RUN_W - 10)
#define RUN_Y    6

/* ---------------- render ------------------------------------------ */

static void render(void) {
    /* Toolbar. */
    ox_draw_rect(g_win, 0, 0, WIN_W, TOOLBAR_H, COL_TOOLBAR);
    char title[256];
    snprintf(title, sizeof(title), "DB: %s", g_db);
    ox_draw_text(g_win, 10, 10, title, COL_TOOLBAR_FG);

    /* Run button. */
    ox_draw_rect(g_win, RUN_X, RUN_Y, RUN_W, RUN_H,
                 g_run_hover ? COL_RUN_BTN_HOV : COL_RUN_BTN);
    ox_draw_text(g_win, RUN_X + (RUN_W - 5 * CHAR_W) / 2,
                 RUN_Y + 8, "Run F5", COL_RUN_BTN_FG);

    /* Left panel — table list. */
    ox_draw_rect(g_win, 0, BODY_Y, LEFT_W, WIN_H - BODY_Y - STATUS_H,
                 COL_LEFT_BG);
    ox_draw_text(g_win, 8, BODY_Y + 4, "Tables", COL_TOOLBAR_FG);
    for (int i = 0; i < g_ntables; i++) {
        int y = BODY_Y + 22 + i * LINE_H;
        if (y > WIN_H - STATUS_H - LINE_H) break;
        uint32_t bg = COL_LEFT_BG;
        if (i == g_sel_table)      bg = COL_LEFT_SEL;
        else if (i == g_hover_table) bg = COL_LEFT_HOV;
        ox_draw_rect(g_win, 0, y, LEFT_W, LINE_H, bg);
        ox_draw_text(g_win, 12, y + 2, g_tables[i], COL_LEFT_FG);
    }
    /* Divider. */
    ox_draw_rect(g_win, LEFT_W - 1, BODY_Y, 1,
                 WIN_H - BODY_Y - STATUS_H, COL_DIVIDER);

    /* Query area background. */
    ox_draw_rect(g_win, QUERY_X, QUERY_Y, RIGHT_W, QUERY_H,
                 COL_QUERY_BG);
    /* Render visible query lines. */
    {
        int line = 0, i = 0;
        while (i < g_qlen && line < g_q_scroll) {
            if (g_query[i] == '\n') line++;
            i++;
        }
        int cur_line, cur_col;
        q_byte_to_lc(g_qcur, &cur_line, &cur_col);
        int row = 0, vis = q_visible_lines();
        int vis_cols = q_visible_cols();
        while (row < vis) {
            int y = QUERY_Y + 4 + row * LINE_H;
            int x = QUERY_X + 6;
            int col = 0;
            while (i < g_qlen && g_query[i] != '\n') {
                if (col >= vis_cols) {
                    while (i < g_qlen && g_query[i] != '\n') i++;
                    break;
                }
                char s[2] = { g_query[i], 0 };
                ox_draw_text(g_win, x, y, s, COL_QUERY_FG);
                x += CHAR_W;
                col++;
                i++;
            }
            if (line == cur_line && g_focus_query) {
                int cx = QUERY_X + 6 + cur_col * CHAR_W;
                if (cur_col > vis_cols) cx = QUERY_X + 6 + vis_cols * CHAR_W;
                ox_draw_rect(g_win, cx, y, 2, LINE_H - 2, COL_CARET);
            }
            if (i < g_qlen && g_query[i] == '\n') i++;
            line++;
            row++;
        }
    }
    /* Divider between query and grid. */
    ox_draw_rect(g_win, QUERY_X, QUERY_Y + QUERY_H, RIGHT_W, 2,
                 COL_DIVIDER);

    /* Result grid. */
    ox_draw_rect(g_win, GRID_X, GRID_Y, GRID_W, GRID_H, COL_GRID_BG);
    /* Compute column x positions in CHAR_W units. */
    int grid_w_chars = GRID_W / CHAR_W;
    /* Render header. */
    if (g_nrows > 0 && g_ncols > 0) {
        ox_draw_rect(g_win, GRID_X, GRID_Y, GRID_W, LINE_H + 2,
                     COL_GRID_HEAD);
        int xpos = 2;
        for (int c = g_grid_xscroll; c < g_ncols; c++) {
            int cw = g_col_w[c] + 2;
            if (xpos + cw > grid_w_chars) break;
            ox_draw_text(g_win, GRID_X + xpos * CHAR_W, GRID_Y + 1,
                         g_cells[0][c], COL_GRID_FG);
            xpos += cw;
            ox_draw_rect(g_win,
                         GRID_X + (xpos - 1) * CHAR_W, GRID_Y,
                         1, LINE_H + 2, COL_GRID_LINE);
        }
        /* Render data rows. */
        int visible_rows = (GRID_H - LINE_H - 2) / LINE_H;
        int dy = GRID_Y + LINE_H + 2;
        for (int r = 0; r < visible_rows; r++) {
            int data_idx = g_grid_scroll + r;
            int row_idx = 1 + data_idx;   /* skip header */
            if (row_idx >= g_nrows) break;
            int alt = data_idx & 1;
            ox_draw_rect(g_win, GRID_X, dy, GRID_W, LINE_H,
                         alt ? COL_GRID_ALT : COL_GRID_BG);
            int xp = 2;
            for (int c = g_grid_xscroll; c < g_ncols; c++) {
                int cw = g_col_w[c] + 2;
                if (xp + cw > grid_w_chars) break;
                ox_draw_text(g_win, GRID_X + xp * CHAR_W, dy,
                             g_cells[row_idx][c], COL_GRID_FG);
                xp += cw;
            }
            dy += LINE_H;
        }
    } else {
        ox_draw_text(g_win, GRID_X + 10, GRID_Y + 10,
                     "(no results — click a table or type a query, F5 to run)",
                     COL_GRID_FG);
    }

    /* Status bar. */
    ox_draw_rect(g_win, 0, WIN_H - STATUS_H, WIN_W, STATUS_H,
                 COL_STATUS_BG);
    ox_draw_text(g_win, 8, WIN_H - 12, g_status,
                 g_status_err ? COL_STATUS_ERR : COL_STATUS_FG);

    ox_present(g_win);
}

/* ---------------- input ------------------------------------------- */

static int hit(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int hit_table_at(int x, int y) {
    if (x < 0 || x >= LEFT_W) return -1;
    if (y < BODY_Y + 22) return -1;
    int idx = (y - BODY_Y - 22) / LINE_H;
    if (idx < 0 || idx >= g_ntables) return -1;
    return idx;
}

static void cursor_from_click_query(int x, int y) {
    int row = (y - QUERY_Y - 4) / LINE_H;
    int line = g_q_scroll + row;
    int total = q_total_lines();
    if (line >= total) line = total - 1;
    if (line < 0) line = 0;
    int col = (x - QUERY_X - 6) / CHAR_W;
    if (col < 0) col = 0;
    int ll = q_line_length(line);
    if (col > ll) col = ll;
    g_qcur = q_line_start(line) + col;
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1] && argv[1][0]) {
        size_t L = strlen(argv[1]);
        if (L >= sizeof(g_db)) L = sizeof(g_db) - 1;
        memcpy(g_db, argv[1], L);
        g_db[L] = 0;
    }
    if (ox_init() < 0) return 1;
    char title[80];
    snprintf(title, sizeof(title), "SQLite Viewer — %s", g_db);
    g_win = ox_window_create(WIN_W, WIN_H, title);
    if (g_win < 0) return 1;

    /* Seed query area with a friendly default. */
    snprintf(g_query, sizeof(g_query),
             "-- Click a table on the left to see its rows,\n"
             "-- or type your own SQL here and press F5.\n"
             "SELECT name, type FROM sqlite_master ORDER BY type, name;");
    g_qlen = (int)strlen(g_query);
    g_qcur = g_qlen;

    snprintf(g_status, sizeof(g_status), "loading tables...");
    render();
    load_tables();
    snprintf(g_status, sizeof(g_status),
             "%d table%s in %s",
             g_ntables, g_ntables == 1 ? "" : "s", g_db);
    g_status_err = 0;
    render();

    int quit = 0;
    while (!quit) {
        ox_event_t ev;
        if (!ox_wait_event(&ev)) continue;

        if (ev.type == OX_EV_CLOSE) break;

        if (ev.type == OX_EV_MOUSE) {
            if (ev.mouse_kind == OX_MOUSE_DOWN && (ev.buttons & 0x01)) {
                if (hit(ev.x, ev.y, RUN_X, RUN_Y, RUN_W, RUN_H)) {
                    run_query();
                    render();
                    continue;
                }
                int t = hit_table_at(ev.x, ev.y);
                if (t >= 0) {
                    g_sel_table = t;
                    /* Replace query with default SELECT against the table. */
                    snprintf(g_query, sizeof(g_query),
                             "SELECT * FROM %s LIMIT 100;",
                             g_tables[t]);
                    g_qlen = (int)strlen(g_query);
                    g_qcur = g_qlen;
                    g_q_scroll = 0;
                    run_query();
                    render();
                    continue;
                }
                if (hit(ev.x, ev.y, QUERY_X, QUERY_Y, RIGHT_W, QUERY_H)) {
                    g_focus_query = 1;
                    cursor_from_click_query(ev.x, ev.y);
                    render();
                    continue;
                }
                g_focus_query = 0;
                render();
                continue;
            }
            if (ev.mouse_kind == OX_MOUSE_MOVE) {
                int new_hover_run  = hit(ev.x, ev.y, RUN_X, RUN_Y, RUN_W, RUN_H);
                int new_hover_tab  = hit_table_at(ev.x, ev.y);
                if (new_hover_run != g_run_hover ||
                    new_hover_tab != g_hover_table) {
                    g_run_hover   = new_hover_run;
                    g_hover_table = new_hover_tab;
                    render();
                }
                continue;
            }
            if (ev.mouse_kind == OX_MOUSE_WHEEL) {
                /* Wheel over the grid scrolls rows; over the left
                 * panel could scroll the table list (not yet impl). */
                if (ev.y >= GRID_Y) {
                    int visible = (GRID_H - LINE_H - 2) / LINE_H;
                    int max_scroll = (g_nrows > 1 ? g_nrows - 1 : 0) - visible;
                    if (max_scroll < 0) max_scroll = 0;
                    g_grid_scroll -= ev.wheel_delta * 3;
                    if (g_grid_scroll < 0)          g_grid_scroll = 0;
                    if (g_grid_scroll > max_scroll) g_grid_scroll = max_scroll;
                    render();
                }
                continue;
            }
            continue;
        }

        if (ev.type != OX_EV_KEY) continue;

        /* F5 = run from anywhere. */
        if (ev.keycode == 63 /* F5 */ ||
            ((ev.mods & OX_MOD_CTRL) &&
             (ev.ascii == 'r' || ev.ascii == 'R' || ev.ascii == 0x12))) {
            run_query();
            render();
            continue;
        }

        if (g_focus_query) {
            if (ev.ascii == '\b' || ev.keycode == OX_KEY_BACKSPACE) {
                q_backspace(); q_ensure_visible(); render(); continue;
            }
            if (ev.keycode == OX_KEY_DELETE) {
                q_delete(); q_ensure_visible(); render(); continue;
            }
            if (ev.keycode == OX_KEY_LEFT)  { q_left();  q_ensure_visible(); render(); continue; }
            if (ev.keycode == OX_KEY_RIGHT) { q_right(); q_ensure_visible(); render(); continue; }
            if (ev.keycode == OX_KEY_UP)    { q_up();    q_ensure_visible(); render(); continue; }
            if (ev.keycode == OX_KEY_DOWN)  { q_down();  q_ensure_visible(); render(); continue; }
            if (ev.keycode == OX_KEY_HOME)  { q_home();  q_ensure_visible(); render(); continue; }
            if (ev.keycode == OX_KEY_END)   { q_end();   q_ensure_visible(); render(); continue; }
            int ch = ev.ascii;
            if (ch == '\r') ch = '\n';
            if (ch == '\n' || (ch >= 0x20 && ch < 0x7f) || ch == '\t') {
                q_insert((char)ch);
                q_ensure_visible();
                render();
                continue;
            }
            continue;
        }

        /* Grid focus: arrows scroll. */
        int visible = (GRID_H - LINE_H - 2) / LINE_H;
        int max_scroll = (g_nrows > 1 ? g_nrows - 1 : 0) - visible;
        if (max_scroll < 0) max_scroll = 0;
        if (ev.keycode == OX_KEY_UP   && g_grid_scroll > 0)             g_grid_scroll--;
        if (ev.keycode == OX_KEY_DOWN && g_grid_scroll < max_scroll)    g_grid_scroll++;
        if (ev.keycode == OX_KEY_PGUP) {
            g_grid_scroll -= visible; if (g_grid_scroll < 0) g_grid_scroll = 0;
        }
        if (ev.keycode == OX_KEY_PGDN) {
            g_grid_scroll += visible; if (g_grid_scroll > max_scroll) g_grid_scroll = max_scroll;
        }
        if (ev.keycode == OX_KEY_HOME) g_grid_scroll = 0;
        if (ev.keycode == OX_KEY_END)  g_grid_scroll = max_scroll;
        render();
    }

    ox_window_destroy(g_win);
    return 0;
}
