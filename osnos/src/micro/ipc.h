#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "../include/osnos_ipc_abi.h"
#include "../include/osnos_status.h"
#include "service.h"

/*
 * OSnOS IPC — kernel-side helpers
 * ===============================
 *
 * The wire types (`ipc_msg_t`, `ipc_type_t`, `SERVER_*`, sizes) live
 * in <osnos_ipc_abi.h> so both kernel and ring-3 servers see exactly
 * the same definitions. This header only declares the kernel-internal
 * dispatch functions; ring-3 callers reach the same queue via the
 * SYS_IPC_SEND / SYS_IPC_RECV syscalls (FASE 10.1+).
 *
 * Messages are fixed-size structures copied at send time into a
 * kernel-resident queue. Senders may reuse / discard their buffer
 * immediately after `ipc_send` returns; receivers obtain their own
 * copy via `ipc_recv` / `ipc_recv_block`.
 *
 * Response convention:
 *   For every request that yields a reply (e.g. IPC_FS_RESPONSE):
 *     arg0 = osnos_status_t (0 = OK, >0 = errno-like)
 *     arg1 = payload size in bytes when meaningful
 *     data = optional textual or binary payload, null-terminated
 *            when textual
 *
 * Blocking model:
 *   ipc_recv         non-blocking poll; returns false if no matching
 *                    message
 *   ipc_recv_block   marks the caller task BLOCKED if no message is
 *                    ready; the task is unblocked when a matching
 *                    message is sent
 *
 * Queue:
 *   Fixed depth IPC_QUEUE_SIZE. `ipc_send` returns:
 *     OSNOS_OK      message enqueued
 *     OSNOS_EAGAIN  queue is full; caller may retry later
 *     OSNOS_ESRCH   target service is not registered
 *   Callers must check the return — silent failure stalls the
 *   requester.
 */

void ipc_init(void);

osnos_status_t ipc_send(const ipc_msg_t *msg);

/* Number of messages currently waiting in the shared queue. For sysfs. */
size_t ipc_pending(void);

/* True if any queued message is addressed to `pid`. O(queue) peek — used
 * by sys_poll's POLL_IPC_PENDING bit so a task can block waiting for
 * IPC alongside file-descriptor events. */
bool ipc_has_for_pid(uint64_t pid);

/* Compact the queue, dropping every message addressed to `pid`. Called
 * from proc_exit_current_user so a dying task doesn't leave stale
 * messages stuck in the 64-slot queue forever. Without this, the queue
 * saturates after a handful of app open/close cycles and ipc_send
 * starts returning EAGAIN — events never reach the compositor. */
void ipc_drop_for_pid(uint64_t pid);

bool ipc_recv(uint64_t to, ipc_msg_t *out);

bool ipc_recv_block(
    uint64_t to,
    ipc_msg_t *out
);
