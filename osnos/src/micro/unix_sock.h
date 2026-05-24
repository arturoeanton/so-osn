#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../include/osnos_unix_abi.h"
#include "../include/osnos_limits.h"
#include "../include/osnos_status.h"

/*
 * unix_sock — AF_UNIX SOCK_STREAM en kernel space.
 *
 * Modelo simple, single-process-style: pool fijo de sockets +
 * pool fijo de pathnames bound. Cada socket es uno de:
 *   - UNBOUND: recien creado, ni listening ni connected.
 *   - LISTENING: bound a un path, tiene cola de pending connects.
 *   - CONNECTED: peer = otro socket, ring buffers full-duplex.
 *
 * Sin abstract namespace, sin SOCK_DGRAM, sin permisos.
 * Suficiente para X11 client/server, dbus simple, IPC casero.
 */

#define UNIX_SOCK_MAX        32      /* total sockets vivos */
#define UNIX_BIND_MAX        16      /* paths bound listening */
#define UNIX_BACKLOG_MAX      8      /* pending connects per listener */
#define UNIX_BUF_BYTES     4096      /* per-direction ring buffer */

typedef enum {
    UNIX_SOCK_UNUSED = 0,
    UNIX_SOCK_UNBOUND,
    UNIX_SOCK_LISTENING,
    UNIX_SOCK_CONNECTED,
    UNIX_SOCK_DISCONNECTED,   /* peer closed; reads drain then EOF */
} unix_sock_state_t;

typedef struct unix_sock {
    unix_sock_state_t state;
    int               type;       /* SOCK_STREAM=1 (only one supported) */

    /* Path bound to (LISTENING only). For CONNECTED, vacío. */
    char              path[OSNOS_UNIX_PATH_MAX];

    /* Listener-only: pending connects (sockets que llamaron connect()
     * y esperan accept). Ring. */
    int               backlog[UNIX_BACKLOG_MAX];
    int               backlog_head;
    int               backlog_tail;
    int               backlog_count;

    /* Connected-only: peer socket index. -1 si no aplica. */
    int               peer;

    /* Connected-only: ring buffer de bytes que el PEER nos escribió y
     * NOSOTROS leemos. Cada socket es dueño de su buffer de lectura;
     * peer.rx == my.tx semánticamente. */
    uint8_t           rx[UNIX_BUF_BYTES];
    size_t            rx_head;     /* próximo byte a leer */
    size_t            rx_tail;     /* próximo slot a escribir */
} unix_sock_t;

void unix_sock_init(void);

/* Crea un socket UNBOUND, retorna su índice (>=0) o -OSNOS_EMFILE. */
int  unix_sock_create(int type);

/* Bind a un pathname. El path queda registrado globalmente; otros
 * sockets pueden connect() a él. ENADDRINUSE si ya bound. */
int  unix_sock_bind(int idx, const char *path);

/* Marca un socket BOUND como LISTENING. */
int  unix_sock_listen(int idx, int backlog);

/* Connect: busca un socket LISTENING bound a `path`. Si lo encuentra,
 * crea el peer socket internamente y lo encola en el backlog del
 * listener; queda en CONNECTED apuntando al recién-creado. El recién-
 * creado también queda CONNECTED apuntando a nosotros. Retorna 0 o
 * -OSNOS_ECONNREFUSED / -OSNOS_ENOENT. */
int  unix_sock_connect(int idx, const char *path);

/* Accept: extrae el primer socket pendiente del backlog. Retorna su
 * índice (otro socket ya CONNECTED) o -OSNOS_EAGAIN si vacío. */
int  unix_sock_accept(int listen_idx);

/* True si hay bytes para leer o si el peer se desconectó. */
bool unix_sock_readable(int idx);

/* Leer hasta `len` bytes del rx buffer. Retorna bytes leídos, 0 si
 * vacío + peer disconnected (EOF), o -OSNOS_EAGAIN si vacío y peer
 * vivo. Hace UN copy_to_user — caller pasa puntero user. */
int  unix_sock_recv(int idx, void *buf, size_t len);

/* Escribir hasta `len` bytes al rx buffer del peer. Retorna bytes
 * escritos, -OSNOS_EPIPE si peer disconnected, -OSNOS_EAGAIN si lleno. */
int  unix_sock_send(int idx, const void *buf, size_t len);

/* Cierra el socket. Si CONNECTED, marca al peer como DISCONNECTED
 * (sus reads recibirán EOF cuando drainen). Si LISTENING, libera el
 * pathname y rechaza pendings. */
void unix_sock_close(int idx);
