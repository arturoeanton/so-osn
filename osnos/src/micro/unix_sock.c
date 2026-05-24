#include "unix_sock.h"

#include "uaccess.h"
#include "../lib/string.h"

/* Pool fijo de sockets + registro de paths bound. */
static unix_sock_t  unix_socks[UNIX_SOCK_MAX];

typedef struct {
    bool used;
    int  sock_idx;
    char path[OSNOS_UNIX_PATH_MAX];
} unix_bind_entry_t;

static unix_bind_entry_t unix_binds[UNIX_BIND_MAX];

void unix_sock_init(void) {
    for (int i = 0; i < UNIX_SOCK_MAX; i++) {
        unix_socks[i].state = UNIX_SOCK_UNUSED;
        unix_socks[i].peer  = -1;
    }
    for (int i = 0; i < UNIX_BIND_MAX; i++) {
        unix_binds[i].used = false;
    }
}

/* Helpers internos. */

static unix_sock_t *sock_at(int idx) {
    if (idx < 0 || idx >= UNIX_SOCK_MAX) return 0;
    if (unix_socks[idx].state == UNIX_SOCK_UNUSED) return 0;
    return &unix_socks[idx];
}

static int alloc_sock(void) {
    for (int i = 0; i < UNIX_SOCK_MAX; i++) {
        if (unix_socks[i].state == UNIX_SOCK_UNUSED) return i;
    }
    return -1;
}

static int find_bind(const char *path) {
    for (int i = 0; i < UNIX_BIND_MAX; i++) {
        if (unix_binds[i].used && os_streq(unix_binds[i].path, path)) {
            return i;
        }
    }
    return -1;
}

static int alloc_bind(void) {
    for (int i = 0; i < UNIX_BIND_MAX; i++) {
        if (!unix_binds[i].used) return i;
    }
    return -1;
}

static void reset_sock(int idx) {
    unix_sock_t *s = &unix_socks[idx];
    s->state         = UNIX_SOCK_UNUSED;
    s->type          = 0;
    s->path[0]       = 0;
    s->peer          = -1;
    s->rx_head       = 0;
    s->rx_tail       = 0;
    s->backlog_head  = 0;
    s->backlog_tail  = 0;
    s->backlog_count = 0;
}

/* Ring buffer helpers — power-of-two NO requerido; usamos head/tail
 * con módulo simple, mantenemos un slot vacío para distinguir
 * vacío vs lleno. */

static size_t rx_bytes_available(const unix_sock_t *s) {
    if (s->rx_tail >= s->rx_head) return s->rx_tail - s->rx_head;
    return UNIX_BUF_BYTES - s->rx_head + s->rx_tail;
}

static size_t rx_bytes_free(const unix_sock_t *s) {
    /* -1 para distinguir full de empty. */
    return UNIX_BUF_BYTES - rx_bytes_available(s) - 1;
}

/* Public API */

int unix_sock_create(int type) {
    if (type != 1 /* SOCK_STREAM */) return -(int)OSNOS_EAFNOSUPPORT;
    int idx = alloc_sock();
    if (idx < 0) return -(int)OSNOS_EMFILE;
    reset_sock(idx);
    unix_sock_t *s = &unix_socks[idx];
    s->state = UNIX_SOCK_UNBOUND;
    s->type  = type;
    return idx;
}

int unix_sock_bind(int idx, const char *path) {
    unix_sock_t *s = sock_at(idx);
    if (!s)                          return -(int)OSNOS_EBADF;
    if (s->state != UNIX_SOCK_UNBOUND) return -(int)OSNOS_EINVAL;
    if (!path || !path[0])           return -(int)OSNOS_EINVAL;
    if (find_bind(path) >= 0)        return -(int)OSNOS_EADDRINUSE;

    int b = alloc_bind();
    if (b < 0)                        return -(int)OSNOS_ENFILE;

    os_strlcpy(unix_binds[b].path, path, OSNOS_UNIX_PATH_MAX);
    unix_binds[b].sock_idx = idx;
    unix_binds[b].used     = true;
    os_strlcpy(s->path, path, OSNOS_UNIX_PATH_MAX);
    return 0;
}

int unix_sock_listen(int idx, int backlog) {
    (void)backlog;
    unix_sock_t *s = sock_at(idx);
    if (!s)                              return -(int)OSNOS_EBADF;
    if (!s->path[0])                     return -(int)OSNOS_EINVAL;
    if (s->state != UNIX_SOCK_UNBOUND &&
        s->state != UNIX_SOCK_LISTENING) return -(int)OSNOS_EINVAL;
    s->state = UNIX_SOCK_LISTENING;
    return 0;
}

int unix_sock_connect(int idx, const char *path) {
    unix_sock_t *cli = sock_at(idx);
    if (!cli)                            return -(int)OSNOS_EBADF;
    if (cli->state != UNIX_SOCK_UNBOUND) return -(int)OSNOS_EISCONN;
    if (!path || !path[0])               return -(int)OSNOS_EINVAL;

    int b = find_bind(path);
    if (b < 0)                            return -(int)OSNOS_ENOENT;

    int lidx = unix_binds[b].sock_idx;
    unix_sock_t *listener = sock_at(lidx);
    if (!listener || listener->state != UNIX_SOCK_LISTENING) {
        return -(int)OSNOS_ECONNREFUSED;
    }
    if (listener->backlog_count >= UNIX_BACKLOG_MAX) {
        return -(int)OSNOS_ECONNREFUSED;
    }

    /* Crear el peer side — el socket que accept() retornará al
     * server. Forma un par con cli: ambos quedan CONNECTED, cada uno
     * con peer = el otro. */
    int srv_idx = alloc_sock();
    if (srv_idx < 0)                     return -(int)OSNOS_ENFILE;
    reset_sock(srv_idx);

    unix_sock_t *srv = &unix_socks[srv_idx];
    srv->state = UNIX_SOCK_CONNECTED;
    srv->type  = cli->type;
    srv->peer  = idx;

    cli->state = UNIX_SOCK_CONNECTED;
    cli->peer  = srv_idx;

    /* Encolar en backlog del listener. */
    listener->backlog[listener->backlog_tail] = srv_idx;
    listener->backlog_tail = (listener->backlog_tail + 1) % UNIX_BACKLOG_MAX;
    listener->backlog_count++;

    return 0;
}

int unix_sock_accept(int listen_idx) {
    unix_sock_t *l = sock_at(listen_idx);
    if (!l)                              return -(int)OSNOS_EBADF;
    if (l->state != UNIX_SOCK_LISTENING) return -(int)OSNOS_EINVAL;
    if (l->backlog_count == 0)           return -(int)OSNOS_EAGAIN;

    int srv_idx = l->backlog[l->backlog_head];
    l->backlog_head = (l->backlog_head + 1) % UNIX_BACKLOG_MAX;
    l->backlog_count--;
    return srv_idx;
}

bool unix_sock_readable(int idx) {
    unix_sock_t *s = sock_at(idx);
    if (!s) return false;
    if (s->state == UNIX_SOCK_LISTENING) {
        return s->backlog_count > 0;
    }
    if (rx_bytes_available(s) > 0) return true;
    /* Peer disconnected + buffer drained → readable returns true so
     * caller's read() can pick up the EOF. */
    return s->state == UNIX_SOCK_DISCONNECTED;
}

int unix_sock_recv(int idx, void *buf, size_t len) {
    unix_sock_t *s = sock_at(idx);
    if (!s)                              return -(int)OSNOS_EBADF;
    if (s->state != UNIX_SOCK_CONNECTED &&
        s->state != UNIX_SOCK_DISCONNECTED) return -(int)OSNOS_ENOTCONN;
    if (len == 0) return 0;

    size_t avail = rx_bytes_available(s);
    if (avail == 0) {
        if (s->state == UNIX_SOCK_DISCONNECTED) return 0; /* EOF */
        return -(int)OSNOS_EAGAIN;
    }
    size_t n = avail < len ? avail : len;

    /* Drain via dos memcpy si wrap. Hago staging y un solo
     * copy_to_user para uniformidad. */
    static uint8_t scratch[UNIX_BUF_BYTES];
    for (size_t i = 0; i < n; i++) {
        scratch[i] = s->rx[(s->rx_head + i) % UNIX_BUF_BYTES];
    }
    if (copy_to_user(buf, scratch, n) != OSNOS_OK) {
        return -(int)OSNOS_EFAULT;
    }
    s->rx_head = (s->rx_head + n) % UNIX_BUF_BYTES;
    return (int)n;
}

int unix_sock_send(int idx, const void *buf, size_t len) {
    unix_sock_t *s = sock_at(idx);
    if (!s)                              return -(int)OSNOS_EBADF;
    if (s->state != UNIX_SOCK_CONNECTED) return -(int)OSNOS_EPIPE;
    unix_sock_t *peer = sock_at(s->peer);
    if (!peer || peer->state != UNIX_SOCK_CONNECTED) return -(int)OSNOS_EPIPE;
    if (len == 0) return 0;

    size_t freebytes = rx_bytes_free(peer);
    if (freebytes == 0) return -(int)OSNOS_EAGAIN;
    size_t n = freebytes < len ? freebytes : len;

    static uint8_t scratch[UNIX_BUF_BYTES];
    if (copy_from_user(scratch, buf, n) != OSNOS_OK) {
        return -(int)OSNOS_EFAULT;
    }
    for (size_t i = 0; i < n; i++) {
        peer->rx[(peer->rx_tail + i) % UNIX_BUF_BYTES] = scratch[i];
    }
    peer->rx_tail = (peer->rx_tail + n) % UNIX_BUF_BYTES;
    return (int)n;
}

void unix_sock_close(int idx) {
    unix_sock_t *s = sock_at(idx);
    if (!s) return;

    /* Liberar bind si era listener. Rechazo de pendings: marcamos
     * a cada pending como DISCONNECTED (su lado client va a ver EOF
     * o ECONNRESET en próximo I/O). */
    if (s->state == UNIX_SOCK_LISTENING) {
        for (int i = 0; i < UNIX_BIND_MAX; i++) {
            if (unix_binds[i].used && unix_binds[i].sock_idx == idx) {
                unix_binds[i].used = false;
                unix_binds[i].path[0] = 0;
            }
        }
        while (s->backlog_count > 0) {
            int p = s->backlog[s->backlog_head];
            s->backlog_head = (s->backlog_head + 1) % UNIX_BACKLOG_MAX;
            s->backlog_count--;
            unix_sock_t *pending = sock_at(p);
            if (pending) {
                /* Mark pending and its client peer as disconnected. */
                pending->state = UNIX_SOCK_DISCONNECTED;
                unix_sock_t *cli = sock_at(pending->peer);
                if (cli && cli->state == UNIX_SOCK_CONNECTED) {
                    cli->state = UNIX_SOCK_DISCONNECTED;
                }
            }
        }
    }

    /* Si conectado, marcar el peer como DISCONNECTED — sus reads van
     * a drainear los bytes pendientes y luego retornar 0 (EOF). */
    if (s->state == UNIX_SOCK_CONNECTED) {
        unix_sock_t *peer = sock_at(s->peer);
        if (peer && peer->state == UNIX_SOCK_CONNECTED) {
            peer->state = UNIX_SOCK_DISCONNECTED;
        }
    }

    reset_sock(idx);
}
