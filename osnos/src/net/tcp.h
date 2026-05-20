#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * TCP — RFC 793. 8.5.5a covers the passive handshake only:
 *
 *   LISTEN → (recv SYN) → SYN_RCVD → (recv ACK) → ESTABLISHED
 *
 * Data transfer + graceful close land in 8.5.5b. For now an
 * ESTABLISHED socket that sees a FIN or any data response with RST.
 *
 * No options support — every segment we emit has a fixed 20-byte
 * header (data offset 5). Incoming options past byte 20 are skipped
 * by `data_offset * 4`.
 */

#define TCP_HEADER_SIZE   20

/* Flags byte (bits 7..0). */
#define TCP_FLAG_FIN      0x01
#define TCP_FLAG_SYN      0x02
#define TCP_FLAG_RST      0x04
#define TCP_FLAG_PSH      0x08
#define TCP_FLAG_ACK      0x10
#define TCP_FLAG_URG      0x20

typedef enum {
    TCP_CLOSED      = 0,
    TCP_LISTEN      = 1,
    TCP_SYN_SENT    = 2,
    TCP_SYN_RCVD    = 3,
    TCP_ESTABLISHED = 4,
    TCP_FIN_WAIT_1  = 5,
    TCP_FIN_WAIT_2  = 6,
    TCP_CLOSE_WAIT  = 7,
    TCP_CLOSING     = 8,
    TCP_LAST_ACK    = 9,
    TCP_TIME_WAIT   = 10
} tcp_state_t;

/* Build + send one segment. payload may be NULL/0. */
bool tcp_send(uint32_t dst_ip, uint16_t dst_port,
              uint16_t src_port, uint32_t seq, uint32_t ack,
              uint8_t flags, uint16_t window,
              const void *payload, size_t len);

/* IRQ-context entry from ip_handle. */
void tcp_handle(const uint8_t *data, size_t len,
                uint32_t src_ip, uint32_t dst_ip);

uint64_t tcp_rx_packets(void);
uint64_t tcp_tx_packets(void);
uint64_t tcp_rx_drops(void);
