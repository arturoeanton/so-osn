#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * IPv4 ARP — Address Resolution Protocol (RFC 826).
 *
 * Keeps a tiny IP→MAC cache (ARP_CACHE_SIZE entries). Lookups are
 * cli/sti-guarded since updates come in from the NIC's IRQ handler.
 * Resolution is a synchronous busy-poll up to a timeout, suitable
 * for one-shot shell commands; long-running tasks should use the
 * lookup-only API and re-issue requests as needed.
 */

#define ARP_CACHE_SIZE  8

void arp_init(void);

/* Lookup `ip` in the cache. Returns true and fills `mac_out` if found. */
bool arp_lookup(uint32_t ip, uint8_t mac_out[6]);

/* Send an ARP who-has request for `ip`. Fire-and-forget. */
void arp_send_request(uint32_t ip);

/* Synchronous: send a request (if not already cached) and poll the
 * cache until populated or `timeout_ms` elapses. Returns true on hit.
 * Suitable for non-IRQ contexts only — relies on the timer being
 * armed and IRQs delivering the reply. */
bool arp_resolve(uint32_t ip, uint8_t mac_out[6], uint32_t timeout_ms);

/* Driver-RX entry point: process an incoming ARP packet (the 28-byte
 * payload of an Ethernet frame with ethertype 0x0806). Replies to
 * who-has-our-IP queries and populates the cache from replies. */
void arp_handle(const uint8_t *arp_payload, size_t len);

/* Render the cache as human-readable text for /sys/arp. */
void arp_dump(char *out, size_t out_size);
