#pragma once

#include <stdint.h>

/*
 * Internal libc DNS resolver — not exposed in any installed header.
 * getaddrinfo() calls this when the hostname isn't a literal IPv4.
 *
 * Returns 0 and stores the first A record IP (network byte order,
 * ready to drop into sockaddr_in.sin_addr.s_addr) in *ip_be on
 * success. Returns -1 on timeout, NXDOMAIN, or any parse failure.
 */
int dns_resolve_a(const char *hostname, uint32_t *ip_be);
