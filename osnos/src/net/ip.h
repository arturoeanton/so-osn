#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * IPv4 — RFC 791 minimal. 20-byte header (no options, IHL=5), no
 * fragmentation, TTL fixed at 64.
 *
 * Outgoing routing is trivial: if dst is inside our /24 subnet, ARP
 * the destination directly; otherwise ARP the gateway. The first
 * arp_resolve() call may block up to 500 ms while the cache fills.
 */

#define IP_HEADER_SIZE     20
#define IP_MAX_PAYLOAD     (1500 - IP_HEADER_SIZE)

#define IP_PROTO_ICMP      1
#define IP_PROTO_TCP       6
#define IP_PROTO_UDP       17

/* Build an IPv4 packet around `payload`, wrap it in Ethernet, and send.
 * Returns false on ARP timeout, payload too large, or TX-ring full. */
bool ip_send(uint32_t dst_ip, uint8_t protocol,
             const void *payload, size_t payload_len);

/* IRQ-context entry: validate version/IHL/checksum, dispatch by
 * protocol byte (1=ICMP, 6=TCP, 17=UDP). Silently drops anything
 * not addressed to us. */
void ip_handle(const uint8_t *data, size_t len);

/* RFC 1071 Internet checksum over `data[len]`. Returns the 16-bit
 * value ready to be stored in network byte order at the checksum
 * field. Verifying side computes over the full header (including the
 * checksum field) and expects the function to return 0. */
uint16_t ip_checksum(const uint8_t *data, size_t len);

/* Counters for /sys/net. */
uint64_t ip_rx_packets(void);
uint64_t ip_tx_packets(void);
uint64_t ip_rx_drops(void);
