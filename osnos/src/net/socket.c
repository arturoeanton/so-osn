#include "socket.h"

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
} sock_t;

static sock_t socks[SOCK_MAX];
static uint16_t ephemeral_next = 40000;

static inline void cli(void) { __asm__ volatile ("cli"); }
static inline void sti(void) { __asm__ volatile ("sti"); }

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

int sock_recvfrom(int sd, void *buf, size_t buf_len,
                   uint32_t *src_ip, uint16_t *src_port,
                   uint32_t timeout_ms) {
    sock_t *s = sock_at(sd);
    if (!s) return -1;
    if (s->local_port == 0) return -1;     /* unbound */

    uint64_t deadline = timer_ms() + timeout_ms;
    for (;;) {
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
    (void)backlog;
    sock_t *s = sock_at(sd);
    if (!s) return -1;
    if (s->type != OSNOS_SOCK_STREAM) return -1;
    if (s->local_port == 0) return -1;          /* must bind first */
    s->tcp_state = TCP_LISTEN;
    return 0;
}

bool sock_tcp_get_peer(int sd, uint32_t *ip_out, uint16_t *port_out) {
    sock_t *s = sock_at(sd);
    if (!s) return false;
    if (s->type != OSNOS_SOCK_STREAM) return false;
    if (s->tcp_state != TCP_ESTABLISHED) return false;
    if (ip_out)   *ip_out   = s->remote_ip;
    if (port_out) *port_out = s->remote_port;
    return true;
}

void sock_tcp_reset(int sd) {
    sock_t *s = sock_at(sd);
    if (!s) return;
    if (s->type != OSNOS_SOCK_STREAM) return;

    /* If we know a peer, slap a RST on it. snd_nxt holds the next
     * unsent seq, which is exactly what the peer expects in their
     * receive window for an immediate teardown. */
    if (s->remote_ip != 0 && s->local_port != 0) {
        tcp_send(s->remote_ip, s->remote_port,
                  s->local_port, s->snd_nxt, s->rcv_nxt,
                  TCP_FLAG_RST | TCP_FLAG_ACK, 0, NULL, 0);
    }

    cli();
    s->tcp_state   = TCP_LISTEN;
    s->remote_ip   = 0;
    s->remote_port = 0;
    s->snd_nxt = s->snd_una = s->rcv_nxt = 0;
    sti();
}

/*
 * Single-connection-per-socket simplification: a LISTEN socket
 * transitions through SYN_RCVD → ESTABLISHED in-place when its first
 * SYN arrives. accept() (8.5.5c) will introduce real child sockets
 * pulled from a backlog queue.
 */
void sock_tcp_handle_segment(uint32_t src_ip, uint16_t src_port,
                               uint32_t dst_ip, uint16_t dst_port,
                               uint32_t seq, uint32_t ack, uint8_t flags) {
    (void)dst_ip;

    for (int i = 0; i < SOCK_MAX; i++) {
        sock_t *s = &socks[i];
        if (!s->used) continue;
        if (s->type != OSNOS_SOCK_STREAM) continue;
        if (s->local_port != dst_port) continue;

        /* For non-LISTEN states the connection 4-tuple must match. */
        if (s->tcp_state != TCP_LISTEN && s->tcp_state != TCP_CLOSED) {
            if (s->remote_ip != src_ip || s->remote_port != src_port) continue;
        }

        switch (s->tcp_state) {
        case TCP_LISTEN:
            if (flags & TCP_FLAG_RST) return;
            if (flags & TCP_FLAG_SYN) {
                cli();
                s->remote_ip   = src_ip;
                s->remote_port = src_port;
                /* Cheap ISN: timer + slot index. Not security-relevant
                 * yet (no SYN-flood defense). */
                s->snd_una = s->snd_nxt = (uint32_t)timer_ms() + (uint32_t)i * 1000;
                s->rcv_nxt = seq + 1;                /* SYN consumes 1 seq */
                s->tcp_state = TCP_SYN_RCVD;
                sti();

                tcp_send(src_ip, src_port, dst_port,
                          s->snd_nxt, s->rcv_nxt,
                          TCP_FLAG_SYN | TCP_FLAG_ACK, 8192,
                          NULL, 0);
                s->snd_nxt++;                         /* our SYN consumes 1 */
            }
            return;

        case TCP_SYN_RCVD:
            if (flags & TCP_FLAG_RST) {
                cli();
                s->tcp_state = TCP_LISTEN;
                s->remote_ip = 0;
                s->remote_port = 0;
                sti();
                return;
            }
            if ((flags & TCP_FLAG_ACK) && ack == s->snd_nxt) {
                cli();
                s->snd_una = ack;
                s->tcp_state = TCP_ESTABLISHED;
                sti();
            }
            return;

        case TCP_ESTABLISHED:
            /* 8.5.5a: no data transfer. RST anything that isn't a
             * keep-alive ACK so the peer doesn't hang. */
            if (flags & TCP_FLAG_RST) {
                cli();
                s->tcp_state = TCP_LISTEN;
                s->remote_ip = 0;
                s->remote_port = 0;
                sti();
                return;
            }
            if (flags & (TCP_FLAG_FIN | TCP_FLAG_PSH)) {
                tcp_send(src_ip, src_port, dst_port,
                          s->snd_nxt, seq + 1,
                          TCP_FLAG_RST, 0, NULL, 0);
                cli();
                s->tcp_state = TCP_LISTEN;
                s->remote_ip = 0;
                s->remote_port = 0;
                sti();
            }
            return;

        default:
            return;
        }
    }

    /*
     * No socket bound to dst_port — bounce an RST so the peer doesn't
     * stay in SYN_SENT until its retransmission timer expires.
     */
    if (!(flags & TCP_FLAG_RST)) {
        uint32_t reply_ack = seq + ((flags & TCP_FLAG_SYN) ? 1 : 0);
        tcp_send(src_ip, src_port, dst_port,
                  ack, reply_ack,
                  TCP_FLAG_RST | TCP_FLAG_ACK, 0, NULL, 0);
    }
}
