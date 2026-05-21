#pragma once

/*
 * Ring-3 facing wrapper for the OSnOS IPC ABI. Re-exposes the
 * kernel/userland shared types so user ELFs (the future ring-3
 * servers under elfs/osn-server/) can build ipc_msg_t values and
 * hand them to SYS_IPC_SEND once that syscall lands in FASE 10.1.
 *
 * The actual definitions live in src/include/osnos_ipc_abi.h —
 * a single source of truth for both sides. The user build adds
 * `-I src/include` so this include resolves cleanly.
 *
 * Syscall helpers (sys_ipc_send, sys_ipc_recv, sys_service_register,
 * etc.) will be added here when the corresponding syscalls land
 * (FASE 10.1+, see PLAN_FASE10.md).
 */

#include "osnos_ipc_abi.h"
