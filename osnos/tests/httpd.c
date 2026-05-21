/*
 * /bin/httpd — minimal HTTP/1.0 file server for FASE 8.5.10.
 *
 *   exec /bin/httpd [PORT]   (default port 80)
 *
 * Serves files from /sd/ over the TCP socket. Pair with the existing
 * QEMU hostfwd tcp::8080-:80 → from the Mac: `curl http://localhost:8080`.
 *
 * Wire format: HTTP/1.0 single-shot, Connection: close after every
 * response. No persistent connections, no keep-alive, no Range, no
 * chunked. Only GET. Path "/" maps to "/sd/index.html"; otherwise
 * "/foo/bar" maps to "/sd/foo/bar". `..` in the path is rejected so
 * clients can't escape the document root.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define DOCROOT      "/sd"
#define MAX_CONNS    50            /* serve this many then exit cleanly */
#define READ_CHUNK   2048

static const char *guess_mime(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html") || !strcmp(dot, ".htm"))  return "text/html";
    if (!strcmp(dot, ".txt"))                            return "text/plain";
    if (!strcmp(dot, ".css"))                            return "text/css";
    if (!strcmp(dot, ".js"))                             return "application/javascript";
    if (!strcmp(dot, ".json"))                           return "application/json";
    if (!strcmp(dot, ".png"))                            return "image/png";
    if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg"))   return "image/jpeg";
    if (!strcmp(dot, ".gif"))                            return "image/gif";
    return "application/octet-stream";
}

static void send_status(int fd, int code, const char *reason,
                         const char *body) {
    char hdr[256];
    int blen = body ? (int)strlen(body) : 0;
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %d\r\n"
        "Server: osnos/0.0\r\n"
        "Connection: close\r\n\r\n",
        code, reason, blen);
    send(fd, hdr, (size_t)hn, 0);
    if (blen) send(fd, body, (size_t)blen, 0);
}

/*
 * User stack is a single 4 KiB page on osnos today, so the big I/O
 * buffers have to live in BSS or we run straight off the end. Single
 * threaded → static is fine.
 */
static char g_chunk[READ_CHUNK];
static char g_hdr[256];
static char g_req[1024];
static char g_abs_path[256];

static void serve_file(int fd, const char *abs_path) {
    int f = open(abs_path, O_RDONLY);
    if (f < 0) {
        send_status(fd, 404, "Not Found",
                     "<h1>404 Not Found</h1>\n");
        return;
    }

    struct stat st;
    if (fstat(f, &st) < 0) {
        close(f);
        send_status(fd, 500, "Internal Server Error",
                     "<h1>500 fstat failed</h1>\n");
        return;
    }

    int hn = snprintf(g_hdr, sizeof(g_hdr),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lu\r\n"
        "Server: osnos/0.0\r\n"
        "Connection: close\r\n\r\n",
        guess_mime(abs_path), (unsigned long)st.st_size);
    ssize_t sn = send(fd, g_hdr, (size_t)hn, 0);
    if (sn < 0) {
        printf("  send hdr FAILED errno=%d\n", errno);
        close(f);
        return;
    }

    /* Stream the body in chunks rather than buffering the whole file. */
    int total = 0;
    for (;;) {
        ssize_t r = read(f, g_chunk, sizeof(g_chunk));
        if (r <= 0) break;
        sn = send(fd, g_chunk, (size_t)r, 0);
        if (sn < 0) {
            printf("  send body FAILED at %d errno=%d\n", total, errno);
            break;
        }
        total += (int)sn;
    }
    close(f);
    printf("  served %d body bytes\n", total);
}

static void handle_conn(int cli) {
    ssize_t n = recv(cli, g_req, sizeof(g_req) - 1, 0);
    if (n <= 0) return;
    g_req[n] = 0;
    char *req = g_req;

    if (strncmp(req, "GET ", 4) != 0) {
        send_status(cli, 405, "Method Not Allowed",
                     "<h1>405 only GET supported</h1>\n");
        return;
    }

    char *path = req + 4;
    char *end  = strchr(path, ' ');
    if (!end) {
        send_status(cli, 400, "Bad Request",
                     "<h1>400 malformed request</h1>\n");
        return;
    }
    *end = 0;

    if (strstr(path, "..")) {
        send_status(cli, 403, "Forbidden",
                     "<h1>403 path traversal blocked</h1>\n");
        return;
    }

    /* Trim trailing query / fragment — we don't pass them anywhere. */
    char *qmark = strchr(path, '?');
    if (qmark) *qmark = 0;
    char *frag = strchr(path, '#');
    if (frag)  *frag = 0;

    if (!strcmp(path, "/") || path[0] == 0) {
        snprintf(g_abs_path, sizeof(g_abs_path), "%s/index.html", DOCROOT);
    } else if (path[0] == '/') {
        snprintf(g_abs_path, sizeof(g_abs_path), "%s%s", DOCROOT, path);
    } else {
        send_status(cli, 400, "Bad Request",
                     "<h1>400 path must start with /</h1>\n");
        return;
    }

    printf("httpd: GET %s\n", g_abs_path);
    serve_file(cli, g_abs_path);
    (void)req;
}

int main(int argc, char **argv) {
    uint16_t port = 80;
    if (argc >= 2) {
        int v = atoi(argv[1]);
        if (v > 0 && v < 65536) port = (uint16_t)v;
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { printf("socket: errno=%d\n", errno); return 1; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    for (int i = 0; i < 8; i++) ((char *)addr.sin_zero)[i] = 0;

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("bind: errno=%d\n", errno);
        return 1;
    }
    if (listen(srv, 4) < 0) {
        printf("listen: errno=%d\n", errno);
        return 1;
    }

    printf("httpd: serving %s on TCP port %u (%d connections max)\n",
           DOCROOT, (unsigned)port, MAX_CONNS);

    for (int i = 0; i < MAX_CONNS; i++) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cli = accept(srv, (struct sockaddr *)&peer, &plen);
        if (cli < 0) {
            printf("accept: errno=%d\n", errno);
            break;
        }
        handle_conn(cli);
        close(cli);
    }

    close(srv);
    printf("httpd: done\n");
    return 0;
}
