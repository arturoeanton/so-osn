#include "socket.h"

#include "../micro/timer.h"
#include "eth.h"
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
    if (type != OSNOS_SOCK_DGRAM) return -1;   /* SOCK_STREAM: 8.5.5 */

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
