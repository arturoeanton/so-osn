/*
 * /bin/less — osnos minimal pager. Derived from /bin/ovi; same line
 * buffer + render loop, but READ-ONLY and with /pattern search.
 *
 *   exec /bin/less FILE
 *
 * Keys:
 *   q          quit
 *   h j k l    move left / down / up / right
 *   arrows     idem
 *   SPACE      page down       b        page up
 *   d          half-page down  u        half-page up
 *   0  $       line start / end
 *   g g        file start      G        file end
 *   /pat       search forward  n / N    next / previous match
 *
 * Search highlights every match on screen in reverse video. The most
 * recent /pattern is remembered; n re-runs it from the current row.
 *
 * Buffer caps: 4096 lines × 1024 cols. File is fully loaded at startup
 * (osnos doesn't have a /dev/tty yet so stdin-from-pipe isn't wired —
 * `cat foo | less` would lose its keyboard. FILE-only mode for now.)
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#define MAX_LINES   4096
#define MAX_COLS    1024

static char    lines[MAX_LINES][MAX_COLS];
static int     line_len[MAX_LINES];
static int     nlines = 1;

static int  cur_row = 0;
static int  top_row = 0;

static int  screen_rows = 25;
static int  screen_cols = 80;
static int  text_rows;

static char filename[128];
static char status_msg[128];

/* search state */
static char pattern[128];
static int  pattern_len = 0;
static int  in_search = 0;          /* 1 while reading "/...." */
static char search_buf[128];
static int  search_len = 0;

static struct termios saved_termios;

/* ---- output buffering (same recipe as ovi — avoid IPC queue overflow). */

#define LESS_OUTBUF 16384
static char   out_buf[LESS_OUTBUF];
static size_t out_pos;

static void less_flush(void) {
    if (out_pos > 0) {
        size_t off = 0;
        while (off < out_pos) {
            ssize_t w = write(1, out_buf + off, out_pos - off);
            if (w <= 0) break;
            off += (size_t)w;
        }
    }
    out_pos = 0;
}

static void out_n(const char *s, size_t n) {
    if (out_pos + n > LESS_OUTBUF) less_flush();
    if (n > LESS_OUTBUF) { write(1, s, n); return; }
    for (size_t i = 0; i < n; i++) out_buf[out_pos++] = s[i];
}

static void out(const char *s) { out_n(s, strlen(s)); }

static void clr_screen(void) { out("\x1b[2J\x1b[H"); }
static void cursor_at(int row1, int col1) {
    char b[32];
    int n = snprintf(b, sizeof(b), "\x1b[%d;%dH", row1, col1);
    out_n(b, n);
}
static void clr_eol(void) { out("\x1b[K"); }

/* ---- file load ---- */

static int load_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(status_msg, sizeof(status_msg),
                  "less: cannot open %s (errno=%d)", path, errno);
        nlines = 1; line_len[0] = 0; lines[0][0] = 0;
        return -1;
    }
    static char buf[65536];
    ssize_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf + total, sizeof(buf) - (size_t)total);
        if (n <= 0) break;
        total += n;
        if ((size_t)total >= sizeof(buf)) break;
    }
    close(fd);

    nlines = 0;
    int col = 0;
    for (ssize_t i = 0; i < total; i++) {
        if (buf[i] == '\n') {
            lines[nlines][col] = 0;
            line_len[nlines] = col;
            nlines++; col = 0;
            if (nlines >= MAX_LINES) break;
        } else if (col < MAX_COLS - 1) {
            lines[nlines][col++] = buf[i];
        }
    }
    if (col > 0 || nlines == 0) {
        lines[nlines][col] = 0;
        line_len[nlines] = col;
        nlines++;
    }
    snprintf(status_msg, sizeof(status_msg),
              "%s — %zd bytes, %d lines", path, total, nlines);
    return 0;
}

/* ---- search ---- */

/* Naive substring scan: returns offset of first match in `line` at or
 * after `from_col`, or -1 if no match. */
static int find_in_line(const char *line, int len, int from_col,
                         const char *pat, int plen) {
    if (plen == 0 || plen > len) return -1;
    for (int i = from_col; i + plen <= len; i++) {
        int j;
        for (j = 0; j < plen; j++) {
            if (line[i + j] != pat[j]) break;
        }
        if (j == plen) return i;
    }
    return -1;
}

/* Find the next line (>= start_row, or > start_row if same_row==0)
 * that contains the pattern. Returns row index or -1. */
static int search_forward(int start_row, int same_row_ok) {
    if (pattern_len == 0) return -1;
    int r = start_row + (same_row_ok ? 0 : 1);
    for (; r < nlines; r++) {
        if (find_in_line(lines[r], line_len[r], 0, pattern, pattern_len) >= 0)
            return r;
    }
    return -1;
}

static int search_backward(int start_row) {
    if (pattern_len == 0) return -1;
    for (int r = start_row - 1; r >= 0; r--) {
        if (find_in_line(lines[r], line_len[r], 0, pattern, pattern_len) >= 0)
            return r;
    }
    return -1;
}

/* ---- render ---- */

static void render(void) {
    clr_screen();

    if (cur_row < top_row) top_row = cur_row;
    if (cur_row >= top_row + text_rows) top_row = cur_row - text_rows + 1;
    if (top_row < 0) top_row = 0;

    for (int r = 0; r < text_rows; r++) {
        int file_row = top_row + r;
        cursor_at(r + 1, 1);
        if (file_row >= nlines) { out("~"); clr_eol(); continue; }

        int n = line_len[file_row];
        if (n > screen_cols) n = screen_cols;

        if (pattern_len == 0) {
            out_n(lines[file_row], n);
        } else {
            /* Walk the line emitting reverse-video runs for matches. */
            int col = 0;
            while (col < n) {
                int hit = find_in_line(lines[file_row], n, col,
                                        pattern, pattern_len);
                if (hit < 0) {
                    out_n(lines[file_row] + col, n - col);
                    break;
                }
                if (hit > col) out_n(lines[file_row] + col, hit - col);
                out("\x1b[7m");
                int show = pattern_len;
                if (hit + show > n) show = n - hit;
                out_n(lines[file_row] + hit, show);
                out("\x1b[27m");
                col = hit + pattern_len;
                if (col > n) col = n;
            }
        }
        clr_eol();
    }

    /* Status line — show search prompt while typing /..., otherwise
     * file/position info. */
    cursor_at(screen_rows - 1, 1);
    char status[200];
    int n;
    if (in_search) {
        n = snprintf(status, sizeof(status), "/%.*s", search_len, search_buf);
    } else if (status_msg[0]) {
        n = snprintf(status, sizeof(status), "%s", status_msg);
        status_msg[0] = 0;
    } else {
        int pct = (nlines > 0) ? (cur_row + 1) * 100 / nlines : 0;
        n = snprintf(status, sizeof(status),
                      "%s  line %d/%d  (%d%%)  -- q quit  / search  n/N next/prev",
                      filename, cur_row + 1, nlines, pct);
    }
    out_n(status, n);
    clr_eol();

    /* Last row is the command prompt area — keep it blank unless typing. */
    cursor_at(screen_rows, 1);
    clr_eol();

    less_flush();
}

/* ---- input helpers ---- */

static int read_byte(void) {
    char c;
    ssize_t n = read(0, &c, 1);
    if (n != 1) return -1;
    return (unsigned char)c;
}

static int peek_byte_nonblock(void) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(0, &rfds);
    struct timeval tv = { 0, 0 };
    int r = select(1, &rfds, 0, 0, &tv);
    if (r <= 0) return -1;
    char c;
    if (read(0, &c, 1) != 1) return -1;
    return (unsigned char)c;
}

static void page_down(int n) {
    cur_row += n;
    if (cur_row >= nlines) cur_row = nlines - 1;
}
static void page_up(int n) {
    cur_row -= n;
    if (cur_row < 0) cur_row = 0;
}

/* Commit the /pattern after the user hits Enter. */
static void finish_search(void) {
    in_search = 0;
    if (search_len == 0) { pattern_len = 0; return; }
    memcpy(pattern, search_buf, search_len);
    pattern_len = search_len;
    int hit = search_forward(cur_row, 1);
    if (hit >= 0) {
        cur_row = hit;
    } else {
        snprintf(status_msg, sizeof(status_msg),
                  "pattern not found");
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: less FILE\n");
        return 1;
    }
    strcpy(filename, argv[1]);
    if (load_file(filename) != 0) {
        printf("%s\n", status_msg);
        return 1;
    }

    if (tcgetattr(0, &saved_termios) != 0) {
        printf("less: not a tty\n");
        return 1;
    }
    struct termios raw = saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw);

    struct winsize ws;
    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
        screen_rows = ws.ws_row;
        screen_cols = ws.ws_col;
    }
    text_rows = screen_rows - 2;
    if (text_rows < 1) text_rows = 1;

    int quit = 0;
    int pending_g = 0;

    render();
    while (!quit) {
        int c = read_byte();
        if (c < 0) break;

        if (in_search) {
            if (c == 27) {
                in_search = 0; search_len = 0;
            } else if (c == '\n' || c == '\r') {
                finish_search();
            } else if (c == '\b' || c == 0x7f) {
                if (search_len > 0) search_len--;
            } else if (c >= 32 && c < 127) {
                if (search_len + 1 < (int)sizeof(search_buf))
                    search_buf[search_len++] = (char)c;
            }
            render();
            continue;
        }

        if (c != 'g') pending_g = 0;

        if (c == 0x1B) {
            int b1 = peek_byte_nonblock();
            if (b1 == '[') {
                int b2 = peek_byte_nonblock();
                switch (b2) {
                case 'A': c = 'k'; break;
                case 'B': c = 'j'; break;
                case 'C': c = 'l'; break;
                case 'D': c = 'h'; break;
                default:  render(); continue;
                }
            } else {
                render(); continue;
            }
        }

        switch (c) {
        case 'q': quit = 1; break;

        case 'h': /* less doesn't really do horizontal scroll but keep
                   * the no-op so muscle memory doesn't blow up */    break;
        case 'l':                                                     break;

        case 'k': if (cur_row > 0) cur_row--;                          break;
        case 'j': if (cur_row < nlines - 1) cur_row++;                 break;

        case ' ': page_down(text_rows);                                break;
        case 'b': page_up(text_rows);                                  break;
        case 'd': page_down(text_rows / 2);                            break;
        case 'u': page_up(text_rows / 2);                              break;

        case '0': /* no horizontal cursor in less, ignore */           break;
        case '$':                                                     break;

        case 'g':
            if (pending_g) { cur_row = 0; pending_g = 0; }
            else            pending_g = 1;
            break;
        case 'G': cur_row = nlines - 1;                                break;

        case '/':
            in_search  = 1;
            search_len = 0;
            break;

        case 'n': {
            int hit = search_forward(cur_row, 0);
            if (hit >= 0) cur_row = hit;
            else snprintf(status_msg, sizeof(status_msg),
                           "pattern not found (forward)");
            break;
        }
        case 'N': {
            int hit = search_backward(cur_row);
            if (hit >= 0) cur_row = hit;
            else snprintf(status_msg, sizeof(status_msg),
                           "pattern not found (backward)");
            break;
        }

        default: break;
        }

        render();
    }

    clr_screen();
    cursor_at(1, 1);
    less_flush();
    tcsetattr(0, TCSANOW, &saved_termios);
    return 0;
}
