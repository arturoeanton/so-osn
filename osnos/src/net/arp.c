#include "arp.h"

#include "../lib/string.h"
#include "../micro/timer.h"
#include "eth.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * On-wire ARP layout (28 bytes after the Ethernet header):
 *   0..1   htype       big-endian, 1 = Ethernet
 *   2..3   ptype       big-endian, 0x0800 = IPv4
 *   4      hlen        6
 *   5      plen        4
 *   6..7   oper        big-endian, 1 = request, 2 = reply
 *   8..13  sha         sender hardware addr (MAC)
 *   14..17 spa         sender protocol addr (IPv4)
 *   18..23 tha         target hardware addr
 *   24..27 tpa         target protocol addr
 */
#define ARP_HEADER_SIZE   28
#define ARP_HTYPE_ETH     1
#define ARP_PTYPE_IPV4    0x0800
#define ARP_OPER_REQUEST  1
#define ARP_OPER_REPLY    2

typedef struct {
    bool      valid;
    uint32_t  ip;          /* host byte order */
    uint8_t   mac[6];
    uint64_t  last_seen_ms;
} arp_entry_t;

static arp_entry_t cache[ARP_CACHE_SIZE];

static inline void cli(void) { __asm__ volatile ("cli"); }
static inline void sti(void) { __asm__ volatile ("sti"); }

static uint16_t rd16_be(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t rd32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static void wr16_be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static void wr32_be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

void arp_init(void) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) cache[i].valid = false;
}

bool arp_lookup(uint32_t ip, uint8_t mac_out[6]) {
    cli();
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (cache[i].valid && cache[i].ip == ip) {
            for (int k = 0; k < 6; k++) mac_out[k] = cache[i].mac[k];
            sti();
            return true;
        }
    }
    sti();
    return false;
}

/* Insert / refresh (ip, mac). LRU on full cache (oldest last_seen_ms). */
static void cache_put(uint32_t ip, const uint8_t mac[6]) {
    cli();
    int slot = -1;
    uint64_t oldest = (uint64_t)-1;
    int oldest_idx = 0;

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (cache[i].valid && cache[i].ip == ip) { slot = i; break; }
        if (!cache[i].valid)                     { if (slot < 0) slot = i; }
        if (cache[i].last_seen_ms < oldest)      { oldest = cache[i].last_seen_ms;
                                                    oldest_idx = i; }
    }
    if (slot < 0) slot = oldest_idx;

    cache[slot].valid = true;
    cache[slot].ip = ip;
    for (int k = 0; k < 6; k++) cache[slot].mac[k] = mac[k];
    cache[slot].last_seen_ms = timer_ms();
    sti();
}

void arp_send_request(uint32_t target_ip) {
    uint8_t pkt[ARP_HEADER_SIZE];
    const uint8_t *src_mac = net_local_mac();
    uint32_t       src_ip  = net_local_ip();

    wr16_be(pkt + 0,  ARP_HTYPE_ETH);
    wr16_be(pkt + 2,  ARP_PTYPE_IPV4);
    pkt[4] = 6;
    pkt[5] = 4;
    wr16_be(pkt + 6,  ARP_OPER_REQUEST);
    for (int i = 0; i < 6; i++) pkt[8  + i] = src_mac[i];
    wr32_be(pkt + 14, src_ip);
    for (int i = 0; i < 6; i++) pkt[18 + i] = 0;       /* tha unknown */
    wr32_be(pkt + 24, target_ip);

    eth_send(eth_broadcast_mac, ETHERTYPE_ARP, pkt, ARP_HEADER_SIZE);
}

static void arp_send_reply(const uint8_t target_mac[6], uint32_t target_ip) {
    uint8_t pkt[ARP_HEADER_SIZE];
    const uint8_t *src_mac = net_local_mac();
    uint32_t       src_ip  = net_local_ip();

    wr16_be(pkt + 0,  ARP_HTYPE_ETH);
    wr16_be(pkt + 2,  ARP_PTYPE_IPV4);
    pkt[4] = 6;
    pkt[5] = 4;
    wr16_be(pkt + 6,  ARP_OPER_REPLY);
    for (int i = 0; i < 6; i++) pkt[8  + i] = src_mac[i];
    wr32_be(pkt + 14, src_ip);
    for (int i = 0; i < 6; i++) pkt[18 + i] = target_mac[i];
    wr32_be(pkt + 24, target_ip);

    eth_send(target_mac, ETHERTYPE_ARP, pkt, ARP_HEADER_SIZE);
}

void arp_handle(const uint8_t *arp, size_t len) {
    if (len < ARP_HEADER_SIZE) return;
    if (rd16_be(arp + 0) != ARP_HTYPE_ETH)  return;
    if (rd16_be(arp + 2) != ARP_PTYPE_IPV4) return;
    if (arp[4] != 6) return;
    if (arp[5] != 4) return;

    uint16_t oper = rd16_be(arp + 6);
    const uint8_t *sha = arp + 8;
    uint32_t       spa = rd32_be(arp + 14);
    uint32_t       tpa = rd32_be(arp + 24);

    /* Always remember whoever just talked to us — they exist and we
     * already paid for the packet. */
    cache_put(spa, sha);

    if (oper == ARP_OPER_REQUEST && tpa == net_local_ip()) {
        arp_send_reply(sha, spa);
    }
    /* oper == ARP_OPER_REPLY → already populated cache above. */
}

bool arp_resolve(uint32_t ip, uint8_t mac_out[6], uint32_t timeout_ms) {
    if (arp_lookup(ip, mac_out)) return true;

    arp_send_request(ip);

    uint64_t deadline = timer_ms() + (uint64_t)timeout_ms;
    while (timer_ms() < deadline) {
        if (arp_lookup(ip, mac_out)) return true;
        /* Tight poll — IRQs deliver the reply asynchronously and
         * timer_ms ticks past us when the timer fires. */
    }
    return false;
}

/* ---------------------------------------------------------------- */
/* /sys/arp text dump                                               */
/* ---------------------------------------------------------------- */

static void emit_ip(uint32_t ip, char *out, size_t out_size) {
    char num[8];
    for (int i = 3; i >= 0; i--) {
        os_format_u64((ip >> (i * 8)) & 0xFF, num, sizeof(num));
        os_strlcat(out, num, out_size);
        if (i > 0) os_strlcat(out, ".", out_size);
    }
}

static char nib(uint8_t n) {
    n &= 0xF;
    return (char)(n < 10 ? '0' + n : 'a' + (n - 10));
}

void arp_dump(char *out, size_t out_size) {
    out[0] = 0;

    char ipbuf[32];
    ipbuf[0] = 0;
    emit_ip(net_local_ip(), ipbuf, sizeof(ipbuf));
    os_strlcat(out, "local:   ", out_size);
    os_strlcat(out, ipbuf, out_size);
    os_strlcat(out, "\ngateway: ", out_size);
    ipbuf[0] = 0;
    emit_ip(net_gateway_ip(), ipbuf, sizeof(ipbuf));
    os_strlcat(out, ipbuf, out_size);
    os_strlcat(out, "\n\nIP              MAC                age_ms\n", out_size);

    uint64_t now = timer_ms();
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!cache[i].valid) continue;
        char row[64];
        row[0] = 0;
        emit_ip(cache[i].ip, row, sizeof(row));
        /* Pad to width 16. */
        size_t pad = 16 - os_strlen(row);
        for (size_t k = 0; k < pad; k++) os_strlcat(row, " ", sizeof(row));

        char macstr[18];
        for (int m = 0; m < 6; m++) {
            macstr[m * 3 + 0] = nib((uint8_t)(cache[i].mac[m] >> 4));
            macstr[m * 3 + 1] = nib(cache[i].mac[m]);
            macstr[m * 3 + 2] = (m < 5) ? ':' : 0;
        }
        os_strlcat(row, macstr, sizeof(row));
        os_strlcat(row, "  ", sizeof(row));
        char num[24];
        os_format_u64(now - cache[i].last_seen_ms, num, sizeof(num));
        os_strlcat(row, num, sizeof(row));
        os_strlcat(row, "\n", sizeof(row));

        os_strlcat(out, row, out_size);
    }
}
