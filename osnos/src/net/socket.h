#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Kernel-side socket API. Lives below the syscall layer; 8.5.4a
 * exposes it directly through a shell command so we can validate the
 * UDP path before wiring it into the Linux socket() syscalls in 8.5.4b.
 *
 * Descriptor space is independent of the fd table for now — 8.5.4b
 * promotes these handles into the global fd table so that sys_recvfrom
 * etc. can route by integer like POSIX expects.
 */

#define SOCK_MAX            8
#define SOCK_RX_QUEUE_DEPTH 8
#define SOCK_RX_MAX_DGRAM   1024

/* Match Linux x86_64 SOCK_* numbering so user-mode `socket(2)` calls
 * pass these through verbatim. */
#define OSNOS_SOCK_STREAM   1   /* TCP — 8.5.5 */
#define OSNOS_SOCK_DGRAM    2   /* UDP */

/* Returns >=0 socket descriptor, -1 on full. */
int  sock_create(int type);

/* Bind a UDP socket to local (ip, port). ip=0 means INADDR_ANY (accept
 * any of our local IPs — only one today). port=0 picks an ephemeral
 * port. Returns 0 on success, -1 on bad sd / port in use. */
int  sock_bind(int sd, uint32_t ip, uint16_t port);

int  sock_close(int sd);

/*
 * Synchronous receive. Blocks (busy-poll on the cooperative scheduler)
 * until a datagram lands or `timeout_ms` elapses. Returns bytes copied
 * into buf on success, 0 on timeout, -1 on error. `src_ip` / `src_port`
 * receive the peer's address when non-NULL.
 */
int  sock_recvfrom(int sd, void *buf, size_t buf_len,
                    uint32_t *src_ip, uint16_t *src_port,
                    uint32_t timeout_ms);

/* Send a UDP datagram. Returns bytes sent or -1. */
int  sock_sendto(int sd, const void *buf, size_t len,
                  uint32_t dst_ip, uint16_t dst_port);

/* ----- internal (called from UDP RX path) ----- */

/* Find the socket bound to local_port (and local_ip matching ANY or
 * our IP) and enqueue an incoming datagram. Returns true if delivered.
 * Called from IRQ context — must be brief and tolerate re-entry. */
bool sock_deliver_udp(uint32_t src_ip, uint16_t src_port,
                       uint32_t dst_ip, uint16_t dst_port,
                       const uint8_t *data, size_t len);

/* Local port → 0 if sd is invalid. Used by udp_send to fill src_port. */
uint16_t sock_local_port(int sd);
