#include "icmp.h"

#include "../micro/timer.h"
#include "ip.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ICMP wire layout (echo):
 *   0    type
 *   1    code
 *   2-3  checksum (BE)
 *   4-5  identifier (BE)
 *   6-7  sequence (BE)
 *   8...  payload (echoed verbatim)
 *
 * Checksum is the same Internet checksum used by IPv4 but spans the
 * ICMP header + payload, not the IP header.
 */

static uint64_t rx_packets;
static uint64_t tx_packets;

/* Pending-ping state. Single in-flight at a time — fine because
 * icmp_ping is synchronous: it sends, busy-polls, returns. */
static volatile struct {
    bool     waiting;
    uint32_t src_ip;
    uint16_t id;
    uint16_t seq;
    bool     replied;
} pending;

static inline uint16_t rd16_be(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline void wr16_be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

static void icmp_send(uint32_t dst_ip, uint8_t type,
                       uint16_t id, uint16_t seq,
                       const void *payload, size_t payload_len) {
    uint8_t buf[ICMP_HEADER_SIZE + 56];        /* 64-byte default ping size */
    if (payload_len > sizeof(buf) - ICMP_HEADER_SIZE) {
        payload_len = sizeof(buf) - ICMP_HEADER_SIZE;
    }

    buf[0] = type;
    buf[1] = 0;
    wr16_be(buf + 2, 0);                       /* checksum placeholder */
    wr16_be(buf + 4, id);
    wr16_be(buf + 6, seq);

    const uint8_t *p = (const uint8_t *)payload;
    for (size_t i = 0; i < payload_len; i++) buf[ICMP_HEADER_SIZE + i] = p[i];

    /* Checksum spans the whole ICMP message (header + payload). */
    uint16_t cs = ip_checksum(buf, ICMP_HEADER_SIZE + payload_len);
    wr16_be(buf + 2, cs);

    if (ip_send(dst_ip, IP_PROTO_ICMP, buf, ICMP_HEADER_SIZE + payload_len)) {
        tx_packets++;
    }
}

void icmp_handle(const uint8_t *data, size_t len, uint32_t src_ip) {
    if (len < ICMP_HEADER_SIZE) return;

    /* Verify checksum: full ICMP message including the checksum field
     * must sum to 0xFFFF (function returns 0). */
    if (ip_checksum(data, len) != 0) return;

    rx_packets++;

    uint8_t  type = data[0];
    uint16_t id   = rd16_be(data + 4);
    uint16_t seq  = rd16_be(data + 6);

    switch (type) {
    case ICMP_TYPE_ECHO_REQUEST:
        /* Echo the payload back unchanged with type=0. */
        icmp_send(src_ip, ICMP_TYPE_ECHO_REPLY,
                    id, seq,
                    data + ICMP_HEADER_SIZE, len - ICMP_HEADER_SIZE);
        break;

    case ICMP_TYPE_ECHO_REPLY:
        if (pending.waiting &&
            pending.src_ip == src_ip &&
            pending.id == id &&
            pending.seq == seq) {
            pending.replied = true;
        }
        break;

    default:
        /* Unrecognised ICMP type — drop silently. */
        break;
    }
}

bool icmp_ping(uint32_t dst_ip, uint16_t id, uint16_t seq,
               uint32_t timeout_ms, uint64_t *rtt_ms_out) {
    /* Default 56-byte payload (`ping` standard) gives a 64-byte ICMP
     * message — small enough for one cluster, large enough to exercise
     * the checksum properly. */
    uint8_t payload[56];
    for (int i = 0; i < 56; i++) payload[i] = (uint8_t)i;

    pending.waiting = true;
    pending.src_ip  = dst_ip;
    pending.id      = id;
    pending.seq     = seq;
    pending.replied = false;

    uint64_t t0 = timer_ms();
    icmp_send(dst_ip, ICMP_TYPE_ECHO_REQUEST, id, seq, payload, sizeof(payload));

    uint64_t deadline = t0 + timeout_ms;
    while (timer_ms() < deadline) {
        if (pending.replied) {
            if (rtt_ms_out) *rtt_ms_out = timer_ms() - t0;
            pending.waiting = false;
            return true;
        }
    }
    pending.waiting = false;
    return false;
}

uint64_t icmp_rx_packets(void) { return rx_packets; }
uint64_t icmp_tx_packets(void) { return tx_packets; }
