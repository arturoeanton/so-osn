/*
 * /bin/oxbrowser — minimal HTTP browser for the Ox window system.
 *
 * Scope (v1): plain-text rendering of HTTP/1.1 GET responses.
 *   - URL bar: type "http://host[:port]/path" and Enter to fetch
 *   - Naive HTML stripping: tag tokens dropped, entities decoded
 *   - Anchors (<a href="…">) tracked + clickable; click navigates
 *   - Mouse wheel scroll + arrow nav + PgUp/PgDn + Home/End
 *   - Backspace = back (linear history stack, 16 entries)
 *
 * Out of scope (intentional): CSS, JS, images, forms, HTTPS, redirects
 * beyond a single follow, frames, chunked encoding edge cases, gzip.
 * The HTML parser is deliberately simple — Lynx vibes, not Chrome.
 *
 * Wire path: getaddrinfo → socket → connect → write GET → drain into
 * g_response[] until EOF or cap. Split at "\r\n\r\n" for body. The
 * body then runs through a tag/entity stripper that emits to
 * g_view[] with parallel link span tracking in g_links[].
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <ox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
/* tlse — single-file TLS 1.2/1.3 client (eduardsui/tlse, BSD).
 * Used for the https:// scheme. We skip cert validation (NULL verify
 * callback in tls_consume_stream) — adequate for a hobby browser. */
#include "../../vendor/tlse/tlse.h"

/* ---------------- geometry / palette ------------------------------ */
#define WIN_W       780
#define WIN_H       560
#define URLBAR_H     36
#define STATUS_H     16
#define MARGIN_X     10
#define LINE_H       12
#define CHAR_W        8
#define GO_BTN_W     48

#define BODY_X       MARGIN_X
#define BODY_Y       (URLBAR_H + 6)
#define BODY_W       (WIN_W - 2 * MARGIN_X)
#define BODY_H       (WIN_H - URLBAR_H - STATUS_H - 6)
#define VIS_LINES    (BODY_H / LINE_H)
#define VIS_COLS     (BODY_W / CHAR_W)

#define COL_BG          OX_RGB(245, 245, 240)
#define COL_TEXT        OX_RGB( 20,  20,  25)
#define COL_URLBAR_BG   OX_RGB( 30,  30,  35)   /* dark toolbar */
#define COL_URLBAR_FG   OX_RGB(220, 220, 220)
#define COL_URL_EDIT    OX_RGB(245, 245, 245)
#define COL_URL_EDIT_FG OX_RGB( 20,  20,  30)
#define COL_GO_BTN      OX_RGB( 53, 132, 228)   /* Adwaita blue */
#define COL_GO_BTN_HOV  OX_RGB( 80, 158, 255)
#define COL_GO_BTN_FG   OX_RGB(255, 255, 255)
#define COL_BACK_BTN    OX_RGB( 60,  60,  70)
#define COL_BACK_BTN_FG OX_RGB(230, 230, 230)
#define COL_LINK_FG     OX_RGB( 30,  90, 200)
#define COL_LINK_HOV    OX_RGB(192,  28,  40)
#define COL_STATUS_BG   OX_RGB( 40,  40,  50)
#define COL_STATUS_FG   OX_RGB(220, 220, 220)
#define COL_CARET       OX_RGB( 53, 132, 228)

/* ---------------- limits ----------------------------------------- */
#define URL_MAX        256
#define HOST_MAX       128
#define PATH_MAX_LEN   200
#define RESP_MAX       (64 * 1024)
#define VIEW_MAX       (32 * 1024)
#define LINK_MAX       128
#define HIST_MAX        16

/* ---------------- state ------------------------------------------ */
static char g_url_edit[URL_MAX];        /* what the user is typing */
static int  g_url_edit_len = 0;
static int  g_url_caret = 0;
static int  g_url_focused = 1;          /* URL bar has focus */

/* Last successfully fetched URL — shown in status, used by back. */
static char g_url_current[URL_MAX] = "";

/* Linear history: [0 .. g_hist_top) are previous pages, current page
 * sits at g_hist_top - 1. Back pops off the top. */
static char g_history[HIST_MAX][URL_MAX];
static int  g_hist_top = 0;

/* Raw HTTP response and the stripped view text. */
static char g_response[RESP_MAX];
static int  g_response_len = 0;
static int  g_body_off = 0;             /* offset of body start in g_response */
static int  g_http_status = 0;
static char g_content_type[64] = "";

static char g_view[VIEW_MAX];
static int  g_view_len = 0;

/* Per-anchor span in g_view + href text. Tracked while stripping. */
typedef struct {
    int  start;      /* offset in g_view */
    int  end;        /* offset in g_view (exclusive) */
    char href[256];
} link_t;
static link_t g_links[LINK_MAX];
static int    g_link_count = 0;
static int    g_link_hover = -1;

static int  g_scroll = 0;                /* topmost visible line */
static char g_status[160] = "ready";
static ox_win_t g_win;

/* ---------------- view buffer helpers ----------------------------- */

static void view_clear(void) {
    g_view_len = 0;
    g_link_count = 0;
    g_link_hover = -1;
    g_scroll = 0;
}

static void view_putc(char c) {
    if (g_view_len < VIEW_MAX - 1) g_view[g_view_len++] = c;
}

static void view_puts(const char *s) {
    while (*s) view_putc(*s++);
}

/* Count total lines in g_view (always >= 1). */
static int view_total_lines(void) {
    int n = 1;
    for (int i = 0; i < g_view_len; i++)
        if (g_view[i] == '\n') n++;
    return n;
}

/* Convert (x,y) window-local pixel → g_view byte offset. */
static int view_xy_to_off(int x, int y) {
    if (y < BODY_Y) return 0;
    int row = (y - BODY_Y) / LINE_H + g_scroll;
    int target_line = row;
    int target_col  = (x - BODY_X) / CHAR_W;
    if (target_col < 0) target_col = 0;
    int line = 0, col = 0;
    for (int i = 0; i < g_view_len; i++) {
        if (line == target_line && col == target_col) return i;
        if (g_view[i] == '\n') {
            if (line == target_line) return i;  /* clicked past EOL */
            line++; col = 0;
        } else {
            col++;
        }
    }
    return g_view_len;
}

/* ---------------- HTML stripping --------------------------------- */

/* Match a single named entity at &<name>; — return byte to emit or 0
 * if not recognized. Caller is responsible for advancing past the ';'. */
static int decode_entity(const char *p, int n, int *out_skip) {
    /* Numeric: &#N; or &#xN; */
    if (n >= 4 && p[0] == '&' && p[1] == '#') {
        int i = 2, base = 10, val = 0;
        if (p[2] == 'x' || p[2] == 'X') { base = 16; i = 3; }
        while (i < n && p[i] != ';') {
            int d = -1;
            if (p[i] >= '0' && p[i] <= '9') d = p[i] - '0';
            else if (base == 16 && p[i] >= 'a' && p[i] <= 'f') d = 10 + p[i] - 'a';
            else if (base == 16 && p[i] >= 'A' && p[i] <= 'F') d = 10 + p[i] - 'A';
            else return 0;
            val = val * base + d;
            i++;
            if (i > 8) return 0;  /* sanity cap */
        }
        if (i >= n || p[i] != ';') return 0;
        *out_skip = i + 1;
        if (val < 32 || val > 126) return '?';
        return val;
    }
    /* Named, very small table. */
    static const struct { const char *name; char c; } tbl[] = {
        { "amp;",   '&'  }, { "lt;",    '<'  }, { "gt;",    '>'  },
        { "quot;",  '"'  }, { "apos;",  '\'' }, { "nbsp;",  ' '  },
        { "copy;",  'c'  }, { "reg;",   'r'  }, { "hellip;",'.'  },
        { "mdash;", '-'  }, { "ndash;", '-'  }, { "lsquo;", '\'' },
        { "rsquo;", '\'' }, { "ldquo;", '"'  }, { "rdquo;", '"'  },
        { 0, 0 }
    };
    for (int i = 0; tbl[i].name; i++) {
        size_t L = strlen(tbl[i].name);
        if ((int)L < n - 1 && memcmp(p + 1, tbl[i].name, L) == 0) {
            *out_skip = (int)L + 1;
            return tbl[i].c;
        }
    }
    return 0;
}

/* Extract href="..." (or href='...') from a tag attr string into out.
 * Returns 1 on success. */
static int parse_href(const char *attrs, int n, char *out, size_t cap) {
    for (int i = 0; i < n - 5; i++) {
        if ((attrs[i]=='h'||attrs[i]=='H') &&
            (attrs[i+1]=='r'||attrs[i+1]=='R') &&
            (attrs[i+2]=='e'||attrs[i+2]=='E') &&
            (attrs[i+3]=='f'||attrs[i+3]=='F') &&
            (attrs[i+4]=='=' || attrs[i+4]==' ')) {
            int j = i + 4;
            while (j < n && (attrs[j] == ' ' || attrs[j] == '=')) j++;
            char quote = 0;
            if (j < n && (attrs[j] == '"' || attrs[j] == '\'')) {
                quote = attrs[j]; j++;
            }
            size_t k = 0;
            while (j < n && k + 1 < cap &&
                   (quote ? attrs[j] != quote
                          : (attrs[j] != ' ' && attrs[j] != '>'))) {
                out[k++] = attrs[j++];
            }
            out[k] = 0;
            return 1;
        }
    }
    return 0;
}

/* Tag name match (case insensitive, exact). Returns 1 if `tag` (up to
 * first space/>) equals `name`. */
static int tag_eq(const char *tag, int n, const char *name) {
    int i = 0;
    while (i < n && name[i] && tag[i] != ' ' && tag[i] != '>') {
        char a = tag[i], b = name[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
        i++;
    }
    if (!name[i]) {
        /* Name exhausted — make sure tag ends here too. */
        if (i >= n) return 1;
        return (tag[i] == ' ' || tag[i] == '>' || tag[i] == '/');
    }
    return 0;
}

/* Block-level tags trigger a newline in the output. */
static int is_block_tag(const char *tag, int n) {
    static const char *blocks[] = {
        "p", "div", "br", "h1", "h2", "h3", "h4", "h5", "h6",
        "li", "tr", "ul", "ol", "table", "section", "article",
        "header", "footer", "nav", "main", "aside", "blockquote",
        "pre", "hr", 0
    };
    /* skip leading '/' for closing tags */
    if (n > 0 && tag[0] == '/') { tag++; n--; }
    for (int i = 0; blocks[i]; i++) if (tag_eq(tag, n, blocks[i])) return 1;
    return 0;
}

static void strip_html(const char *src, int n) {
    int active_link = -1;   /* index into g_links, -1 if not inside <a> */
    int in_script = 0;      /* inside <script>...</script> */
    int in_style  = 0;
    int last_was_space = 1;

    for (int i = 0; i < n; i++) {
        char c = src[i];

        /* Skip past <script>...</script> bodies wholesale — they're
         * never useful as text and contain wild characters. */
        if (in_script) {
            if (c == '<' && i + 8 < n &&
                memcmp(src + i, "</script", 8) == 0) {
                in_script = 0;
                while (i < n && src[i] != '>') i++;
            }
            continue;
        }
        if (in_style) {
            if (c == '<' && i + 7 < n &&
                memcmp(src + i, "</style", 7) == 0) {
                in_style = 0;
                while (i < n && src[i] != '>') i++;
            }
            continue;
        }

        if (c == '<') {
            int j = i + 1;
            while (j < n && src[j] != '>') j++;
            int tag_n = j - i - 1;
            const char *tag = src + i + 1;

            if (tag_eq(tag, tag_n, "script")) in_script = 1;
            else if (tag_eq(tag, tag_n, "style")) in_style = 1;
            else if (tag_eq(tag, tag_n, "a")) {
                /* Open anchor — record href. */
                if (g_link_count < LINK_MAX) {
                    link_t *L = &g_links[g_link_count];
                    L->start = g_view_len;
                    L->end   = g_view_len;
                    L->href[0] = 0;
                    parse_href(tag, tag_n, L->href, sizeof(L->href));
                    active_link = g_link_count++;
                }
            } else if (tag_n >= 1 && tag[0] == '/' &&
                       tag_eq(tag + 1, tag_n - 1, "a")) {
                if (active_link >= 0) {
                    g_links[active_link].end = g_view_len;
                    active_link = -1;
                }
            } else if (is_block_tag(tag, tag_n)) {
                if (!last_was_space || g_view_len == 0 ||
                    g_view[g_view_len - 1] != '\n')
                    view_putc('\n');
                /* Bullet-ish marker for list items. */
                if (tag_eq(tag, tag_n, "li")) { view_puts("  - "); }
                last_was_space = 1;
            }
            i = j;   /* skip past '>' */
            continue;
        }
        if (c == '&') {
            int skip = 0;
            int ch = decode_entity(src + i, n - i, &skip);
            if (ch != 0) {
                view_putc((char)ch);
                if (active_link >= 0) g_links[active_link].end = g_view_len;
                i += skip - 1;
                last_was_space = (ch == ' ');
                continue;
            }
        }
        /* Whitespace collapse: condense runs of WS to a single space,
         * but PRESERVE explicit '\n' from already-stripped block tags. */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!last_was_space && g_view_len > 0) {
                view_putc(' ');
                if (active_link >= 0) g_links[active_link].end = g_view_len;
            }
            last_was_space = 1;
            continue;
        }
        view_putc(c);
        if (active_link >= 0) g_links[active_link].end = g_view_len;
        last_was_space = 0;
    }
}

/* ---------------- HTTP fetch ------------------------------------- */

/* Parse "[http://|https://]host[:port][/path]" into pieces. Returns 1
 * on success. `*out_https` is set to 1 for https://, 0 for http://. */
static int parse_url(const char *url, char *host, size_t host_cap,
                      int *out_port, int *out_https,
                      char *path, size_t path_cap) {
    const char *p = url;
    int is_https = 0;
    int default_port = 80;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) { p += 8; is_https = 1; default_port = 443; }

    size_t hi = 0;
    while (*p && *p != ':' && *p != '/' && hi + 1 < host_cap) {
        host[hi++] = *p++;
    }
    host[hi] = 0;
    if (hi == 0) return 0;

    int port = default_port;
    if (*p == ':') {
        p++;
        port = 0;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (*p - '0');
            p++;
        }
        if (port <= 0 || port > 65535) port = default_port;
    }
    *out_port  = port;
    *out_https = is_https;

    if (*p == 0) {
        path[0] = '/'; path[1] = 0;
    } else {
        size_t pi = 0;
        while (*p && pi + 1 < path_cap) path[pi++] = *p++;
        path[pi] = 0;
    }
    return 1;
}

/* Resolve `host` to an in_addr_t using getaddrinfo (handles both
 * numeric IPs and DNS names via the kernel resolver path). */
static int resolve_host(const char *host, struct in_addr *out) {
    struct addrinfo hints, *res = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, 0, &hints, &res) != 0 || !res) return 0;
    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
    *out = sa->sin_addr;
    freeaddrinfo(res);
    return 1;
}

static void render(void);   /* fwd ref */

/* Update status text + repaint so the user sees progress during the
 * slow network/TLS phases. Without this, fetch_url looks like a
 * total freeze (especially TLS handshake which can take 5-30s in
 * QEMU TCG with soft crypto). */
static void status_set(const char *msg) {
    snprintf(g_status, sizeof(g_status), "%s", msg);
    render();
}

/* read() with a millisecond timeout via poll(). Returns:
 *   >0 = bytes read
 *    0 = EOF
 *   -1 = error or timeout (errno == ETIMEDOUT for timeout) */
static ssize_t read_timeout(int fd, void *buf, size_t cap, int timeout_ms) {
    struct pollfd pfd = { fd, POLLIN, 0 };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) {
        errno = pr == 0 ? 110 /* ETIMEDOUT */ : errno;
        return -1;
    }
    return read(fd, buf, cap);
}

/* Read response into g_response. Stops at EOF, cap, or 30s of silence.
 * Returns total bytes read or -1 on error. The timeout avoids hanging
 * on misbehaving servers (no FIN). */
static int drain_response(int fd) {
    g_response_len = 0;
    int idle = 0;
    while (g_response_len < RESP_MAX - 1) {
        ssize_t n = read_timeout(fd, g_response + g_response_len,
                                  (size_t)(RESP_MAX - 1 - g_response_len),
                                  3000);
        if (n < 0) {
            if (errno == EINTR) continue;
            /* Timeout — give up if a few idle ticks in a row. */
            if (++idle > 10) break;
            continue;
        }
        if (n == 0) break;
        g_response_len += (int)n;
        idle = 0;
    }
    g_response[g_response_len] = 0;
    return g_response_len;
}

/* Locate status code + content-type + body offset in the raw response. */
static void parse_response_meta(void) {
    g_http_status = 0;
    g_content_type[0] = 0;
    g_body_off = g_response_len;

    /* Status line: "HTTP/1.1 200 OK\r\n" */
    int i = 0;
    while (i < g_response_len && g_response[i] != ' ') i++;
    if (i < g_response_len) {
        i++;
        while (i < g_response_len && g_response[i] >= '0' && g_response[i] <= '9') {
            g_http_status = g_http_status * 10 + (g_response[i] - '0');
            i++;
        }
    }
    /* Find headers/body separator. */
    for (int j = 0; j + 3 < g_response_len; j++) {
        if (g_response[j] == '\r' && g_response[j+1] == '\n' &&
            g_response[j+2] == '\r' && g_response[j+3] == '\n') {
            g_body_off = j + 4;
            break;
        }
        if (g_response[j] == '\n' && g_response[j+1] == '\n') {
            g_body_off = j + 2;
            break;
        }
    }
    /* Content-Type header (case-insensitive). */
    for (int j = 0; j < g_body_off; j++) {
        if (j + 14 > g_body_off) break;
        char buf[16];
        for (int k = 0; k < 14; k++) {
            char c = g_response[j + k];
            if (c >= 'A' && c <= 'Z') c += 32;
            buf[k] = c;
        }
        buf[14] = 0;
        if (memcmp(buf, "content-type: ", 14) == 0) {
            int s = j + 14, e = s;
            while (e < g_body_off && g_response[e] != '\r' &&
                   g_response[e] != '\n' && g_response[e] != ';') e++;
            int n = e - s;
            if (n >= (int)sizeof(g_content_type))
                n = sizeof(g_content_type) - 1;
            memcpy(g_content_type, g_response + s, n);
            g_content_type[n] = 0;
            break;
        }
    }
}

/* TLS-encrypted HTTP fetch via tlse. Mirrors the plain-HTTP path but
 * each socket write/read passes through tls_get_write_buffer / tls_read
 * + tls_consume_stream. Cert validation is skipped (NULL callback). */
static int do_tls_fetch(int fd, const char *host, const char *path) {
    static int tls_inited = 0;
    if (!tls_inited) { tls_init(); tls_inited = 1; }

    status_set("TLS: creating context (1.3)...");
    /* TLS_V13 → tlse genera un ClientHello moderno con la extension
     * `supported_versions` que incluye 1.2 como fallback. Cloudflare
     * y otros edges modernos hacen silent-drop de ClientHellos puros
     * TLS 1.2 (sin supported_versions) por anti-scanning. El TCP
     * raw a 1.1.1.1:443 funciona pero la respuesta TLS desaparece
     * al vacío. Con 1.3 el handshake real-mundo arranca. */
    struct TLSContext *ctx = tls_create_context(0, TLS_V13);
    if (!ctx) { status_set("TLS: create_context returned NULL"); return -1; }
    tls_sni_set(ctx, host);
    tls_client_connect(ctx);

    unsigned char buf[4096];
    int hs_iter = 0;
    int total_in = 0;
    char dbg[160];
    /* Handshake loop. Most sessions complete in 2-4 iters. */
    while (!tls_established(ctx)) {
        if (++hs_iter > 60) {
            snprintf(dbg, sizeof(dbg),
                     "TLS: handshake stuck after %d iters (%d bytes in)",
                     hs_iter, total_in);
            status_set(dbg);
            goto tls_fail;
        }
        unsigned int wlen = 0;
        const unsigned char *wbuf = tls_get_write_buffer(ctx, &wlen);
        if (wbuf && wlen > 0) {
            snprintf(dbg, sizeof(dbg),
                     "TLS: sending %u (iter %d)", wlen, hs_iter);
            status_set(dbg);
            if (write(fd, wbuf, wlen) != (ssize_t)wlen) {
                status_set("TLS: write failed during handshake");
                goto tls_fail;
            }
            tls_buffer_clear(ctx);
        }
        snprintf(dbg, sizeof(dbg),
                 "TLS: waiting for server (iter %d, %d in so far)",
                 hs_iter, total_in);
        status_set(dbg);
        /* 10s per read — TCP RTT to remote host can be slow + the
         * server might fragment its response across packets. */
        ssize_t n = read_timeout(fd, buf, sizeof(buf), 10000);
        if (n < 0) {
            snprintf(dbg, sizeof(dbg),
                     "TLS: read timeout/error (errno %d, %d bytes in)",
                     errno, total_in);
            status_set(dbg);
            goto tls_fail;
        }
        if (n == 0) {
            snprintf(dbg, sizeof(dbg),
                     "TLS: server closed early (%d bytes in)", total_in);
            status_set(dbg);
            goto tls_fail;
        }
        total_in += (int)n;
        int rc = tls_consume_stream(ctx, buf, (int)n, NULL);
        if (rc < 0) {
            snprintf(dbg, sizeof(dbg),
                     "TLS: consume_stream rc=%d (%d bytes in)", rc, total_in);
            status_set(dbg);
            goto tls_fail;
        }
    }
    /* Flush any final handshake bytes. */
    unsigned int wlen = 0;
    const unsigned char *wbuf = tls_get_write_buffer(ctx, &wlen);
    if (wbuf && wlen > 0) {
        if (write(fd, wbuf, wlen) != (ssize_t)wlen) goto tls_fail;
        tls_buffer_clear(ctx);
    }

    status_set("TLS: sending GET...");
    char req[512];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: oxbrowser/1.0\r\n"
        "Accept: text/html, text/plain, */*\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);
    tls_write(ctx, (const unsigned char *)req, (unsigned int)rl);
    wbuf = tls_get_write_buffer(ctx, &wlen);
    if (wbuf && wlen > 0) {
        if (write(fd, wbuf, wlen) != (ssize_t)wlen) goto tls_fail;
        tls_buffer_clear(ctx);
    }

    status_set("TLS: receiving...");
    g_response_len = 0;
    int idle_reads = 0;
    for (;;) {
        int pt = tls_read(ctx, buf, sizeof(buf));
        if (pt > 0) {
            int room = RESP_MAX - 1 - g_response_len;
            if (room <= 0) break;
            int take = pt < room ? pt : room;
            memcpy(g_response + g_response_len, buf, (size_t)take);
            g_response_len += take;
            idle_reads = 0;
            continue;
        }
        if (++idle_reads > 20) break;   /* ~40s cap on body */
        ssize_t n = read_timeout(fd, buf, sizeof(buf), 2000);
        if (n <= 0) break;
        if (tls_consume_stream(ctx, buf, (int)n, NULL) < 0) break;
    }
    g_response[g_response_len] = 0;

    tls_close_notify(ctx);
    tls_destroy_context(ctx);
    return g_response_len;

tls_fail:
    tls_destroy_context(ctx);
    return -1;
}

static int fetch_url(const char *url) {
    char host[HOST_MAX], path[PATH_MAX_LEN];
    int port = 80;
    int is_https = 0;

    if (!parse_url(url, host, sizeof(host), &port, &is_https,
                    path, sizeof(path))) {
        snprintf(g_status, sizeof(g_status), "Bad URL: %s", url);
        return -1;
    }

    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Resolving %s...", host);
        status_set(msg);
    }
    struct in_addr ip;
    if (!resolve_host(host, &ip)) {
        snprintf(g_status, sizeof(g_status), "DNS: cannot resolve '%s'", host);
        view_clear();
        view_puts("Failed to resolve host.\n");
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(g_status, sizeof(g_status), "socket: %s", strerror(errno));
        return -1;
    }
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Connecting %s:%d...", host, port);
        status_set(msg);
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    sa.sin_addr   = ip;
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        snprintf(g_status, sizeof(g_status), "connect %s:%d: %s",
                 host, port, strerror(errno));
        close(fd);
        view_clear();
        view_puts("Connection failed.\n");
        return -1;
    }

    int total;
    if (is_https) {
        total = do_tls_fetch(fd, host, path);
        if (total < 0) {
            close(fd);
            /* Preserve do_tls_fetch's detailed error message in g_status
             * (e.g., "consume_stream rc=-2 (1234 in)"). Don't overwrite.
             * Also surface it in the body so it's visible even after
             * status bar churns on next interaction. */
            char saved[160];
            snprintf(saved, sizeof(saved), "%s", g_status);
            view_clear();
            view_puts("TLS handshake failed:\n  ");
            view_puts(saved);
            view_puts("\n\nCert validation is disabled in this build.\n");
            view_puts("Common causes:\n");
            view_puts("  - server requires modern extensions (try TLS_V13)\n");
            view_puts("  - cipher mismatch (tlse + this server)\n");
            view_puts("  - server cert chain too large (>32 KB)\n");
            return -1;
        }
    } else {
        status_set("Sending GET...");
        char req[512];
        int rl = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: oxbrowser/1.0\r\n"
            "Accept: text/html, text/plain, */*\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, host);
        if (write(fd, req, (size_t)rl) != rl) {
            close(fd);
            snprintf(g_status, sizeof(g_status), "write: %s", strerror(errno));
            return -1;
        }
        status_set("Receiving...");
        total = drain_response(fd);
    }

    close(fd);
    if (total < 0) {
        snprintf(g_status, sizeof(g_status), "read: %s", strerror(errno));
        return -1;
    }
    parse_response_meta();

    /* Render: text/html → strip; everything else (text/plain, missing
     * content-type) → emit verbatim. */
    view_clear();
    const char *body = g_response + g_body_off;
    int body_n = g_response_len - g_body_off;
    int is_html = (strstr(g_content_type, "html") != 0);
    if (is_html) {
        strip_html(body, body_n);
    } else {
        for (int i = 0; i < body_n && g_view_len < VIEW_MAX - 1; i++) {
            if (body[i] == '\r') continue;
            view_putc(body[i]);
        }
    }

    snprintf(g_status, sizeof(g_status),
             "%s %d  %s  %d bytes  %d links",
             is_https ? "HTTPS" : "HTTP ",
             g_http_status,
             g_content_type[0] ? g_content_type : "(no type)",
             g_view_len, g_link_count);
    return 0;
}

static void resolve_relative(const char *href, char *out, size_t cap) {
    if (strncmp(href, "http://", 7) == 0 ||
        strncmp(href, "https://", 8) == 0) {
        snprintf(out, cap, "%s", href);
        return;
    }
    /* Extract scheme+host from current URL. */
    char base[URL_MAX];
    snprintf(base, sizeof(base), "%s", g_url_current);
    char *p = base;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    /* Find first '/' after host. */
    char *slash = strchr(p, '/');
    if (href[0] == '/') {
        if (slash) *slash = 0;
        snprintf(out, cap, "http://%s%s", p, href);
    } else {
        /* Strip last path segment. */
        if (slash) {
            char *last = strrchr(slash, '/');
            if (last) last[1] = 0;
        } else {
            snprintf(out, cap, "http://%s/%s", p, href);
            return;
        }
        snprintf(out, cap, "http://%s%s", p, href);
    }
}

static void load_url(const char *url) {
    /* Push current page to history before navigating. */
    if (g_url_current[0] && g_hist_top < HIST_MAX) {
        snprintf(g_history[g_hist_top++], URL_MAX, "%s", g_url_current);
    }
    snprintf(g_url_current, sizeof(g_url_current), "%s", url);
    /* Reflect in URL bar so user sees where they ended up. */
    snprintf(g_url_edit, sizeof(g_url_edit), "%s", url);
    g_url_edit_len = (int)strlen(g_url_edit);
    g_url_caret = g_url_edit_len;
    fetch_url(url);
}

static void go_back(void) {
    if (g_hist_top <= 0) return;
    char prev[URL_MAX];
    snprintf(prev, sizeof(prev), "%s", g_history[--g_hist_top]);
    /* Don't re-push on the navigation. */
    snprintf(g_url_current, sizeof(g_url_current), "%s", prev);
    snprintf(g_url_edit, sizeof(g_url_edit), "%s", prev);
    g_url_edit_len = (int)strlen(g_url_edit);
    g_url_caret = g_url_edit_len;
    fetch_url(prev);
}

/* ---------------- render ----------------------------------------- */

/* URL bar layout (geometry centralizado para hit-testing). */
#define BACK_X     6
#define BACK_Y     6
#define BACK_W     44
#define BACK_H     24
#define URLBOX_X   (BACK_X + BACK_W + 6)
#define URLBOX_Y   6
#define URLBOX_W   (WIN_W - URLBOX_X - GO_BTN_W - 10)
#define URLBOX_H   24
#define GO_X       (URLBOX_X + URLBOX_W + 4)
#define GO_Y       6
#define GO_W       GO_BTN_W
#define GO_H       24

static int g_go_hover = 0;
static int g_back_hover = 0;

static void render(void) {
    /* Body background — fill BEFORE the toolbar so the toolbar can
     * paint over the top edge without flicker. */
    ox_draw_rect(g_win, 0, URLBAR_H, WIN_W, WIN_H - URLBAR_H - STATUS_H, COL_BG);

    /* Toolbar background. */
    ox_draw_rect(g_win, 0, 0, WIN_W, URLBAR_H, COL_URLBAR_BG);

    /* Back button. */
    int back_dim = g_hist_top == 0;
    ox_draw_rect(g_win, BACK_X, BACK_Y, BACK_W, BACK_H,
                 g_back_hover && !back_dim ? COL_GO_BTN_HOV : COL_BACK_BTN);
    ox_draw_text(g_win, BACK_X + 8, BACK_Y + 8, "Back",
                 back_dim ? COL_TEXT : COL_BACK_BTN_FG);

    /* URL textbox. */
    ox_draw_rect(g_win, URLBOX_X, URLBOX_Y, URLBOX_W, URLBOX_H, COL_URL_EDIT);
    /* Truncate display if URL is too long for the box. */
    int max_chars = URLBOX_W / CHAR_W - 2;
    int show_from = 0;
    if (g_url_edit_len > max_chars) show_from = g_url_edit_len - max_chars;
    char shown[URL_MAX + 4];
    snprintf(shown, sizeof(shown), "%s", g_url_edit + show_from);
    ox_draw_text(g_win, URLBOX_X + 6, URLBOX_Y + 8, shown, COL_URL_EDIT_FG);
    if (g_url_focused) {
        int cx = URLBOX_X + 6 + (g_url_caret - show_from) * CHAR_W;
        ox_draw_rect(g_win, cx, URLBOX_Y + 4, 2, URLBOX_H - 8, COL_CARET);
    }

    /* Go button. */
    ox_draw_rect(g_win, GO_X, GO_Y, GO_W, GO_H,
                 g_go_hover ? COL_GO_BTN_HOV : COL_GO_BTN);
    ox_draw_text(g_win, GO_X + (GO_W - 2 * CHAR_W) / 2,
                 GO_Y + 8, "Go", COL_GO_BTN_FG);

    /* Body. */
    int total = view_total_lines();
    int line = 0;
    int i = 0;
    while (i < g_view_len && line < g_scroll) {
        if (g_view[i] == '\n') line++;
        i++;
    }
    int row = 0;
    while (row < VIS_LINES && line < total) {
        int y = BODY_Y + row * LINE_H;
        int x = BODY_X;
        int col = 0;
        while (i < g_view_len && g_view[i] != '\n') {
            if (col >= VIS_COLS) {
                while (i < g_view_len && g_view[i] != '\n') i++;
                break;
            }
            /* Is this byte inside any link span? */
            int link_idx = -1;
            for (int li = 0; li < g_link_count; li++) {
                if (i >= g_links[li].start && i < g_links[li].end) {
                    link_idx = li;
                    break;
                }
            }
            uint32_t color = COL_TEXT;
            if (link_idx >= 0) {
                color = (link_idx == g_link_hover) ? COL_LINK_HOV
                                                    : COL_LINK_FG;
            }
            char s[2] = { g_view[i], 0 };
            ox_draw_text(g_win, x, y, s, color);
            x += CHAR_W;
            col++;
            i++;
        }
        if (i < g_view_len && g_view[i] == '\n') i++;
        line++;
        row++;
    }

    /* Status bar. */
    ox_draw_rect(g_win, 0, WIN_H - STATUS_H, WIN_W, STATUS_H, COL_STATUS_BG);
    char line_status[180];
    if (g_link_hover >= 0) {
        snprintf(line_status, sizeof(line_status),
                 " → %s", g_links[g_link_hover].href);
    } else {
        snprintf(line_status, sizeof(line_status), " %s", g_status);
    }
    ox_draw_text(g_win, MARGIN_X, WIN_H - 12, line_status, COL_STATUS_FG);

    ox_present(g_win);
}

/* ---------------- input handling --------------------------------- */

static void url_insert(char c) {
    if (g_url_edit_len + 1 >= URL_MAX) return;
    memmove(g_url_edit + g_url_caret + 1, g_url_edit + g_url_caret,
            (size_t)(g_url_edit_len - g_url_caret));
    g_url_edit[g_url_caret] = c;
    g_url_edit_len++;
    g_url_caret++;
    g_url_edit[g_url_edit_len] = 0;
}

static void url_backspace(void) {
    if (g_url_caret == 0) return;
    memmove(g_url_edit + g_url_caret - 1, g_url_edit + g_url_caret,
            (size_t)(g_url_edit_len - g_url_caret));
    g_url_edit_len--;
    g_url_caret--;
    g_url_edit[g_url_edit_len] = 0;
}

static int link_at_offset(int off) {
    for (int i = 0; i < g_link_count; i++) {
        if (off >= g_links[i].start && off < g_links[i].end) return i;
    }
    return -1;
}

static int hit(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

int main(int argc, char **argv) {
    if (ox_init() < 0) return 1;
    g_win = ox_window_create(WIN_W, WIN_H, "Browser");
    if (g_win < 0) return 1;

    /* Default URL pre-filled in the bar — but DO NOT auto-fetch.
     * The first fetch happens when the user clicks Go or presses Enter,
     * so the toolbar paints immediately even if there's no httpd
     * running (was the "everything blank" failure mode pre-1.1: a
     * blocking connect() to a refused/timed-out endpoint left the
     * window on its default backing). */
    const char *start = argc > 1 && argv[1] && argv[1][0]
        ? argv[1] : "https://httpbin.org/get";
    snprintf(g_url_edit, sizeof(g_url_edit), "%s", start);
    g_url_edit_len = (int)strlen(g_url_edit);
    g_url_caret = g_url_edit_len;
    snprintf(g_status, sizeof(g_status),
             "Type a URL and press Enter or click Go");
    view_clear();
    view_puts("\n  oxbrowser v1\n\n");
    view_puts("  Type a URL (http:// or https://) and press Enter / click Go.\n");
    view_puts("  Backspace or the Back button = navigate back.\n");
    view_puts("  Wheel + arrows = scroll.  Click links to navigate.\n\n");
    view_puts("  HTTPS uses tlse (TLS 1.2). Cert validation disabled.\n");
    render();

    int quit = 0;
    while (!quit) {
        ox_event_t ev;
        if (!ox_wait_event(&ev)) continue;

        if (ev.type == OX_EV_CLOSE) break;

        if (ev.type == OX_EV_MOUSE) {
            if (ev.mouse_kind == OX_MOUSE_DOWN && (ev.buttons & 0x01)) {
                if (hit(ev.x, ev.y, BACK_X, BACK_Y, BACK_W, BACK_H)) {
                    if (g_hist_top > 0) { go_back(); render(); }
                    continue;
                }
                if (hit(ev.x, ev.y, GO_X, GO_Y, GO_W, GO_H)) {
                    g_url_edit[g_url_edit_len] = 0;
                    load_url(g_url_edit);
                    render();
                    continue;
                }
                if (hit(ev.x, ev.y, URLBOX_X, URLBOX_Y, URLBOX_W, URLBOX_H)) {
                    g_url_focused = 1;
                    int local = (ev.x - URLBOX_X - 6) / CHAR_W;
                    if (local < 0) local = 0;
                    if (local > g_url_edit_len) local = g_url_edit_len;
                    g_url_caret = local;
                    render();
                    continue;
                }
                if (ev.y >= URLBAR_H) {
                    g_url_focused = 0;
                    int off = view_xy_to_off(ev.x, ev.y);
                    int li = link_at_offset(off);
                    if (li >= 0) {
                        char abs_url[URL_MAX];
                        resolve_relative(g_links[li].href, abs_url, sizeof(abs_url));
                        load_url(abs_url);
                    }
                    render();
                }
                continue;
            }
            if (ev.mouse_kind == OX_MOUSE_MOVE) {
                int new_link_hover = -1;
                int new_go = hit(ev.x, ev.y, GO_X, GO_Y, GO_W, GO_H);
                int new_back = hit(ev.x, ev.y, BACK_X, BACK_Y, BACK_W, BACK_H);
                if (ev.y >= URLBAR_H) {
                    int off = view_xy_to_off(ev.x, ev.y);
                    new_link_hover = link_at_offset(off);
                }
                if (new_link_hover != g_link_hover ||
                    new_go != g_go_hover ||
                    new_back != g_back_hover) {
                    g_link_hover  = new_link_hover;
                    g_go_hover    = new_go;
                    g_back_hover  = new_back;
                    render();
                }
                continue;
            }
            if (ev.mouse_kind == OX_MOUSE_WHEEL) {
                g_scroll -= ev.wheel_delta * 3;
                int max_scroll = view_total_lines() - VIS_LINES;
                if (max_scroll < 0) max_scroll = 0;
                if (g_scroll < 0)            g_scroll = 0;
                if (g_scroll > max_scroll)   g_scroll = max_scroll;
                render();
                continue;
            }
            continue;
        }

        if (ev.type != OX_EV_KEY) continue;

        /* URL bar has focus: text-edit + Enter to fetch. */
        if (g_url_focused) {
            if (ev.ascii == '\r' || ev.ascii == '\n' ||
                ev.keycode == OX_KEY_ENTER) {
                g_url_edit[g_url_edit_len] = 0;
                load_url(g_url_edit);
                render();
                continue;
            }
            if (ev.ascii == '\b' || ev.keycode == OX_KEY_BACKSPACE) {
                url_backspace();
                render();
                continue;
            }
            if (ev.keycode == OX_KEY_LEFT && g_url_caret > 0) {
                g_url_caret--; render(); continue;
            }
            if (ev.keycode == OX_KEY_RIGHT && g_url_caret < g_url_edit_len) {
                g_url_caret++; render(); continue;
            }
            if (ev.keycode == OX_KEY_HOME) {
                g_url_caret = 0; render(); continue;
            }
            if (ev.keycode == OX_KEY_END) {
                g_url_caret = g_url_edit_len; render(); continue;
            }
            if (ev.ascii >= 0x20 && ev.ascii < 0x7f) {
                url_insert((char)ev.ascii);
                render();
                continue;
            }
            continue;
        }

        /* Body has focus: scroll keys + back. */
        if (ev.keycode == OX_KEY_BACKSPACE || ev.ascii == '\b') {
            go_back();
            render();
            continue;
        }
        int max_scroll = view_total_lines() - VIS_LINES;
        if (max_scroll < 0) max_scroll = 0;
        int new_scroll = g_scroll;
        if      (ev.keycode == OX_KEY_UP)   new_scroll--;
        else if (ev.keycode == OX_KEY_DOWN) new_scroll++;
        else if (ev.keycode == OX_KEY_PGUP) new_scroll -= VIS_LINES - 1;
        else if (ev.keycode == OX_KEY_PGDN) new_scroll += VIS_LINES - 1;
        else if (ev.keycode == OX_KEY_HOME) new_scroll = 0;
        else if (ev.keycode == OX_KEY_END)  new_scroll = max_scroll;
        if (new_scroll < 0)          new_scroll = 0;
        if (new_scroll > max_scroll) new_scroll = max_scroll;
        if (new_scroll != g_scroll) {
            g_scroll = new_scroll;
            render();
        }
    }

    ox_window_destroy(g_win);
    return 0;
}
