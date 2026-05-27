/*
 * /bin/oxnetsurf — HTML browser built on NetSurf's libhubbub + libdom.
 *
 * Architecture:
 *   1. fetch URL via plain socket (HTTP) or BearSSL (HTTPS)
 *   2. feed body to dom_hubbub_parser → get dom_document
 *   3. walk DOM extracting text + <a href> links into a flat view buf
 *   4. render in a scrollable Ox window with clickable links
 *
 * Built against musl (vendor/musl) because libparserutils needs iconv,
 * libdom needs regex.h, etc. — none of which mini-libc ships.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <ox.h>
#include <ox_ui.h>

/* libhubbub headers come FIRST so HUBBUB_* enums are defined before
 * libdom's binding/hubbub/errors.h references them. */
#include "hubbub/hubbub.h"
#include "hubbub/errors.h"
#include "hubbub/parser.h"
/* libdom's hubbub binding header lives under bindings/hubbub/parser.h
 * — same basename as libhubbub's own parser.h. Use the explicit
 * bindings/ prefix to disambiguate. */
#include "dom/dom.h"
#include "bindings/hubbub/parser.h"

/* BearSSL for HTTPS (vendored same as oxbrowser). */
#include "bearssl.h"

/* --- Logging direct to /dev/ttyS0 (musl stderr doesn't reach serial
 * for spawned-by-oxsrv children). --- */
static int  g_log_fd = -1;
static void nslog(const char *fmt, ...) {
    if (g_log_fd < 0) g_log_fd = open("/dev/ttyS0", O_WRONLY);
    if (g_log_fd < 0) return;
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    write(g_log_fd, buf, (size_t)n);
}

/* --- Geometry / colors --- */
#define WIN_W       900
#define WIN_H       600
#define URLBAR_H     32
#define STATUS_H     16
#define LINE_H       12
#define CHAR_W        8
#define BODY_Y       (URLBAR_H + 4)
#define BODY_H       (WIN_H - URLBAR_H - STATUS_H - 4)
#define VIS_LINES    (BODY_H / LINE_H)

#define COL_BG          OX_RGB(252, 252, 248)
#define COL_TEXT        OX_RGB( 20,  20,  25)
#define COL_HEADING     OX_RGB(  0,  60, 130)
#define COL_LINK        OX_RGB( 30,  90, 200)
#define COL_LINK_HOVER  OX_RGB(192,  28,  40)
#define COL_URLBAR_BG   OX_RGB( 30,  30,  35)
#define COL_URLBAR_FG   OX_RGB(220, 220, 220)
#define COL_URL_EDIT    OX_RGB(245, 245, 245)
#define COL_URL_EDIT_FG OX_RGB( 20,  20,  30)
#define COL_GO_BTN      OX_RGB( 53, 132, 228)
#define COL_GO_BTN_HOV  OX_RGB( 80, 158, 255)
#define COL_GO_BTN_FG   OX_RGB(255, 255, 255)
#define COL_STATUS_BG   OX_RGB( 40,  40,  50)
#define COL_STATUS_FG   OX_RGB(220, 220, 220)
#define COL_CARET       OX_RGB( 53, 132, 228)

#define URL_MAX     512
#define VIEW_MAX    (256 * 1024)
#define LINK_MAX    256
#define RESP_MAX    (256 * 1024)
#define HOST_MAX    256

/* --- State --- */
static ox_win_t g_win;
static int  g_w = WIN_W, g_h = WIN_H;
static char g_url_edit[URL_MAX] = "http://httpbin.org/html";
static int  g_url_len = 0;
static int  g_url_cur = 0;
static int  g_url_focused = 1;
static char g_url_current[URL_MAX] = "";

static char g_view[VIEW_MAX];
static int  g_view_len = 0;
static int  g_scroll = 0;

typedef struct {
    int  start;
    int  end;
    char href[URL_MAX];
} link_t;
static link_t g_links[LINK_MAX];
static int    g_link_count = 0;
static int    g_link_hover = -1;

static char g_status[256] = "Type a URL and press Enter or click Go";
static int  g_status_err = 0;
static int  g_go_hover = 0;

/* --- view helpers --- */
static void view_putc(char c) {
    if (g_view_len < VIEW_MAX - 1) g_view[g_view_len++] = c;
}
static void view_puts(const char *s) {
    while (*s) view_putc(*s++);
}
static int view_total_lines(void) {
    int n = 1;
    for (int i = 0; i < g_view_len; i++) if (g_view[i] == '\n') n++;
    return n;
}

/* --- URL parsing --- */
static int parse_url(const char *url, char *host, size_t host_cap,
                      int *out_port, char *path, size_t path_cap,
                      int *is_https) {
    *is_https = 0;
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) { *is_https = 1; p += 8; *out_port = 443; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; *out_port = 80; }
    else return -1;
    int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi + 1 < (int)host_cap)
        host[hi++] = *p++;
    host[hi] = 0;
    if (*p == ':') {
        p++;
        int port = 0;
        while (*p >= '0' && *p <= '9') { port = port*10 + (*p++ - '0'); }
        *out_port = port;
    }
    if (*p == '/') {
        size_t i = 0;
        while (*p && i + 1 < path_cap) path[i++] = *p++;
        path[i] = 0;
    } else {
        path[0] = '/'; path[1] = 0;
    }
    return 0;
}

/* --- HTTP fetch (plain) --- */
static int http_fetch(const char *host, int port, const char *path,
                       char *resp, size_t cap, size_t *out_len) {
    *out_len = 0;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_s[8];
    snprintf(port_s, sizeof(port_s), "%d", port);
    int gai = getaddrinfo(host, port_s, &hints, &res);
    if (gai != 0 || !res) {
        nslog("oxnetsurf: getaddrinfo(%s) failed gai=%d errno=%d\n",
              host, gai, errno);
        return -1;
    }
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) { freeaddrinfo(res); return -1; }
    if (connect(s, res->ai_addr, res->ai_addrlen) < 0) {
        nslog("oxnetsurf: connect failed errno=%d\n", errno);
        close(s); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    char req[1024];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: oxnetsurf/0.1\r\n"
        "Accept: text/html\r\nConnection: close\r\n\r\n",
        path, host);
    if (write(s, req, rlen) != rlen) { close(s); return -1; }
    size_t got = 0;
    while (got < cap - 1) {
        ssize_t n = read(s, resp + got, cap - 1 - got);
        if (n <= 0) break;
        got += n;
    }
    close(s);
    resp[got] = 0;
    *out_len = got;
    return 0;
}

/* --- HTTPS fetch using BearSSL (no-anchor X.509). Adapted from oxbrowser. --- */
typedef struct {
    const br_x509_class *vtable;
    br_x509_minimal_context minimal;
} xwc_ctx;
static void xwc_start_chain(const br_x509_class **c, const char *sn) {
    xwc_ctx *x = (xwc_ctx *)c; x->minimal.vtable->start_chain(&x->minimal.vtable, sn);
}
static void xwc_start_cert(const br_x509_class **c, uint32_t l) {
    xwc_ctx *x = (xwc_ctx *)c; x->minimal.vtable->start_cert(&x->minimal.vtable, l);
}
static void xwc_append(const br_x509_class **c, const unsigned char *b, size_t l) {
    xwc_ctx *x = (xwc_ctx *)c; x->minimal.vtable->append(&x->minimal.vtable, b, l);
}
static void xwc_end_cert(const br_x509_class **c) {
    xwc_ctx *x = (xwc_ctx *)c; x->minimal.vtable->end_cert(&x->minimal.vtable);
}
static unsigned xwc_end_chain(const br_x509_class **c) {
    xwc_ctx *x = (xwc_ctx *)c;
    unsigned r = x->minimal.vtable->end_chain(&x->minimal.vtable);
    if (r == BR_ERR_X509_NOT_TRUSTED ||
        r == BR_ERR_X509_TIME_UNKNOWN ||
        r == BR_ERR_X509_EXPIRED) return 0;
    return r;
}
static const br_x509_pkey *xwc_get_pkey(const br_x509_class *const *c, unsigned *usages) {
    const xwc_ctx *x = (const xwc_ctx *)c;
    return x->minimal.vtable->get_pkey(&x->minimal.vtable, usages);
}
static const br_x509_class xwc_vtable = {
    sizeof(xwc_ctx), xwc_start_chain, xwc_start_cert, xwc_append,
    xwc_end_cert, xwc_end_chain, xwc_get_pkey
};

static int https_fetch(const char *host, int port, const char *path,
                        char *resp, size_t cap, size_t *out_len) {
    *out_len = 0;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_s[8];
    snprintf(port_s, sizeof(port_s), "%d", port);
    if (getaddrinfo(host, port_s, &hints, &res) != 0 || !res) return -1;
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) { freeaddrinfo(res); return -1; }
    if (connect(s, res->ai_addr, res->ai_addrlen) < 0) {
        close(s); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);

    static br_ssl_client_context sc;
    static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    static xwc_ctx xnc;
    br_ssl_client_init_full(&sc, &xnc.minimal, NULL, 0);
    xnc.vtable = &xwc_vtable;
    br_ssl_engine_set_x509(&sc.eng, &xnc.vtable);
    br_ssl_engine_set_versions(&sc.eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof(iobuf), 1);
    br_ssl_client_reset(&sc, host, 0);

    char req[1024];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: oxnetsurf/0.1\r\n"
        "Accept: text/html\r\nConnection: close\r\n\r\n", path, host);

    int sent_req = 0;
    size_t got = 0;
    for (;;) {
        unsigned st = br_ssl_engine_current_state(&sc.eng);
        if (st & BR_SSL_CLOSED) break;
        if (st & BR_SSL_SENDREC) {
            size_t avail;
            unsigned char *p = br_ssl_engine_sendrec_buf(&sc.eng, &avail);
            ssize_t w = write(s, p, avail);
            if (w <= 0) break;
            br_ssl_engine_sendrec_ack(&sc.eng, w);
            continue;
        }
        if ((st & BR_SSL_SENDAPP) && !sent_req) {
            size_t avail;
            unsigned char *p = br_ssl_engine_sendapp_buf(&sc.eng, &avail);
            int to_send = (rlen < (int)avail) ? rlen : (int)avail;
            memcpy(p, req, to_send);
            br_ssl_engine_sendapp_ack(&sc.eng, to_send);
            br_ssl_engine_flush(&sc.eng, 0);
            sent_req = 1;
            continue;
        }
        if (st & BR_SSL_RECVAPP) {
            size_t avail;
            unsigned char *p = br_ssl_engine_recvapp_buf(&sc.eng, &avail);
            size_t to_copy = (got + avail < cap - 1) ? avail : (cap - 1 - got);
            memcpy(resp + got, p, to_copy);
            got += to_copy;
            br_ssl_engine_recvapp_ack(&sc.eng, to_copy);
            continue;
        }
        if (st & BR_SSL_RECVREC) {
            size_t avail;
            unsigned char *p = br_ssl_engine_recvrec_buf(&sc.eng, &avail);
            ssize_t r = read(s, p, avail);
            if (r <= 0) { br_ssl_engine_close(&sc.eng); continue; }
            br_ssl_engine_recvrec_ack(&sc.eng, r);
            continue;
        }
        break;
    }
    close(s);
    resp[got] = 0;
    *out_len = got;
    int err = br_ssl_engine_last_error(&sc.eng);
    if (err != 0 && err != BR_ERR_OK) {
        nslog("oxnetsurf: TLS engine err=%d\n", err);
    }
    return got > 0 ? 0 : -1;
}

/* --- DOM walk --- */

/* Element-by-element handling. Block elements add newlines around
 * their text content; inline elements just contribute text. */
static int is_block_tag(const char *t) {
    if (!t) return 0;
    static const char *blocks[] = {
        "p", "div", "h1", "h2", "h3", "h4", "h5", "h6",
        "li", "ul", "ol", "br", "tr", "blockquote", "pre",
        "section", "article", "header", "footer", "nav",
        "hr", "table", "form", "fieldset", NULL
    };
    for (int i = 0; blocks[i]; i++) if (strcmp(t, blocks[i]) == 0) return 1;
    return 0;
}
static int is_skip_tag(const char *t) {
    if (!t) return 0;
    static const char *skip[] = {
        "script", "style", "head", "meta", "link", "noscript", "svg", NULL
    };
    for (int i = 0; skip[i]; i++) if (strcmp(t, skip[i]) == 0) return 1;
    return 0;
}

static void walk_dom(dom_node *node) {
    dom_node_type type;
    if (dom_node_get_node_type(node, &type) != DOM_NO_ERR) return;

    if (type == DOM_TEXT_NODE) {
        dom_string *text = NULL;
        if (dom_node_get_text_content(node, &text) == DOM_NO_ERR && text) {
            const char *d = dom_string_data(text);
            size_t L = dom_string_byte_length(text);
            /* Collapse whitespace. */
            int last_space = (g_view_len > 0 && g_view[g_view_len-1] == ' ');
            int last_nl    = (g_view_len > 0 && g_view[g_view_len-1] == '\n');
            for (size_t i = 0; i < L; i++) {
                char c = d[i];
                if (c == '\r' || c == '\n' || c == '\t') c = ' ';
                if (c == ' ') {
                    if (!last_space && !last_nl) { view_putc(' '); last_space = 1; }
                } else {
                    view_putc(c);
                    last_space = 0; last_nl = 0;
                }
            }
            dom_string_unref(text);
        }
        return;
    }

    if (type != DOM_ELEMENT_NODE) {
        /* Recurse into other node types (e.g., document). */
    }

    dom_string *name_s = NULL;
    if (type == DOM_ELEMENT_NODE)
        dom_node_get_node_name(node, &name_s);

    char tag[32] = "";
    if (name_s) {
        const char *d = dom_string_data(name_s);
        size_t L = dom_string_byte_length(name_s);
        if (L >= sizeof(tag)) L = sizeof(tag) - 1;
        for (size_t i = 0; i < L; i++) {
            char c = d[i];
            tag[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        tag[L] = 0;
    }

    if (tag[0] && is_skip_tag(tag)) {
        if (name_s) dom_string_unref(name_s);
        return;
    }

    int is_a   = (strcmp(tag, "a") == 0);
    int is_blk = is_block_tag(tag);
    int link_idx = -1;
    int link_start = g_view_len;

    /* Block-level: ensure leading newline. */
    if (is_blk && g_view_len > 0 && g_view[g_view_len-1] != '\n')
        view_putc('\n');

    /* Heading prefix marker. */
    if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == 0) {
        for (int i = 0; i < (tag[1] - '0'); i++) view_putc('#');
        view_putc(' ');
    }
    /* List item bullet. */
    if (strcmp(tag, "li") == 0) view_puts(" * ");
    /* Horizontal rule. */
    if (strcmp(tag, "hr") == 0) {
        view_puts("------------------------------\n");
    }

    /* Link tracking. */
    if (is_a && g_link_count < LINK_MAX) {
        dom_string *href_attr = NULL;
        static dom_string *href_name = NULL;
        if (!href_name) {
            dom_string_create((const uint8_t *)"href", 4, &href_name);
        }
        if (href_name && dom_element_get_attribute(node, href_name, &href_attr) == DOM_NO_ERR && href_attr) {
            const char *hd = dom_string_data(href_attr);
            size_t hL = dom_string_byte_length(href_attr);
            if (hL >= sizeof(g_links[0].href)) hL = sizeof(g_links[0].href) - 1;
            link_idx = g_link_count++;
            memcpy(g_links[link_idx].href, hd, hL);
            g_links[link_idx].href[hL] = 0;
            g_links[link_idx].start = link_start;
            dom_string_unref(href_attr);
        }
    }

    /* Recurse children. */
    dom_node *child = NULL;
    if (dom_node_get_first_child(node, &child) == DOM_NO_ERR && child) {
        while (child) {
            walk_dom(child);
            dom_node *next = NULL;
            dom_node_get_next_sibling(child, &next);
            dom_node_unref(child);
            child = next;
        }
    }

    if (link_idx >= 0) g_links[link_idx].end = g_view_len;

    if (is_blk && g_view_len > 0 && g_view[g_view_len-1] != '\n')
        view_putc('\n');
    if (strcmp(tag, "br") == 0)
        view_putc('\n');

    if (name_s) dom_string_unref(name_s);
}

/* --- Parse + walk --- */
static int parse_html(const char *body, size_t body_len) {
    g_view_len = 0;
    g_link_count = 0;
    g_link_hover = -1;
    g_scroll = 0;

    dom_hubbub_parser_params params = {0};
    params.enc = "UTF-8";
    params.fix_enc = true;
    params.enable_script = false;
    params.msg = NULL;
    params.script = NULL;
    params.ctx = NULL;
    params.daf = NULL;

    dom_hubbub_parser *parser = NULL;
    dom_document *doc = NULL;
    dom_hubbub_error err = dom_hubbub_parser_create(&params, &parser, &doc);
    if (err != DOM_HUBBUB_OK) {
        nslog("oxnetsurf: dom_hubbub_parser_create err=%d\n", err);
        return -1;
    }

    err = dom_hubbub_parser_parse_chunk(parser, (const uint8_t *)body, body_len);
    if (err != DOM_HUBBUB_OK) {
        nslog("oxnetsurf: parse_chunk err=%d (len=%zu)\n", err, body_len);
    }
    dom_hubbub_parser_completed(parser);

    dom_element *root = NULL;
    if (dom_document_get_document_element(doc, &root) == DOM_NO_ERR && root) {
        walk_dom((dom_node *)root);
        dom_node_unref(root);
    }

    dom_hubbub_parser_destroy(parser);
    if (doc) dom_node_unref(doc);
    return 0;
}

/* --- Fetch + parse --- */
static char g_resp[RESP_MAX];

static void load_url(const char *url) {
    char host[HOST_MAX];
    char path[URL_MAX];
    int port, is_https;
    if (parse_url(url, host, sizeof(host), &port, path, sizeof(path), &is_https) < 0) {
        snprintf(g_status, sizeof(g_status), "Bad URL: %s", url);
        g_status_err = 1;
        return;
    }
    snprintf(g_status, sizeof(g_status), "Fetching %s...", host);
    g_status_err = 0;
    nslog("oxnetsurf: fetching %s host=%s port=%d path=%s https=%d\n",
          url, host, port, path, is_https);

    size_t resp_len = 0;
    int r = is_https
        ? https_fetch(host, port, path, g_resp, sizeof(g_resp), &resp_len)
        : http_fetch (host, port, path, g_resp, sizeof(g_resp), &resp_len);
    if (r < 0 || resp_len == 0) {
        snprintf(g_status, sizeof(g_status), "Fetch failed: %s", url);
        g_status_err = 1;
        return;
    }
    nslog("oxnetsurf: got %zu bytes\n", resp_len);

    /* Split headers from body at first "\r\n\r\n" or "\n\n". */
    const char *body = NULL;
    size_t body_len = 0;
    for (size_t i = 0; i + 3 < resp_len; i++) {
        if (g_resp[i] == '\r' && g_resp[i+1] == '\n' &&
            g_resp[i+2] == '\r' && g_resp[i+3] == '\n') {
            body = g_resp + i + 4;
            body_len = resp_len - i - 4;
            break;
        }
        if (g_resp[i] == '\n' && g_resp[i+1] == '\n') {
            body = g_resp + i + 2;
            body_len = resp_len - i - 2;
            break;
        }
    }
    if (!body) { body = g_resp; body_len = resp_len; }

    parse_html(body, body_len);

    snprintf(g_url_current, sizeof(g_url_current), "%s", url);
    snprintf(g_status, sizeof(g_status),
             "%s %zu bytes, %d lines, %d links",
             is_https ? "HTTPS" : "HTTP",
             body_len, view_total_lines(), g_link_count);
}

/* --- Rendering --- */

#define GO_BTN_W 56
static int go_btn_x(void) { return g_w - GO_BTN_W - 6; }

static int line_start_for(int line_no) {
    int line = 0;
    for (int i = 0; i < g_view_len; i++) {
        if (line == line_no) return i;
        if (g_view[i] == '\n') line++;
    }
    return g_view_len;
}

static int char_offset_to_line_col(int off, int *line, int *col) {
    int L = 0, C = 0;
    for (int i = 0; i < off && i < g_view_len; i++) {
        if (g_view[i] == '\n') { L++; C = 0; }
        else C++;
    }
    *line = L; *col = C;
    return 0;
}

static int xy_to_offset(int x, int y) {
    if (y < BODY_Y) return -1;
    int row = (y - BODY_Y) / LINE_H + g_scroll;
    int target_col = (x - 8) / CHAR_W;
    if (target_col < 0) target_col = 0;
    int line = 0, col = 0;
    for (int i = 0; i < g_view_len; i++) {
        if (line == row && col == target_col) return i;
        if (g_view[i] == '\n') {
            if (line == row) return i;
            line++; col = 0;
        } else col++;
    }
    return g_view_len;
}

static int link_at_offset(int off) {
    for (int i = 0; i < g_link_count; i++) {
        if (off >= g_links[i].start && off < g_links[i].end) return i;
    }
    return -1;
}

static void render(void) {
    /* URL bar. */
    ox_draw_rect(g_win, 0, 0, g_w, URLBAR_H, COL_URLBAR_BG);
    ox_draw_text(g_win, 6, 10, "URL", COL_URLBAR_FG);
    int ux = 32, uw = g_w - 32 - GO_BTN_W - 12;
    ox_draw_rect(g_win, ux, 6, uw, URLBAR_H - 12,
                 g_url_focused ? OX_RGB(255, 255, 255) : COL_URL_EDIT);
    ox_draw_rect(g_win, ux, 6, uw, 1, OX_RGB(0, 0, 0));
    ox_draw_rect(g_win, ux, URLBAR_H - 7, uw, 1, OX_RGB(0, 0, 0));
    ox_draw_rect(g_win, ux, 6, 1, URLBAR_H - 12, OX_RGB(0, 0, 0));
    ox_draw_rect(g_win, ux + uw - 1, 6, 1, URLBAR_H - 12, OX_RGB(0, 0, 0));
    int max_chars = (uw - 12) / CHAR_W;
    int start = 0;
    if (g_url_cur > max_chars) start = g_url_cur - max_chars + 1;
    char view[URL_MAX];
    int n = g_url_len - start;
    if (n > max_chars) n = max_chars;
    if (n > 0) memcpy(view, g_url_edit + start, n);
    view[n > 0 ? n : 0] = 0;
    ox_draw_text(g_win, ux + 6, 12, view, COL_URL_EDIT_FG);
    if (g_url_focused) {
        int cx = ux + 6 + (g_url_cur - start) * CHAR_W;
        ox_draw_rect(g_win, cx, 8, 2, URLBAR_H - 16, COL_CARET);
    }

    /* Go button. */
    ox_draw_rect(g_win, go_btn_x(), 6, GO_BTN_W, URLBAR_H - 12,
                 g_go_hover ? COL_GO_BTN_HOV : COL_GO_BTN);
    ox_draw_text(g_win, go_btn_x() + 18, 12, "Go", COL_GO_BTN_FG);

    /* Body. */
    ox_draw_rect(g_win, 0, BODY_Y, g_w, BODY_H, COL_BG);
    int row = 0;
    int i = 0;
    int line_no = 0;
    while (i < g_view_len && line_no < g_scroll) {
        if (g_view[i] == '\n') line_no++;
        i++;
    }
    while (row < VIS_LINES) {
        int y = BODY_Y + row * LINE_H;
        int x = 8;
        int col = 0;
        while (i < g_view_len && g_view[i] != '\n' && x + CHAR_W < g_w - 4) {
            int link = link_at_offset(i);
            uint32_t fg = (link >= 0)
                ? (link == g_link_hover ? COL_LINK_HOVER : COL_LINK)
                : COL_TEXT;
            char s[2] = { g_view[i], 0 };
            ox_draw_text(g_win, x, y, s, fg);
            x += CHAR_W;
            col++;
            i++;
        }
        if (i < g_view_len && g_view[i] == '\n') i++;
        row++;
        line_no++;
        if (i >= g_view_len) break;
    }

    /* Status. */
    ox_draw_rect(g_win, 0, g_h - STATUS_H, g_w, STATUS_H, COL_STATUS_BG);
    ox_draw_text(g_win, 8, g_h - STATUS_H + 4, g_status,
                 g_status_err ? OX_RGB(255, 120, 120) : COL_STATUS_FG);

    ox_present(g_win);
}

/* --- URL edit helpers --- */
static void url_insert(char c) {
    if (g_url_len + 1 >= URL_MAX) return;
    memmove(g_url_edit + g_url_cur + 1, g_url_edit + g_url_cur,
            g_url_len - g_url_cur);
    g_url_edit[g_url_cur++] = c;
    g_url_len++;
    g_url_edit[g_url_len] = 0;
}
static void url_backspace(void) {
    if (g_url_cur <= 0) return;
    memmove(g_url_edit + g_url_cur - 1, g_url_edit + g_url_cur,
            g_url_len - g_url_cur);
    g_url_cur--; g_url_len--;
    g_url_edit[g_url_len] = 0;
}

/* --- main --- */
int main(int argc, char **argv) {
    nslog("oxnetsurf: starting argc=%d\n", argc);
    if (argc > 1 && argv[1] && argv[1][0]) {
        size_t L = strlen(argv[1]);
        if (L >= URL_MAX) L = URL_MAX - 1;
        memcpy(g_url_edit, argv[1], L);
        g_url_edit[L] = 0;
        g_url_len = L;
        g_url_cur = L;
    } else {
        g_url_len = strlen(g_url_edit);
        g_url_cur = g_url_len;
    }
    if (ox_init() < 0) { nslog("oxnetsurf: ox_init failed\n"); return 1; }
    g_win = ox_window_create(g_w, g_h, "NetSurf");
    if (g_win < 0) { nslog("oxnetsurf: window_create failed\n"); return 1; }
    nslog("oxnetsurf: window created\n");
    render();

    /* If launched with a URL, auto-fetch. */
    if (argc > 1 && argv[1] && argv[1][0]) {
        load_url(g_url_edit);
        render();
    }

    for (;;) {
        ox_event_t ev;
        if (!ox_wait_event(&ev)) continue;
        if (ev.type == OX_EV_CLOSE) break;

        if (ev.type == OX_EV_MOUSE) {
            int over_go = (ev.x >= go_btn_x() && ev.x < go_btn_x() + GO_BTN_W &&
                           ev.y >= 6 && ev.y < URLBAR_H - 6);
            if (over_go != g_go_hover) { g_go_hover = over_go; render(); }

            if (ev.mouse_kind == OX_MOUSE_WHEEL) {
                int total = view_total_lines();
                int max = total - VIS_LINES;
                if (max < 0) max = 0;
                g_scroll -= ev.wheel_delta * 3;
                if (g_scroll < 0) g_scroll = 0;
                if (g_scroll > max) g_scroll = max;
                render();
                continue;
            }

            if (ev.mouse_kind == OX_MOUSE_MOVE) {
                int off = xy_to_offset(ev.x, ev.y);
                int link = (off >= 0) ? link_at_offset(off) : -1;
                if (link != g_link_hover) { g_link_hover = link; render(); }
                continue;
            }

            if (ev.mouse_kind == OX_MOUSE_DOWN && (ev.buttons & 0x01)) {
                int over_url = (ev.y < URLBAR_H && ev.x > 28 && ev.x < go_btn_x() - 4);
                if (over_url) { g_url_focused = 1; render(); continue; }
                if (over_go) {
                    load_url(g_url_edit);
                    render();
                    continue;
                }
                /* Click on body — link? */
                int off = xy_to_offset(ev.x, ev.y);
                int link = (off >= 0) ? link_at_offset(off) : -1;
                if (link >= 0) {
                    /* Resolve relative URL. */
                    char target[URL_MAX];
                    const char *href = g_links[link].href;
                    if (strncmp(href, "http://", 7) == 0 ||
                        strncmp(href, "https://", 8) == 0) {
                        snprintf(target, sizeof(target), "%s", href);
                    } else if (href[0] == '/') {
                        /* Same host, absolute path. */
                        char base[URL_MAX], host[HOST_MAX], path[URL_MAX];
                        int port, is_https;
                        snprintf(base, sizeof(base), "%s", g_url_current);
                        parse_url(base, host, sizeof(host), &port, path, sizeof(path), &is_https);
                        snprintf(target, sizeof(target), "%s://%s%s",
                                 is_https ? "https" : "http", host, href);
                    } else {
                        snprintf(target, sizeof(target), "%s/%s", g_url_current, href);
                    }
                    snprintf(g_url_edit, URL_MAX, "%s", target);
                    g_url_len = strlen(g_url_edit);
                    g_url_cur = g_url_len;
                    load_url(target);
                    render();
                    continue;
                }
                g_url_focused = 0;
                render();
                continue;
            }
            continue;
        }

        if (ev.type == OX_EV_KEY && g_url_focused) {
            if (ev.ascii == '\r' || ev.ascii == '\n' || ev.keycode == OX_KEY_ENTER) {
                load_url(g_url_edit);
                render();
                continue;
            }
            if (ev.ascii == 0x08 || ev.keycode == OX_KEY_BACKSPACE) {
                url_backspace(); render(); continue;
            }
            if (ev.keycode == OX_KEY_LEFT) {
                if (g_url_cur > 0) g_url_cur--; render(); continue;
            }
            if (ev.keycode == OX_KEY_RIGHT) {
                if (g_url_cur < g_url_len) g_url_cur++; render(); continue;
            }
            if (ev.keycode == OX_KEY_HOME) { g_url_cur = 0; render(); continue; }
            if (ev.keycode == OX_KEY_END)  { g_url_cur = g_url_len; render(); continue; }
            if (ev.ascii >= 0x20 && ev.ascii < 0x7f) {
                url_insert((char)ev.ascii);
                render();
                continue;
            }
        } else if (ev.type == OX_EV_KEY) {
            int total = view_total_lines();
            int max = total - VIS_LINES;
            if (max < 0) max = 0;
            if (ev.keycode == OX_KEY_UP && g_scroll > 0) { g_scroll--; render(); }
            if (ev.keycode == OX_KEY_DOWN && g_scroll < max) { g_scroll++; render(); }
            if (ev.keycode == OX_KEY_PGUP) {
                g_scroll -= VIS_LINES; if (g_scroll < 0) g_scroll = 0; render();
            }
            if (ev.keycode == OX_KEY_PGDN) {
                g_scroll += VIS_LINES; if (g_scroll > max) g_scroll = max; render();
            }
        }
    }
    ox_window_destroy(g_win);
    return 0;
}
