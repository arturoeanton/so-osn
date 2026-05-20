#include "tcp.h"

#include "eth.h"
#include "ip.h"
#include "socket.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static uint64_t rx_packets;
static uint64_t tx_packets;
static uint64_t rx_drops;

static inline uint16_t rd16_be(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline uint32_t rd32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static inline void wr16_be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static inline void wr32_be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* TCP checksum — same pseudo-header layout as UDP, only proto=6. */
static uint16_t tcp_compute_checksum(uint32_t src_ip, uint32_t dst_ip,
                                       const uint8_t *seg, size_t seg_len) {
    uint8_t buf[12 + TCP_HEADER_SIZE + 1500];
    if (seg_len > sizeof(buf) - 12) return 0;

    buf[0] = (uint8_t)(src_ip >> 24);
    buf[1] = (uint8_t)(src_ip >> 16);
    buf[2] = (uint8_t)(src_ip >> 8);
    buf[3] = (uint8_t)(src_ip);
    buf[4] = (uint8_t)(dst_ip >> 24);
    buf[5] = (uint8_t)(dst_ip >> 16);
    buf[6] = (uint8_t)(dst_ip >> 8);
    buf[7] = (uint8_t)(dst_ip);
    buf[8] = 0;
    buf[9] = 6;
    buf[10] = (uint8_t)(seg_len >> 8);
    buf[11] = (uint8_t)seg_len;
    for (size_t i = 0; i < seg_len; i++) buf[12 + i] = seg[i];
    return ip_checksum(buf, 12 + seg_len);
}

bool tcp_send(uint32_t dst_ip, uint16_t dst_port,
              uint16_t src_port, uint32_t seq, uint32_t ack,
              uint8_t flags, uint16_t window,
              const void *payload, size_t len) {
    if (len > 1500 - TCP_HEADER_SIZE) return false;

    uint8_t seg[TCP_HEADER_SIZE + 1500];

    wr16_be(seg + 0,  src_port);
    wr16_be(seg + 2,  dst_port);
    wr32_be(seg + 4,  seq);
    wr32_be(seg + 8,  ack);
    seg[12] = (uint8_t)(5u << 4);    /* data offset = 5 words = 20 bytes */
    seg[13] = flags;
    wr16_be(seg + 14, window);
    wr16_be(seg + 16, 0);            /* checksum placeholder */
    wr16_be(seg + 18, 0);            /* urgent ptr */

    const uint8_t *p = (const uint8_t *)payload;
    for (size_t i = 0; i < len; i++) seg[TCP_HEADER_SIZE + i] = p[i];

    uint16_t cs = tcp_compute_checksum(net_local_ip(), dst_ip,
                                         seg, TCP_HEADER_SIZE + len);
    wr16_be(seg + 16, cs);

    if (!ip_send(dst_ip, IP_PROTO_TCP, seg, TCP_HEADER_SIZE + len)) return false;
    tx_packets++;
    return true;
}

void tcp_handle(const uint8_t *data, size_t len,
                uint32_t src_ip, uint32_t dst_ip) {
    if (len < TCP_HEADER_SIZE) { rx_drops++; return; }

    /* Validate checksum: function sum over header+payload + pseudo-hdr
     * should yield 0 for a clean segment. */
    if (tcp_compute_checksum(src_ip, dst_ip, data, len) != 0) {
        rx_drops++;
        return;
    }

    uint16_t src_port = rd16_be(data + 0);
    uint16_t dst_port = rd16_be(data + 2);
    uint32_t seq      = rd32_be(data + 4);
    uint32_t ack      = rd32_be(data + 8);
    uint8_t  doff     = (uint8_t)(data[12] >> 4);
    uint8_t  flags    = data[13];
    /* window + urgent ptr ignored in 8.5.5a (no flow control yet). */

    size_t hlen = (size_t)doff * 4;
    if (hlen < TCP_HEADER_SIZE || hlen > len) { rx_drops++; return; }

    rx_packets++;

    sock_tcp_handle_segment(src_ip, src_port, dst_ip, dst_port,
                              seq, ack, flags);
}

uint64_t tcp_rx_packets(void) { return rx_packets; }
uint64_t tcp_tx_packets(void) { return tx_packets; }
uint64_t tcp_rx_drops(void)   { return rx_drops;   }
