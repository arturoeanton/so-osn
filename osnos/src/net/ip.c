#include "ip.h"

#include "arp.h"
#include "eth.h"
#include "icmp.h"
#include "socket.h"
#include "tcp.h"
#include "udp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static uint64_t rx_packets;
static uint64_t tx_packets;
static uint64_t rx_drops;
static uint16_t next_ip_id = 1;

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

uint16_t ip_checksum(const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        sum += (uint32_t)(data[i] << 8) | data[i + 1];
    }
    if (len & 1) sum += (uint32_t)(data[len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

bool ip_send(uint32_t dst_ip, uint8_t protocol,
             const void *payload, size_t payload_len) {
    if (payload_len > IP_MAX_PAYLOAD) return false;

    /*
     * Routing: same subnet → ARP dst directly; otherwise hop through
     * the gateway. Loopback to ourselves takes the local-subnet path
     * and resolves to our own MAC — fine, the chip drops the frame
     * silently. (Proper loopback would short-circuit the driver.)
     */
    uint32_t local_ip = net_local_ip();
    uint32_t mask     = net_local_netmask();
    uint32_t next_hop = ((dst_ip & mask) == (local_ip & mask))
                            ? dst_ip
                            : net_gateway_ip();

    uint8_t dst_mac[6];
    if (!arp_resolve(next_hop, dst_mac, 500)) return false;

    uint16_t total_len = (uint16_t)(IP_HEADER_SIZE + payload_len);
    uint8_t  buf[IP_HEADER_SIZE + IP_MAX_PAYLOAD];

    buf[0] = 0x45;                 /* version=4, IHL=5 */
    buf[1] = 0;                    /* TOS */
    wr16_be(buf + 2,  total_len);
    wr16_be(buf + 4,  next_ip_id++);
    wr16_be(buf + 6,  0);          /* no flags, no fragment */
    buf[8] = 64;                   /* TTL */
    buf[9] = protocol;
    wr16_be(buf + 10, 0);          /* checksum placeholder */
    wr32_be(buf + 12, local_ip);
    wr32_be(buf + 16, dst_ip);

    /* Compute checksum AFTER all fields are set (except checksum=0). */
    uint16_t cs = ip_checksum(buf, IP_HEADER_SIZE);
    wr16_be(buf + 10, cs);

    const uint8_t *p = (const uint8_t *)payload;
    for (size_t i = 0; i < payload_len; i++) buf[IP_HEADER_SIZE + i] = p[i];

    if (!eth_send(dst_mac, ETHERTYPE_IPV4, buf, total_len)) return false;
    tx_packets++;
    return true;
}

void ip_handle(const uint8_t *data, size_t len) {
    if (len < IP_HEADER_SIZE) { rx_drops++; return; }

    uint8_t ver_ihl = data[0];
    if ((ver_ihl >> 4) != 4)  { rx_drops++; return; }

    size_t hl = (size_t)(ver_ihl & 0xF) * 4;
    if (hl < IP_HEADER_SIZE || hl > len) { rx_drops++; return; }

    uint16_t total_len = rd16_be(data + 2);
    if (total_len > len || total_len < hl) { rx_drops++; return; }

    /* Checksum: over the entire header (including its own field).
     * For a clean packet the function returns 0. */
    if (ip_checksum(data, hl) != 0) { rx_drops++; return; }

    uint32_t dst_ip = rd32_be(data + 16);
    /* Accept unicast to us OR limited broadcast. */
    if (dst_ip != net_local_ip() && dst_ip != 0xFFFFFFFF) return;

    rx_packets++;

    uint8_t  protocol = data[9];
    uint32_t src_ip   = rd32_be(data + 12);
    const uint8_t *payload = data + hl;
    size_t   payload_len = (size_t)(total_len - hl);

    switch (protocol) {
    case IP_PROTO_ICMP:
        icmp_handle(payload, payload_len, src_ip);
        break;
    case IP_PROTO_UDP:
        udp_handle(payload, payload_len, src_ip, dst_ip);
        break;
    case IP_PROTO_TCP:
        tcp_handle(payload, payload_len, src_ip, dst_ip);
        break;
    default:
        break;
    }

    /* Mirror el paquete IPv4 entero (header + payload) a cualquier raw
     * socket cuyo protocol matchea. Linux entrega *con* IP header.
     * Para ICMP, esto se hace además del icmp_handle estándar — ping
     * sigue funcionando aunque haya otros listeners. */
    sock_raw_deliver(protocol, data, (size_t)total_len, src_ip);
}

uint64_t ip_rx_packets(void) { return rx_packets; }
uint64_t ip_tx_packets(void) { return tx_packets; }
uint64_t ip_rx_drops(void)   { return rx_drops;   }
