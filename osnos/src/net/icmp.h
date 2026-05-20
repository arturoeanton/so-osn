#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ICMP — minimal echo (type 8 request / type 0 reply) support per
 * RFC 792. Enough for `ping` to work end-to-end against the QEMU
 * slirp gateway / DNS host.
 */

#define ICMP_HEADER_SIZE   8
#define ICMP_TYPE_ECHO_REPLY    0
#define ICMP_TYPE_ECHO_REQUEST  8

/*
 * Synchronous-ish echo: build a request with the given id/seq, send
 * it, and busy-poll for the matching reply up to `timeout_ms`.
 * Returns the RTT in milliseconds on success, 0 on timeout / error.
 */
bool icmp_ping(uint32_t dst_ip, uint16_t id, uint16_t seq,
               uint32_t timeout_ms, uint64_t *rtt_ms_out);

/* IRQ-context entry from ip_handle: parses an ICMP message. Echoes
 * replies to incoming requests; signals waiting pingers when their
 * reply arrives. */
void icmp_handle(const uint8_t *data, size_t len, uint32_t src_ip);

/* Counters for /sys/net. */
uint64_t icmp_rx_packets(void);
uint64_t icmp_tx_packets(void);
