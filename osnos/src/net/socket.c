#include "socket.h"

#include "../micro/task.h"
#include "../micro/timer.h"
#include "eth.h"
#include "tcp.h"
#include "udp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool      valid;
    uint32_t  src_ip;
    uint16_t  src_port;
    uint16_t  len;
    uint8_t   data[SOCK_RX_MAX_DGRAM];
} sock_rx_slot_t;

typedef struct {
    bool             used;
    int              type;
    uint32_t         local_ip;
    uint16_t         local_port;
    sock_rx_slot_t   rx[SOCK_RX_QUEUE_DEPTH];
    uint8_t          rx_head;     /* next slot to enqueue into */
    uint8_t          rx_tail;     /* next slot to dequeue from */
    uint8_t          rx_count;

    /* TCP-only fields (valid when type == SOCK_STREAM). */
    tcp_state_t      tcp_state;
    uint32_t         remote_ip;
    uint16_t         remote_port;
    uint32_t         snd_nxt;     /* next byte we'll send (1 byte for SYN/FIN) */
    uint32_t         snd_una;     /* oldest unacked byte */
    uint32_t         rcv_nxt;     /* next byte we expect to receive */

    /* RX byte ring buffer (stream semantics). */
    uint8_t          tcp_rx[SOCK_TCP_RX_BUF];
    uint32_t         tcp_rx_head;
    uint32_t         tcp_rx_tail;
    uint32_t         tcp_rx_count;

    /* True when the user called sock_close_tcp but FIN exchange is
     * still draining; the slot is freed when state hits CLOSED. */
    bool             zombie;
    /* True when peer's FIN was processed → recv returns 0 when buf
     * drains. */
    bool             peer_fin;

    /* Backlog parent / child wiring for LISTEN+accept.
     *   parent_sd >= 0   on child sockets (refers to the LISTEN slot).
     *   parent_sd == -1  on regular / LISTEN sockets.
     * The LISTEN socket owns accept_q: a ring of child sd's that have
     * reached ESTABLISHED but not yet been pulled by accept(). */
    int              parent_sd;
    int              accept_q[SOCK_ACCEPT_QUEUE_DEPTH];
    uint8_t          accept_q_head;
    uint8_t          accept_q_tail;
    uint8_t          accept_q_count;
    uint8_t          backlog;
} sock_t;

static sock_t socks[SOCK_MAX];
static uint16_t ephemeral_next = 40000;

static inline void cli(void) { __asm__ volatile ("cli"); }
static inline void sti(void) { __asm__ volatile ("sti"); }

/* True when the current task has a pending kill (e.g. Ctrl+C) and the
 * busy-poll loop should bail out so the syscall can return early. */
static bool poll_interrupted(void) {
    task_t *t = task_current();
    return t && t->kill_pending;
}

static sock_t *sock_at(int sd) {
    if (sd < 0 || sd >= SOCK_MAX) return 0;
    if (!socks[sd].used) return 0;
    return &socks[sd];
}

int sock_create(int type) {
    if (type != OSNOS_SOCK_DGRAM && type != OSNOS_SOCK_STREAM) return -1;

    for (int i = 0; i < SOCK_MAX; i++) {
        if (socks[i].used) continue;
        sock_t *s = &socks[i];
        s->used       = true;
        s->type       = type;
        s->local_ip   = 0;
        s->local_port = 0;
        s->rx_head    = 0;
        s->rx_tail    = 0;
        s->rx_count   = 0;
        s->tcp_state  = TCP_CLOSED;
        s->remote_ip  = 0;
        s->remote_port = 0;
        s->snd_nxt    = 0;
        s->snd_una    = 0;
        s->rcv_nxt    = 0;
        s->tcp_rx_head = 0;
        s->tcp_rx_tail = 0;
        s->tcp_rx_count = 0;
        s->zombie     = false;
        s->peer_fin   = false;
        s->parent_sd  = -1;
        s->accept_q_head = 0;
        s->accept_q_tail = 0;
        s->accept_q_count = 0;
        s->backlog    = 0;
        for (int k = 0; k < SOCK_RX_QUEUE_DEPTH; k++) s->rx[k].valid = false;
        return i;
    }
    return -1;
}

static bool port_in_use(uint16_t port) {
    for (int i = 0; i < SOCK_MAX; i++) {
        if (socks[i].used && socks[i].local_port == port) return true;
    }
    return false;
}

int sock_bind(int sd, uint32_t ip, uint16_t port) {
    sock_t *s = sock_at(sd);
    if (!s) return -1;
    if (s->local_port != 0) return -1;    /* already bound */

    if (port == 0) {
        /* Ephemeral port — pick the next free one. */
        for (int tries = 0; tries < 1000; tries++) {
            uint16_t p = ephemeral_next++;
            if (ephemeral_next > 60000) ephemeral_next = 40000;
            if (p < 40000) continue;
            if (!port_in_use(p)) { port = p; break; }
        }
        if (port == 0) return -1;
    } else {
        if (port_in_use(port)) return -1;
    }

    s->local_ip   = ip;
    s->local_port = port;
    return 0;
}

int sock_close(int sd) {
    sock_t *s = sock_at(sd);
    if (!s) return -1;
    if (s->type == OSNOS_SOCK_STREAM) {
        /* TCP: orderly close path. Slot is freed asynchronously
         * when the FIN exchange completes. */
        return sock_close_tcp(sd);
    }
    cli();
    s->used       = false;
    s->local_port = 0;
    s->rx_count   = 0;
    sti();
    return 0;
}

uint16_t sock_local_port(int sd) {
    sock_t *s = sock_at(sd);
    return s ? s->local_port : 0;
}

bool sock_readable(int sd) {
    /* Volatile reads — this is called in tight busy-poll loops where
     * the watched counters are mutated from the NIC IRQ. Without the
     * volatile coerce, clang -O2 cached the read and select() never
     * saw the IRQ's update. */
    volatile sock_t *s = (volatile sock_t *)sock_at(sd);
    if (!s) return false;

    if (s->type == OSNOS_SOCK_DGRAM) {
        return s->rx_count > 0;
    }
    if (s->type == OSNOS_SOCK_STREAM) {
        tcp_state_t st = s->tcp_state;
        if (st == TCP_LISTEN) {
            return s->accept_q_count > 0;
        }
        if (st == TCP_ESTABLISHED ||
            st == TCP_CLOSE_WAIT  ||
            st == TCP_FIN_WAIT_1  ||
            st == TCP_FIN_WAIT_2) {
            return s->tcp_rx_count > 0 || s->peer_fin;
        }
    }
    return false;
}

int sock_recvfrom(int sd, void *buf, size_t buf_len,
                   uint32_t *src_ip, uint16_t *src_port,
                   uint32_t timeout_ms) {
    sock_t *s = sock_at(sd);
    if (!s) return -1;
    if (s->local_port == 0) return -1;     /* unbound */

    uint64_t deadline = timer_ms() + timeout_ms;
    for (;;) {
        if (poll_interrupted()) return -1;
        cli();
        if (s->rx_count > 0) {
            sock_rx_slot_t *slot = &s->rx[s->rx_tail];
            uint16_t n = slot->len;
            if (n > buf_len) n = (uint16_t)buf_len;
            for (uint16_t i = 0; i < n; i++) {
                ((uint8_t *)buf)[i] = slot->data[i];
            }
            if (src_ip)   *src_ip   = slot->src_ip;
            if (src_port) *src_port = slot->src_port;
            slot->valid = false;
            s->rx_tail = (uint8_t)((s->rx_tail + 1) % SOCK_RX_QUEUE_DEPTH);
            s->rx_count--;
            sti();
            return (int)n;
        }
        sti();

        if (timer_ms() >= deadline) return 0;
    }
}

int sock_sendto(int sd, const void *buf, size_t len,
                 uint32_t dst_ip, uint16_t dst_port) {
    sock_t *s = sock_at(sd);
    if (!s) return -1;

    /* Auto-bind to an ephemeral port if the caller didn't bind yet. */
    if (s->local_port == 0) {
        if (sock_bind(sd, 0, 0) != 0) return -1;
    }

    if (!udp_send(dst_ip, dst_port, s->local_port, buf, len)) return -1;
    return (int)len;
}

bool sock_deliver_udp(uint32_t src_ip, uint16_t src_port,
                       uint32_t dst_ip, uint16_t dst_port,
                       const uint8_t *data, size_t len) {
    (void)dst_ip;                       /* INADDR_ANY only for now */

    for (int i = 0; i < SOCK_MAX; i++) {
        sock_t *s = &socks[i];
        if (!s->used) continue;
        if (s->type != OSNOS_SOCK_DGRAM) continue;
        if (s->local_port != dst_port) continue;

        cli();
        if (s->rx_count >= SOCK_RX_QUEUE_DEPTH) {
            sti();
            return false;               /* queue full — drop */
        }
        sock_rx_slot_t *slot = &s->rx[s->rx_head];
        if (len > SOCK_RX_MAX_DGRAM) len = SOCK_RX_MAX_DGRAM;
        slot->src_ip   = src_ip;
        slot->src_port = src_port;
        slot->len      = (uint16_t)len;
        for (size_t k = 0; k < len; k++) slot->data[k] = data[k];
        slot->valid = true;
        s->rx_head = (uint8_t)((s->rx_head + 1) % SOCK_RX_QUEUE_DEPTH);
        s->rx_count++;
        sti();
        return true;
    }
    return false;                       /* no listener */
}

/* ================================================================ */
/* TCP (8.5.5a: passive handshake)                                  */
/* ================================================================ */

int sock_listen(int sd, int backlog) {
    sock_t *s = sock_at(sd);
    if (!s) return -1;
    if (s->type != OSNOS_SOCK_STREAM) return -1;
    if (s->local_port == 0) return -1;
    s->tcp_state = TCP_LISTEN;
    if (backlog < 1) backlog = 1;
    if (backlog > SOCK_ACCEPT_QUEUE_DEPTH) backlog = SOCK_ACCEPT_QUEUE_DEPTH;
    s->backlog = (uint8_t)backlog;
    return 0;
}

int sock_accept(int listen_sd,
                 uint32_t *peer_ip, uint16_t *peer_port,
                 uint32_t timeout_ms) {
    sock_t *s = sock_at(listen_sd);
    if (!s) return -1;
    if (s->type != OSNOS_SOCK_STREAM) return -1;
    if (s->tcp_state != TCP_LISTEN) return -1;

    uint64_t deadline = timer_ms() + timeout_ms;
    for (;;) {
        if (poll_interrupted()) return -1;
        cli();
        if (s->accept_q_count > 0) {
            int child = s->accept_q[s->accept_q_head];
            s->accept_q_head = (uint8_t)((s->accept_q_head + 1) % SOCK_ACCEPT_QUEUE_DEPTH);
            s->accept_q_count--;
            sti();
            if (peer_ip)   *peer_ip   = socks[child].remote_ip;
            if (peer_port) *peer_port = socks[child].remote_port;
            return child;
        }
        sti();
        if (timer_ms() >= deadline) return -2;
    }
}

bool sock_tcp_get_peer(int sd, uint32_t *ip_out, uint16_t *port_out) {
    sock_t *s = sock_at(sd);
    if (!s) return false;
    if (s->type != OSNOS_SOCK_STREAM) return false;

    /* LISTEN socket: peek at the next pending child (without consuming). */
    if (s->tcp_state == TCP_LISTEN) {
        if (s->accept_q_count == 0) return false;
        int child = s->accept_q[s->accept_q_head];
        if (ip_out)   *ip_out   = socks[child].remote_ip;
        if (port_out) *port_out = socks[child].remote_port;
        return true;
    }
    if (s->tcp_state != TCP_ESTABLISHED) return false;
    if (ip_out)   *ip_out   = s->remote_ip;
    if (port_out) *port_out = s->remote_port;
    return true;
}

static void tcp_reset_socket(sock_t *s);    /* fwd ref */

void sock_tcp_reset(int sd) {
    sock_t *s = sock_at(sd);
    if (!s) return;
    if (s->type != OSNOS_SOCK_STREAM) return;

    if (s->remote_ip != 0 && s->local_port != 0) {
        tcp_send(s->remote_ip, s->remote_port,
                  s->local_port, s->snd_nxt, s->rcv_nxt,
                  TCP_FLAG_RST | TCP_FLAG_ACK, 0, NULL, 0);
    }
    tcp_reset_socket(s);
}

/*
 * Append payload bytes to the socket's RX ring. Called from IRQ
 * context (already holds cli implicitly via the asm stub). Returns
 * how many bytes were actually accepted (drops the rest when full).
 */
static size_t tcp_rx_enqueue(sock_t *s, const uint8_t *p, size_t n) {
    size_t free = SOCK_TCP_RX_BUF - s->tcp_rx_count;
    if (n > free) n = free;
    for (size_t i = 0; i < n; i++) {
        s->tcp_rx[s->tcp_rx_tail] = p[i];
        s->tcp_rx_tail = (s->tcp_rx_tail + 1) % SOCK_TCP_RX_BUF;
    }
    s->tcp_rx_count += (uint32_t)n;
    return n;
}

static uint16_t tcp_advertised_window(const sock_t *s) {
    return (uint16_t)(SOCK_TCP_RX_BUF - s->tcp_rx_count);
}

/* Send a bare ACK reflecting current snd_nxt / rcv_nxt. */
static void tcp_emit_ack(const sock_t *s) {
    tcp_send(s->remote_ip, s->remote_port, s->local_port,
              s->snd_nxt, s->rcv_nxt,
              TCP_FLAG_ACK, tcp_advertised_window(s),
              NULL, 0);
}

static void tcp_emit_fin_ack(sock_t *s) {
    tcp_send(s->remote_ip, s->remote_port, s->local_port,
              s->snd_nxt, s->rcv_nxt,
              TCP_FLAG_FIN | TCP_FLAG_ACK, tcp_advertised_window(s),
              NULL, 0);
    s->snd_nxt++;       /* FIN consumes 1 seq */
}

static void tcp_emit_rst(uint32_t dst_ip, uint16_t dst_port,
                          uint16_t src_port, uint32_t seq, uint32_t ack) {
    tcp_send(dst_ip, dst_port, src_port, seq, ack,
              TCP_FLAG_RST | TCP_FLAG_ACK, 0, NULL, 0);
}

/* Wipe TCP connection state and clear the zombie flag. */
static void tcp_reset_socket(sock_t *s) {
    cli();
    if (s->zombie) {
        s->used = false;
        s->zombie = false;
    } else if (s->tcp_state != TCP_CLOSED) {
        s->tcp_state = TCP_LISTEN;
    }
    s->remote_ip = 0;
    s->remote_port = 0;
    s->snd_nxt = s->snd_una = s->rcv_nxt = 0;
    s->tcp_rx_head = s->tcp_rx_tail = s->tcp_rx_count = 0;
    s->peer_fin = false;
    sti();
}

/* Try to allocate + initialise a child socket for an incoming SYN.
 * Returns the child index, or -1 if the table is full or the parent's
 * accept backlog is already saturated (caller drops the SYN). */
static int alloc_child_for_syn(int parent_idx,
                                 uint32_t peer_ip, uint16_t peer_port,
                                 uint16_t local_port, uint32_t peer_seq) {
    sock_t *p = &socks[parent_idx];
    /* Pending children in SYN_RCVD that haven't queued yet also count
     * against backlog — easier: cap on accept_q_count. Some SYN-floods
     * still slip through but we're not defending today. */
    if (p->accept_q_count >= p->backlog) return -1;

    for (int i = 0; i < SOCK_MAX; i++) {
        if (socks[i].used) continue;
        sock_t *c = &socks[i];
        /* Mirror sock_create's defaults so the slot is consistent. */
        c->used        = true;
        c->type        = OSNOS_SOCK_STREAM;
        c->local_ip    = 0;
        c->local_port  = local_port;
        c->rx_head = c->rx_tail = c->rx_count = 0;
        for (int k = 0; k < SOCK_RX_QUEUE_DEPTH; k++) c->rx[k].valid = false;
        c->tcp_rx_head = c->tcp_rx_tail = c->tcp_rx_count = 0;
        c->zombie = c->peer_fin = false;
        c->parent_sd = parent_idx;
        c->accept_q_head = c->accept_q_tail = c->accept_q_count = 0;
        c->backlog = 0;

        c->remote_ip   = peer_ip;
        c->remote_port = peer_port;
        c->snd_una = c->snd_nxt =
            (uint32_t)timer_ms() + (uint32_t)i * 1000;
        c->rcv_nxt = peer_seq + 1;
        c->tcp_state = TCP_SYN_RCVD;
        return i;
    }
    return -1;
}

void sock_tcp_handle_segment(uint32_t src_ip, uint16_t src_port,
                               uint32_t dst_ip, uint16_t dst_port,
                               uint32_t seq, uint32_t ack, uint8_t flags,
                               const uint8_t *payload, size_t payload_len) {
    (void)dst_ip;

    /*
     * Two-pass lookup: a connected child (4-tuple match) always wins
     * over the LISTEN parent that shares its local_port. Without this
     * order new data packets would be eaten by the LISTEN socket.
     */
    int idx = -1;
    for (int i = 0; i < SOCK_MAX; i++) {
        sock_t *s = &socks[i];
        if (!s->used) continue;
        if (s->type != OSNOS_SOCK_STREAM) continue;
        if (s->local_port != dst_port) continue;
        if (s->tcp_state == TCP_LISTEN || s->tcp_state == TCP_CLOSED) continue;
        if (s->remote_ip != src_ip || s->remote_port != src_port) continue;
        idx = i; break;
    }
    if (idx < 0) {
        for (int i = 0; i < SOCK_MAX; i++) {
            sock_t *s = &socks[i];
            if (!s->used) continue;
            if (s->type != OSNOS_SOCK_STREAM) continue;
            if (s->local_port != dst_port) continue;
            if (s->tcp_state != TCP_LISTEN) continue;
            idx = i; break;
        }
    }

    if (idx >= 0) {
        sock_t *s = &socks[idx];
        switch (s->tcp_state) {
        case TCP_LISTEN:
            if (flags & TCP_FLAG_RST) return;
            if (flags & TCP_FLAG_SYN) {
                int child = alloc_child_for_syn(idx, src_ip, src_port,
                                                  dst_port, seq);
                if (child < 0) return;                /* backlog full → drop */
                sock_t *c = &socks[child];
                tcp_send(src_ip, src_port, dst_port,
                          c->snd_nxt, c->rcv_nxt,
                          TCP_FLAG_SYN | TCP_FLAG_ACK,
                          tcp_advertised_window(c), NULL, 0);
                c->snd_nxt++;
            }
            return;

        case TCP_SYN_RCVD:
            if (flags & TCP_FLAG_RST) { tcp_reset_socket(s); return; }
            if ((flags & TCP_FLAG_ACK) && ack == s->snd_nxt) {
                s->snd_una = ack;
                s->tcp_state = TCP_ESTABLISHED;
                /* Hand the established child over to the LISTEN parent's
                 * accept queue. If full, just leave the child sitting
                 * in ESTABLISHED — the application will time out. */
                if (s->parent_sd >= 0) {
                    sock_t *p = &socks[s->parent_sd];
                    if (p->used && p->tcp_state == TCP_LISTEN &&
                        p->accept_q_count < SOCK_ACCEPT_QUEUE_DEPTH) {
                        p->accept_q[p->accept_q_tail] = idx;
                        p->accept_q_tail = (uint8_t)((p->accept_q_tail + 1)
                                                       % SOCK_ACCEPT_QUEUE_DEPTH);
                        p->accept_q_count++;
                    }
                }
            }
            return;

        case TCP_ESTABLISHED:
            if (flags & TCP_FLAG_RST) { tcp_reset_socket(s); return; }

            /* Advance snd_una on valid ACK. */
            if ((flags & TCP_FLAG_ACK) &&
                (int32_t)(ack - s->snd_una) > 0 &&
                (int32_t)(ack - s->snd_nxt) <= 0) {
                s->snd_una = ack;
            }

            /* In-order data only — drop anything else, peer retransmits. */
            if (payload_len > 0 && seq == s->rcv_nxt) {
                size_t got = tcp_rx_enqueue(s, payload, payload_len);
                s->rcv_nxt += (uint32_t)got;
                tcp_emit_ack(s);
            }

            if (flags & TCP_FLAG_FIN) {
                /* FIN sits one past the last byte of payload. */
                if (seq + payload_len == s->rcv_nxt) {
                    s->rcv_nxt++;
                    s->peer_fin = true;
                    s->tcp_state = TCP_CLOSE_WAIT;
                    tcp_emit_ack(s);
                }
            }
            return;

        case TCP_FIN_WAIT_1:
            if (flags & TCP_FLAG_RST) { tcp_reset_socket(s); return; }

            /* Accept any pending data while we wait for the FIN-ACK. */
            if (payload_len > 0 && seq == s->rcv_nxt) {
                size_t got = tcp_rx_enqueue(s, payload, payload_len);
                s->rcv_nxt += (uint32_t)got;
                tcp_emit_ack(s);
            }

            if ((flags & TCP_FLAG_ACK) && ack == s->snd_nxt) {
                s->snd_una = ack;
                s->tcp_state = TCP_FIN_WAIT_2;
            }
            if (flags & TCP_FLAG_FIN) {
                if (seq + payload_len == s->rcv_nxt) {
                    s->rcv_nxt++;
                    s->peer_fin = true;
                    tcp_emit_ack(s);
                    /* Simultaneous close path or just-after-our-FIN
                     * → CLOSED (we skip TIME_WAIT for simplicity). */
                    s->tcp_state = TCP_CLOSED;
                    if (s->zombie) { s->used = false; s->zombie = false; }
                }
            }
            return;

        case TCP_FIN_WAIT_2:
            if (flags & TCP_FLAG_RST) { tcp_reset_socket(s); return; }
            if (payload_len > 0 && seq == s->rcv_nxt) {
                size_t got = tcp_rx_enqueue(s, payload, payload_len);
                s->rcv_nxt += (uint32_t)got;
                tcp_emit_ack(s);
            }
            if (flags & TCP_FLAG_FIN) {
                if (seq + payload_len == s->rcv_nxt) {
                    s->rcv_nxt++;
                    s->peer_fin = true;
                    tcp_emit_ack(s);
                    s->tcp_state = TCP_CLOSED;
                    if (s->zombie) { s->used = false; s->zombie = false; }
                }
            }
            return;

        case TCP_CLOSE_WAIT:
            if (flags & TCP_FLAG_RST) { tcp_reset_socket(s); return; }
            /* Waiting for application to call close(). Ignore. */
            return;

        case TCP_LAST_ACK:
            if (flags & TCP_FLAG_RST) { tcp_reset_socket(s); return; }
            if ((flags & TCP_FLAG_ACK) && ack == s->snd_nxt) {
                s->tcp_state = TCP_CLOSED;
                if (s->zombie) { s->used = false; s->zombie = false; }
            }
            return;

        case TCP_CLOSING:
            if (flags & TCP_FLAG_RST) { tcp_reset_socket(s); return; }
            if ((flags & TCP_FLAG_ACK) && ack == s->snd_nxt) {
                s->tcp_state = TCP_CLOSED;
                if (s->zombie) { s->used = false; s->zombie = false; }
            }
            return;

        default:
            return;
        }
    }

    /* No matching socket — RST so the peer doesn't hang. */
    if (!(flags & TCP_FLAG_RST)) {
        uint32_t reply_ack = seq + payload_len +
                              ((flags & TCP_FLAG_SYN) ? 1 : 0) +
                              ((flags & TCP_FLAG_FIN) ? 1 : 0);
        tcp_emit_rst(src_ip, src_port, dst_port, ack, reply_ack);
    }
}

/* ================================================================ */
/* TCP user-facing API (recv / send / close)                        */
/* ================================================================ */

int sock_recv(int sd, void *buf, size_t buf_len, uint32_t timeout_ms) {
    sock_t *s = sock_at(sd);
    if (!s) return -1;
    if (s->type != OSNOS_SOCK_STREAM) return -1;

    uint64_t deadline = timer_ms() + timeout_ms;
    for (;;) {
        if (poll_interrupted()) return -1;
        cli();
        if (s->tcp_rx_count > 0) {
            size_t n = s->tcp_rx_count;
            if (n > buf_len) n = buf_len;
            uint8_t *out = (uint8_t *)buf;
            for (size_t i = 0; i < n; i++) {
                out[i] = s->tcp_rx[s->tcp_rx_head];
                s->tcp_rx_head = (s->tcp_rx_head + 1) % SOCK_TCP_RX_BUF;
            }
            s->tcp_rx_count -= (uint32_t)n;
            sti();
            return (int)n;
        }
        /* Buffer empty: EOF if peer has closed, else timeout/wait. */
        if (s->peer_fin || s->tcp_state == TCP_CLOSED) {
            sti();
            return 0;
        }
        sti();
        if (timer_ms() >= deadline) return -2;
    }
}

int sock_send(int sd, const void *buf, size_t len) {
    sock_t *s = sock_at(sd);
    if (!s) return -1;
    if (s->type != OSNOS_SOCK_STREAM) return -1;
    if (s->tcp_state != TCP_ESTABLISHED && s->tcp_state != TCP_CLOSE_WAIT) {
        return -1;
    }

    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > TCP_MSS) chunk = TCP_MSS;

        if (!tcp_send(s->remote_ip, s->remote_port, s->local_port,
                       s->snd_nxt, s->rcv_nxt,
                       TCP_FLAG_ACK | TCP_FLAG_PSH,
                       tcp_advertised_window(s),
                       p + sent, chunk)) {
            return sent > 0 ? (int)sent : -1;
        }
        s->snd_nxt += (uint32_t)chunk;
        sent += chunk;
    }
    return (int)sent;
}

int sock_close_tcp(int sd) {
    sock_t *s = sock_at(sd);
    if (!s) return -1;
    if (s->type != OSNOS_SOCK_STREAM) return -1;

    switch (s->tcp_state) {
    case TCP_ESTABLISHED:
        tcp_emit_fin_ack(s);
        s->tcp_state = TCP_FIN_WAIT_1;
        s->zombie = true;
        return 0;
    case TCP_CLOSE_WAIT:
        tcp_emit_fin_ack(s);
        s->tcp_state = TCP_LAST_ACK;
        s->zombie = true;
        return 0;
    case TCP_LISTEN:
    case TCP_CLOSED:
        cli();
        s->used = false;
        sti();
        return 0;
    default:
        /* Mid-handshake or already closing — let it drain. */
        s->zombie = true;
        return 0;
    }
}
