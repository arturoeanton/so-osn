#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * UDP — RFC 768. 8-byte header (src port, dst port, length, checksum).
 * Checksum spans a pseudo-header (src ip, dst ip, zero, protocol,
 * udp length) + the actual UDP header + payload, computed with the
 * same Internet-checksum routine the IP layer uses.
 *
 * Checksum 0 on the wire is the conventional "skip verification"
 * marker — we generate real checksums but accept incoming packets
 * with cs=0 unverified, the way Linux does.
 */

#define UDP_HEADER_SIZE   8
#define UDP_MAX_PAYLOAD   1472          /* eth max - ip header - udp header */

bool udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
              const void *payload, size_t payload_len);

/* IRQ-context entry from ip_handle. */
void udp_handle(const uint8_t *data, size_t len,
                uint32_t src_ip, uint32_t dst_ip);

uint64_t udp_rx_packets(void);
uint64_t udp_tx_packets(void);
uint64_t udp_rx_drops(void);
