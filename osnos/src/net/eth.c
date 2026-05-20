#include "eth.h"

#include "../drivers/rtl8139.h"
#include "arp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

const uint8_t eth_broadcast_mac[ETH_ADDR_LEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

/*
 * Static IP configuration. QEMU slirp hands out 10.0.2.15 by default
 * (gateway 10.0.2.2, DNS 10.0.2.3) and these match what `-netdev user`
 * expects to NAT for. When DHCP arrives we'll populate these
 * dynamically at boot.
 */
#define LOCAL_IP      ((10u  << 24) | (0u << 16) | (2u << 8) | 15u)
#define LOCAL_NETMASK ((255u << 24) | (255u << 16) | (255u << 8) | 0u)
#define GATEWAY_IP    ((10u  << 24) | (0u << 16) | (2u << 8) | 2u)

uint32_t       net_local_ip      (void) { return LOCAL_IP; }
uint32_t       net_local_netmask (void) { return LOCAL_NETMASK; }
uint32_t       net_gateway_ip    (void) { return GATEWAY_IP; }
const uint8_t *net_local_mac     (void) { return rtl8139_mac(); }

void net_rx(const uint8_t *frame, size_t len) {
    if (len < ETH_HEADER_SIZE) return;

    uint16_t ethertype = (uint16_t)((frame[12] << 8) | frame[13]);
    const uint8_t *payload = frame + ETH_HEADER_SIZE;
    size_t plen = len - ETH_HEADER_SIZE;

    switch (ethertype) {
    case ETHERTYPE_ARP:
        arp_handle(payload, plen);
        break;
    /* ETHERTYPE_IPV4 lands here in 8.5.3 */
    default:
        /* Unknown ethertype — silently drop. */
        break;
    }
}

bool eth_send(const uint8_t dst_mac[ETH_ADDR_LEN],
              uint16_t ethertype,
              const void *payload, size_t payload_len) {
    if (payload_len > ETH_MAX_PAYLOAD) return false;

    /* Frame on the wire: header + payload, padded to 60 bytes minimum.
     * The RTL8139 pads short frames itself, so we only need to copy
     * what we actually have. */
    uint8_t frame[ETH_HEADER_SIZE + ETH_MAX_PAYLOAD];
    const uint8_t *src_mac = rtl8139_mac();

    for (int i = 0; i < ETH_ADDR_LEN; i++) frame[i] = dst_mac[i];
    for (int i = 0; i < ETH_ADDR_LEN; i++) frame[ETH_ADDR_LEN + i] = src_mac[i];
    frame[12] = (uint8_t)(ethertype >> 8);
    frame[13] = (uint8_t)(ethertype & 0xFF);

    const uint8_t *p = (const uint8_t *)payload;
    for (size_t i = 0; i < payload_len; i++) {
        frame[ETH_HEADER_SIZE + i] = p[i];
    }

    return rtl8139_tx(frame, ETH_HEADER_SIZE + payload_len);
}

void net_init(void) {
    if (!rtl8139_present()) return;
    arp_init();
    rtl8139_set_rx_callback(net_rx);
}
