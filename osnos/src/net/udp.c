#include "udp.h"

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
static inline void wr16_be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

/*
 * UDP checksum is computed over:
 *   pseudo-header (12 B): src_ip, dst_ip, 0, proto=17, udp_length
 *   UDP header   (8 B):  ports, length, checksum-as-0
 *   payload (UDP length - 8 B)
 *
 * Easiest correct implementation: lay the bytes out contiguously in a
 * scratch buffer and call ip_checksum on it.
 */
static uint16_t udp_compute_checksum(uint32_t src_ip, uint32_t dst_ip,
                                       const uint8_t *udp_seg, size_t udp_len) {
    /* Pseudo header (12) + udp segment. */
    uint8_t buf[12 + UDP_HEADER_SIZE + UDP_MAX_PAYLOAD];
    if (udp_len > sizeof(buf) - 12) return 0;

    buf[0] = (uint8_t)(src_ip >> 24);
    buf[1] = (uint8_t)(src_ip >> 16);
    buf[2] = (uint8_t)(src_ip >> 8);
    buf[3] = (uint8_t)(src_ip);
    buf[4] = (uint8_t)(dst_ip >> 24);
    buf[5] = (uint8_t)(dst_ip >> 16);
    buf[6] = (uint8_t)(dst_ip >> 8);
    buf[7] = (uint8_t)(dst_ip);
    buf[8] = 0;
    buf[9] = 17;                                  /* IPPROTO_UDP */
    buf[10] = (uint8_t)(udp_len >> 8);
    buf[11] = (uint8_t)udp_len;
    for (size_t i = 0; i < udp_len; i++) buf[12 + i] = udp_seg[i];

    return ip_checksum(buf, 12 + udp_len);
}

bool udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
              const void *payload, size_t payload_len) {
    if (payload_len > UDP_MAX_PAYLOAD) return false;

    uint8_t seg[UDP_HEADER_SIZE + UDP_MAX_PAYLOAD];
    uint16_t udp_len = (uint16_t)(UDP_HEADER_SIZE + payload_len);

    wr16_be(seg + 0, src_port);
    wr16_be(seg + 2, dst_port);
    wr16_be(seg + 4, udp_len);
    wr16_be(seg + 6, 0);                          /* checksum placeholder */

    const uint8_t *p = (const uint8_t *)payload;
    for (size_t i = 0; i < payload_len; i++) seg[UDP_HEADER_SIZE + i] = p[i];

    uint16_t cs = udp_compute_checksum(net_local_ip(), dst_ip, seg, udp_len);
    /* Conventional 0-on-the-wire means "skip verification" — but only
     * the all-zero result of the calculation should appear that way.
     * Per RFC 768, transmit 0xFFFF instead of 0 when the calculated
     * checksum is zero. */
    if (cs == 0) cs = 0xFFFF;
    wr16_be(seg + 6, cs);

    if (!ip_send(dst_ip, IP_PROTO_UDP, seg, udp_len)) return false;
    tx_packets++;
    return true;
}

void udp_handle(const uint8_t *data, size_t len,
                uint32_t src_ip, uint32_t dst_ip) {
    if (len < UDP_HEADER_SIZE) { rx_drops++; return; }

    uint16_t src_port = rd16_be(data + 0);
    uint16_t dst_port = rd16_be(data + 2);
    uint16_t udp_len  = rd16_be(data + 4);
    uint16_t udp_cs   = rd16_be(data + 6);

    if (udp_len < UDP_HEADER_SIZE || udp_len > len) { rx_drops++; return; }

    /* Verify checksum (skip when sender used the all-zero opt-out). */
    if (udp_cs != 0) {
        if (udp_compute_checksum(src_ip, dst_ip, data, udp_len) != 0) {
            rx_drops++;
            return;
        }
    }

    rx_packets++;

    sock_deliver_udp(src_ip, src_port,
                      dst_ip, dst_port,
                      data + UDP_HEADER_SIZE,
                      (size_t)(udp_len - UDP_HEADER_SIZE));
}

uint64_t udp_rx_packets(void) { return rx_packets; }
uint64_t udp_tx_packets(void) { return tx_packets; }
uint64_t udp_rx_drops(void)   { return rx_drops;   }
