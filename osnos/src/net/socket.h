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

#define SOCK_MAX                8
#define SOCK_RX_QUEUE_DEPTH     8
#define SOCK_RX_MAX_DGRAM       1024
#define SOCK_TCP_RX_BUF         4096   /* byte ring per TCP socket */
#define SOCK_ACCEPT_QUEUE_DEPTH 4      /* pending ESTABLISHED children per LISTEN */
#define TCP_MSS                 1400   /* IP+TCP+ETH safe payload */
#define TCP_RTO_MS              500    /* retransmission timeout */
#define TCP_MAX_RETX            5      /* give up after this many resends */

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

/* ----- TCP (8.5.5a handshake + 8.5.5b data/close) ----- */

/* Mark a SOCK_STREAM socket as LISTEN. `backlog` caps the accept queue
 * (silently clamped to SOCK_ACCEPT_QUEUE_DEPTH). */
int  sock_listen(int sd, int backlog);

/*
 * Pull the next ESTABLISHED child off a LISTEN socket's accept queue.
 * Returns the new socket descriptor (>=0) and fills peer_ip / peer_port
 * with the remote endpoint. Blocks up to `timeout_ms`. -2 on timeout.
 * -1 on bad sd / not in LISTEN state.
 */
int  sock_accept(int listen_sd,
                  uint32_t *peer_ip, uint16_t *peer_port,
                  uint32_t timeout_ms);

/* (Legacy 8.5.5a helper — drains the accept queue without consuming
 * a slot. Kept for `tcptest` introspection; new code uses sock_accept.) */
bool sock_tcp_get_peer(int sd, uint32_t *ip_out, uint16_t *port_out);

/* RST the connection (immediate teardown). Returns the socket to
 * LISTEN if it was bound, else CLOSED. */
void sock_tcp_reset(int sd);

/*
 * Active open. Auto-binds to an ephemeral port if not yet bound,
 * sends SYN, polls until ESTABLISHED or `timeout_ms` elapses. Returns
 * 0 on success, -1 on error / timeout / RST.
 */
int  sock_connect(int sd, uint32_t dst_ip, uint16_t dst_port,
                   uint32_t timeout_ms);

/*
 * Stream recv. Waits up to `timeout_ms` for at least 1 byte to be
 * available. Returns:
 *    n > 0  — bytes copied
 *    0      — peer closed (FIN seen and buffer drained) → EOF
 *   -1      — not connected / error
 *   -2      — timeout with no data and connection still open
 */
int  sock_recv(int sd, void *buf, size_t buf_len, uint32_t timeout_ms);

/* Stream send. Splits into MSS-sized segments. Returns total bytes
 * queued (== len on success, or -1 on error). */
int  sock_send(int sd, const void *buf, size_t len);

/*
 * Graceful close. Sends a FIN if the connection is open, transitions
 * the state machine, and lets the FIN/ACK exchange complete in the
 * background. The slot is marked "zombie" and reclaimed when the
 * state reaches CLOSED.
 */
int  sock_close_tcp(int sd);

/* ----- internal (called from UDP / TCP RX path) ----- */

/* Find the socket bound to local_port (and local_ip matching ANY or
 * our IP) and enqueue an incoming datagram. Returns true if delivered.
 * Called from IRQ context — must be brief and tolerate re-entry. */
bool sock_deliver_udp(uint32_t src_ip, uint16_t src_port,
                       uint32_t dst_ip, uint16_t dst_port,
                       const uint8_t *data, size_t len);

/* IRQ-context entry from tcp_handle. Drives the per-socket state
 * machine. `payload` / `payload_len` carry any data bytes after the
 * TCP header. */
void sock_tcp_handle_segment(uint32_t src_ip, uint16_t src_port,
                               uint32_t dst_ip, uint16_t dst_port,
                               uint32_t seq, uint32_t ack, uint8_t flags,
                               const uint8_t *payload, size_t payload_len);

/* Local port → 0 if sd is invalid. Used by udp_send to fill src_port. */
uint16_t sock_local_port(int sd);

/* ---- Diagnostics / test introspection ---- */

uint64_t sock_tcp_retx_total(void);   /* total retransmits across all sockets */
uint64_t sock_tcp_retx_drops(void);   /* connections RST'd after TCP_MAX_RETX */

/* Diagnostics: which path freed slots. Useful for the httpd-multi-curl
 * investigation. */
uint64_t sock_free_udp_close(void);
uint64_t sock_free_tcp_reset_zombie(void);
uint64_t sock_free_finwait1_zombie(void);
uint64_t sock_free_finwait2_zombie(void);
uint64_t sock_free_lastack_zombie(void);
uint64_t sock_free_closing_zombie(void);
uint64_t sock_free_close_listen(void);
uint64_t sock_free_tick_maxretx(void);

int sock_last_send_fail_sd    (void);
int sock_last_send_fail_used  (void);
int sock_last_send_fail_type  (void);
int sock_last_send_fail_state (void);
int sock_last_send_fail_parent(void);

/* Returns -1 on invalid sd. */
int      sock_tcp_state_int(int sd);  /* tcp_state_t cast to int */
int      sock_tcp_retx_len (int sd);  /* current pending retx buffer length */
int      sock_tcp_retx_count(int sd); /* attempts on current segment */
uint16_t sock_tcp_get_local_port(int sd);

/*
 * Timer hook: scan TCP sockets, retransmit any segment whose send time
 * is more than TCP_RTO_MS ms before `now_ms`. Called from timer_handle
 * at 100 Hz. RST + close the connection once TCP_MAX_RETX attempts are
 * exhausted on a single segment.
 */
void     sock_tick(uint64_t now_ms);

/*
 * Readability poll used by sys_select. Returns true when a non-blocking
 * recv on this socket would make progress:
 *   - DGRAM: a datagram is queued.
 *   - STREAM LISTEN: an accept-ready child sits in the backlog.
 *   - STREAM ESTABLISHED / CLOSE_WAIT / FIN_WAIT_*: rx buffer has bytes
 *     OR the peer has already sent FIN (recv would return 0 = EOF).
 *   - Other: false.
 */
bool sock_readable(int sd);
