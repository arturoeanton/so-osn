#pragma once

#include <stdint.h>

#include "../include/osnos_ipc_abi.h"

/*
 * Service registry — maps a SERVER_* id to the pid currently
 * implementing it. SERVER_* constants live in osnos_ipc_abi.h so
 * ring-3 servers see the same numbers as the kernel.
 */

void service_register(uint64_t service_id, uint64_t pid);
uint64_t service_get_pid(uint64_t service_id);
