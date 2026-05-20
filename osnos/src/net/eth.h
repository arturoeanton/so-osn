#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Ethernet II framing + protocol dispatch.
 *
 * Frames have a 14-byte fixed header:
 *   bytes 0..5   dst MAC
 *   bytes 6..11  src MAC
 *   bytes 12..13 ethertype (big-endian)
 *   bytes 14...  payload
 *
 * Minimum on-wire frame is 60 bytes (+4 CRC the NIC appends). The
 * RTL8139 pads short frames on TX, so callers don't need to round up.
 */

#define ETH_HEADER_SIZE   14
#define ETH_ADDR_LEN      6
#define ETH_MIN_FRAME     60
#define ETH_MAX_PAYLOAD   1500

#define ETHERTYPE_IPV4    0x0800
#define ETHERTYPE_ARP     0x0806

/* Broadcast MAC FF:FF:FF:FF:FF:FF — destination for ARP requests. */
extern const uint8_t eth_broadcast_mac[ETH_ADDR_LEN];

/*
 * net_init: register our RX hook with the driver and clear the ARP
 * cache. Called once at boot, after rtl8139_init. Idempotent — fails
 * silently if no NIC was detected.
 */
void net_init(void);

/* IRQ-context entry point: parse the Ethernet header and dispatch by
 * ethertype to the protocol layer. */
void net_rx(const uint8_t *frame, size_t len);

/* Wrap `payload` in an Ethernet header and hand to the driver. Returns
 * false if the NIC is absent, payload is oversized, or every TX slot
 * is busy. */
bool eth_send(const uint8_t dst_mac[ETH_ADDR_LEN],
              uint16_t ethertype,
              const void *payload, size_t payload_len);

/* Static config installed by net_init. */
uint32_t       net_local_ip(void);
uint32_t       net_local_netmask(void);
uint32_t       net_gateway_ip(void);
const uint8_t *net_local_mac(void);
