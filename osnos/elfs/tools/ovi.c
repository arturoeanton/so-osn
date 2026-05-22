/*
 * /bin/ovi — osnos modal text editor (vim-flavoured subset).
 *
 *   exec /bin/ovi FILE
 *
 * Modes:
 *   NORMAL  (default) : navigation + commands
 *   INSERT            : keys insert text; ESC -> NORMAL
 *   COMMAND           : ":" prompt at bottom; Enter to run, ESC to cancel
 *
 * Normal-mode keys:
 *   h j k l    move left / down / up / right
 *   0  $       line start / line end
 *   g g        file start
 *   G          file end
 *   i          enter insert at cursor
 *   a          enter insert after cursor
 *   o          open line below + insert
 *   O          open line above + insert
 *   x          delete char at cursor
 *   d d        delete current line
 *   :          enter command mode
 *
 * Command-mode words:
 *   :w        save
 *   :q        quit (refuses if dirty)
 *   :q!       quit without save
 *   :wq       save + quit
 *
 * Limits: 4096 lines × 1024 cols. Buffer is line-by-line — each line
 * is a fixed-size char array, contents NUL-terminated. Crude but
 * trivially correct and plenty for editing osnos config files.
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

/*
 * Line buffer + cursor state. Static so we don't risk blowing the 4 KiB
 * user stack with a 4 MiB array.
 */
static char    lines[MAX_LINES][MAX_COLS];
static int     line_len[MAX_LINES];   /* without trailing NUL */
static int     nlines = 1;            /* always at least one empty line */

static int  cur_row = 0;              /* 0-based */
static int  cur_col = 0;
static int  top_row = 0;              /* first row visible on screen */

static int  screen_rows = 25;
static int  screen_cols = 80;
static int  text_rows;                /* screen_rows - 2 (status + cmd) */

static int  dirty = 0;
static char filename[128];

static char cmd_buf[128];
static int  cmd_len;

static char status_msg[128];   /* one-shot message for the status bar */

static struct termios saved_termios;

/* ---- low-level output helpers ----
 *
 * Each render emits ~30 small writes (cursor positioning, line
 * contents, status bar, SGR sequences). Without buffering, every
 * write becomes a sys_write → IPC_CONSOLE_WRITE → 1 slot in the
 * shared queue. The queue is 64 slots; two renders saturate it and
 * subsequent writes get dropped, leaving the screen half-painted.
 *
 * Batch everything into a 16 KB buffer and flush once at the end
 * of each render. ovi_flush() is called explicitly after render()
 * + after every input handling step that might want to repaint.
 */
#define OVI_OUTBUF 16384
static char   out_buf[OVI_OUTBUF];
static size_t out_pos;

static void ovi_flush(void) {
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
    if (out_pos + n > OVI_OUTBUF) ovi_flush();
    if (n > OVI_OUTBUF) {
        write(1, s, n);
        return;
    }
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

/* ---- buffer ops ---- */

static void buf_insert_char(char c) {
    if (line_len[cur_row] >= MAX_COLS - 1) return;
    char *L = lines[cur_row];
    for (int i = line_len[cur_row]; i > cur_col; i--) L[i] = L[i - 1];
    L[cur_col] = c;
    line_len[cur_row]++;
    L[line_len[cur_row]] = 0;
    cur_col++;
    dirty = 1;
}

static void buf_split_line(void) {
    if (nlines >= MAX_LINES) return;
    /* Shift lines below cur_row down by 1. */
    for (int i = nlines; i > cur_row + 1; i--) {
        memcpy(lines[i], lines[i - 1], MAX_COLS);
        line_len[i] = line_len[i - 1];
    }
    /* New line below carries the tail of the current line from cur_col. */
    char *src = lines[cur_row];
    char *dst = lines[cur_row + 1];
    int   tail = line_len[cur_row] - cur_col;
    for (int i = 0; i < tail; i++) dst[i] = src[cur_col + i];
    dst[tail] = 0;
    line_len[cur_row + 1] = tail;
    src[cur_col] = 0;
    line_len[cur_row] = cur_col;
    nlines++;
    cur_row++;
    cur_col = 0;
    dirty = 1;
}

static void buf_delete_char_before(void) {
    if (cur_col > 0) {
        char *L = lines[cur_row];
        for (int i = cur_col - 1; i < line_len[cur_row]; i++) L[i] = L[i + 1];
        line_len[cur_row]--;
        cur_col--;
        dirty = 1;
        return;
    }
    if (cur_row == 0) return;
    /* Merge current line into previous one. */
    int prev = cur_row - 1;
    int prev_len = line_len[prev];
    int cur_len  = line_len[cur_row];
    if (prev_len + cur_len >= MAX_COLS - 1) return;
    for (int i = 0; i < cur_len; i++) lines[prev][prev_len + i] = lines[cur_row][i];
    line_len[prev] = prev_len + cur_len;
    lines[prev][line_len[prev]] = 0;
    /* Shift lines below up. */
    for (int i = cur_row; i + 1 < nlines; i++) {
        memcpy(lines[i], lines[i + 1], MAX_COLS);
        line_len[i] = line_len[i + 1];
    }
    nlines--;
    cur_row = prev;
    cur_col = prev_len;
    dirty = 1;
}

static void buf_delete_char_under(void) {
    if (line_len[cur_row] == 0) return;
    char *L = lines[cur_row];
    if (cur_col >= line_len[cur_row]) cur_col = line_len[cur_row] - 1;
    for (int i = cur_col; i < line_len[cur_row]; i++) L[i] = L[i + 1];
    line_len[cur_row]--;
    L[line_len[cur_row]] = 0;
    if (cur_col > 0 && cur_col >= line_len[cur_row]) cur_col--;
    dirty = 1;
}

static void buf_delete_line(void) {
    if (nlines == 1) {
        lines[0][0] = 0;
        line_len[0] = 0;
        cur_col = 0;
        dirty = 1;
        return;
    }
    for (int i = cur_row; i + 1 < nlines; i++) {
        memcpy(lines[i], lines[i + 1], MAX_COLS);
        line_len[i] = line_len[i + 1];
    }
    nlines--;
    if (cur_row >= nlines) cur_row = nlines - 1;
    if (cur_col > line_len[cur_row]) cur_col = line_len[cur_row];
    dirty = 1;
}

/* ---- file load / save ---- */

static int load_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        nlines = 1;
        line_len[0] = 0;
        lines[0][0] = 0;
        snprintf(status_msg, sizeof(status_msg),
                  "new file (errno=%d)", errno);
        return 0;
    }
    static char buf[65536];
    ssize_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf + total, sizeof(buf) - (size_t)total);
        if (n < 0) {
            snprintf(status_msg, sizeof(status_msg),
                      "read failed: errno=%d (got %zd)", errno, total);
            break;
        }
        if (n == 0) break;
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
            nlines++;
            col = 0;
            if (nlines >= MAX_LINES) break;
        } else {
            if (col < MAX_COLS - 1) lines[nlines][col++] = buf[i];
        }
    }
    if (col > 0 || nlines == 0) {
        lines[nlines][col] = 0;
        line_len[nlines] = col;
        nlines++;
    }
    if (status_msg[0] == 0) {
        snprintf(status_msg, sizeof(status_msg),
                  "loaded %zd bytes, %d lines", total, nlines);
    }
    return 0;
}

static int save_file(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        snprintf(status_msg, sizeof(status_msg),
                  "save: open failed errno=%d", errno);
        return -1;
    }
    int total = 0;
    for (int i = 0; i < nlines; i++) {
        if (line_len[i] > 0) {
            ssize_t w = write(fd, lines[i], line_len[i]);
            if (w < 0) {
                snprintf(status_msg, sizeof(status_msg),
                          "save: write failed errno=%d at line %d", errno, i+1);
                close(fd);
                return -1;
            }
            total += (int)w;
        }
        if (i + 1 < nlines) {
            ssize_t w = write(fd, "\n", 1);
            if (w < 0) {
                snprintf(status_msg, sizeof(status_msg),
                          "save: write \\n failed errno=%d", errno);
                close(fd);
                return -1;
            }
            total += 1;
        }
    }
    close(fd);
    dirty = 0;
    snprintf(status_msg, sizeof(status_msg),
              "wrote %d bytes to %s", total, path);
    return 0;
}

/* ---- render ---- */

static const char *mode_str = "NORMAL";

static void render(void) {
    clr_screen();

    if (cur_row < top_row) top_row = cur_row;
    if (cur_row >= top_row + text_rows) top_row = cur_row - text_rows + 1;

    for (int r = 0; r < text_rows; r++) {
        int file_row = top_row + r;
        cursor_at(r + 1, 1);
        if (file_row < nlines) {
            int n = line_len[file_row];
            if (n > screen_cols) n = screen_cols;
            out_n(lines[file_row], n);
        } else {
            out("~");
        }
        clr_eol();
    }

    /* Cursor as a reverse-video block over the char it sits on. */
    cursor_at(cur_row - top_row + 1, cur_col + 1);
    out("\x1b[7m");
    char ch = (cur_col < line_len[cur_row])
                ? lines[cur_row][cur_col] : ' ';
    if (ch < 32 || ch >= 127) ch = ' ';
    out_n(&ch, 1);
    out("\x1b[27m");

    /* Status line — second-to-last row. Show transient status_msg if
     * set, otherwise the steady file/position info. */
    cursor_at(screen_rows - 1, 1);
    char status[200];
    int n;
    if (status_msg[0]) {
        n = snprintf(status, sizeof(status), "%s", status_msg);
        status_msg[0] = 0;
    } else {
        n = snprintf(status, sizeof(status),
            "[%s] %s%s  %d/%d  col %d  ",
            mode_str,
            filename[0] ? filename : "(no file)",
            dirty ? " *" : "",
            cur_row + 1, nlines, cur_col + 1);
    }
    out_n(status, n);
    clr_eol();

    /* Command line — last row. */
    cursor_at(screen_rows, 1);
    if (mode_str[0] == 'C') {
        out(":");
        out_n(cmd_buf, cmd_len);
    }
    clr_eol();

    /* Push everything to the framebuffer in one shot — avoids
     * saturating the IPC console queue and leaving partial paints. */
    ovi_flush();
}

/* ---- input helpers ---- */

static int read_byte(void) {
    char c;
    ssize_t n = read(0, &c, 1);
    if (n != 1) return -1;
    return (unsigned char)c;
}

/*
 * Non-blocking peek: returns -1 immediately if stdin has nothing.
 * Used right after seeing ESC so we can distinguish a real Escape
 * keypress (NO follow-up byte) from an arrow-key CSI sequence (the
 * `[` arrives in the same atomic keyboard_server push).
 */
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

/* ---- command-mode handler ---- */

static int run_command_line(void) {
    cmd_buf[cmd_len] = 0;
    if (strcmp(cmd_buf, "w") == 0) {
        if (save_file(filename) != 0) return 0;
        return 0;
    }
    if (strcmp(cmd_buf, "q") == 0) {
        if (dirty) return 0;     /* refuse — set a warning maybe later */
        return 1;
    }
    if (strcmp(cmd_buf, "q!") == 0) return 1;
    if (strcmp(cmd_buf, "wq") == 0) {
        if (save_file(filename) != 0) return 0;
        return 1;
    }
    return 0;   /* unknown command, ignore */
}

/* ---- main loop ---- */

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: ovi FILE\n");
        return 1;
    }
    strcpy(filename, argv[1]);
    load_file(filename);

    /* Save terminal state, switch to raw mode (one byte at a time,
     * no echo, signals off so Ctrl+C doesn't kill us mid-edit). */
    if (tcgetattr(0, &saved_termios) != 0) {
        printf("ovi: not a tty\n");
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
    int pending_d = 0;

    render();
    while (!quit) {
        int c = read_byte();
        if (c < 0) break;

        if (mode_str[0] == 'N') {
            /* Reset multi-key state if the next key isn't part of it. */
            if (c != 'g') pending_g = 0;
            if (c != 'd') pending_d = 0;

            /* Arrow keys arrive as ESC [ A/B/C/D from the TTY in a
             * single atomic push from keyboard_server. We peek
             * without blocking so a real lone-Escape doesn't hang
             * waiting for follow-up bytes that never come. */
            if (c == 0x1B) {
                int b1 = peek_byte_nonblock();
                if (b1 == '[') {
                    int b2 = peek_byte_nonblock();
                    switch (b2) {
                    case 'A': c = 'k'; break;   /* up    */
                    case 'B': c = 'j'; break;   /* down  */
                    case 'C': c = 'l'; break;   /* right */
                    case 'D': c = 'h'; break;   /* left  */
                    default:  render(); continue;
                    }
                } else {
                    render();
                    continue;
                }
            }

            switch (c) {
            case 'h': if (cur_col > 0) cur_col--;                       break;
            case 'l': if (cur_col < line_len[cur_row]) cur_col++;        break;
            case 'k':
                if (cur_row > 0) cur_row--;
                if (cur_col > line_len[cur_row]) cur_col = line_len[cur_row];
                break;
            case 'j':
                if (cur_row < nlines - 1) cur_row++;
                if (cur_col > line_len[cur_row]) cur_col = line_len[cur_row];
                break;
            case '0': cur_col = 0;                                       break;
            case '$': cur_col = line_len[cur_row];                       break;
            case 'g':
                if (pending_g) { cur_row = 0; cur_col = 0; pending_g = 0; }
                else            pending_g = 1;
                break;
            case 'G':
                cur_row = nlines - 1;
                if (cur_col > line_len[cur_row]) cur_col = line_len[cur_row];
                break;
            case 'i': mode_str = "INSERT";                                break;
            case 'a':
                if (cur_col < line_len[cur_row]) cur_col++;
                mode_str = "INSERT";
                break;
            case 'o':
                cur_col = line_len[cur_row];
                buf_split_line();
                mode_str = "INSERT";
                break;
            case 'O':
                cur_col = 0;
                buf_split_line();
                cur_row--;
                mode_str = "INSERT";
                break;
            case 'x': buf_delete_char_under();                            break;
            case 'd':
                if (pending_d) { buf_delete_line(); pending_d = 0; }
                else            pending_d = 1;
                break;
            case ':':
                mode_str = "COMMAND";
                cmd_len  = 0;
                break;
            default: break;
            }
        } else if (mode_str[0] == 'I') {
            /* In INSERT mode, an ESC followed by '[X' is an arrow key
             * — handle navigation without leaving INSERT. A lone ESC
             * (no '[' after) returns to NORMAL mode. The peek is
             * non-blocking so the common "press Esc → go to NORMAL"
             * case doesn't hang waiting for follow-up bytes. */
            if (c == 27) {
                int b1 = peek_byte_nonblock();
                if (b1 == '[') {
                    int b2 = peek_byte_nonblock();
                    switch (b2) {
                    case 'A': if (cur_row > 0) cur_row--;
                              if (cur_col > line_len[cur_row]) cur_col = line_len[cur_row];
                              break;
                    case 'B': if (cur_row < nlines - 1) cur_row++;
                              if (cur_col > line_len[cur_row]) cur_col = line_len[cur_row];
                              break;
                    case 'C': if (cur_col < line_len[cur_row]) cur_col++; break;
                    case 'D': if (cur_col > 0) cur_col--; break;
                    default: break;
                    }
                    render();
                    continue;
                }
                mode_str = "NORMAL";
                if (cur_col > 0 && cur_col >= line_len[cur_row] &&
                    line_len[cur_row] > 0) cur_col--;
            } else if (c == '\b' || c == 0x7f) {
                buf_delete_char_before();
            } else if (c == '\n' || c == '\r') {
                buf_split_line();
            } else if (c >= 32 && c < 127) {
                buf_insert_char((char)c);
            }
            /* control bytes other than ESC/BS/CR are ignored in INSERT */
        } else {                            /* COMMAND mode */
            if (c == 27) {
                mode_str = "NORMAL";
                cmd_len = 0;
            } else if (c == '\n' || c == '\r') {
                int leave = run_command_line();
                mode_str = "NORMAL";
                cmd_len = 0;
                if (leave) quit = 1;
            } else if (c == '\b' || c == 0x7f) {
                if (cmd_len > 0) cmd_len--;
            } else if (c >= 32 && c < 127) {
                if (cmd_len + 1 < (int)sizeof(cmd_buf)) {
                    cmd_buf[cmd_len++] = (char)c;
                }
            }
        }

        render();
    }

    /* Restore terminal. Last paint clears the screen so we don't
     * leave the editor frame behind on the shell. */
    clr_screen();
    cursor_at(1, 1);
    tcsetattr(0, TCSANOW, &saved_termios);
    return 0;
}
