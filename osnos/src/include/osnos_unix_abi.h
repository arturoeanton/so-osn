#pragma once

#include <stdint.h>

/*
 * osnos_unix_abi.h — kernel↔userland wire contract para AF_UNIX
 * (PF_LOCAL) sockets. Valores match Linux x86_64.
 *
 * Pathname-based bind: `/tmp/sock.X`. Sin abstract namespace
 * (Linux extension donde sun_path[0] == '\0') todavía.
 */

#define OSNOS_AF_UNIX  1
#define OSNOS_AF_LOCAL 1   /* alias */

#define OSNOS_UNIX_PATH_MAX 108

/* Mirror exacto de struct sockaddr_un Linux:
 *   sun_family  uint16_t = AF_UNIX
 *   sun_path    char[108] (pathname, NUL-terminated unless full).
 * Total size = 110, generalmente caller pasa addrlen para acortar. */
typedef struct osnos_sockaddr_un {
    uint16_t sun_family;
    char     sun_path[OSNOS_UNIX_PATH_MAX];
} osnos_sockaddr_un_t;
