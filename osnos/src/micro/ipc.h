#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "../include/osnos_status.h"
#include "service.h"

/*
 * OSnOS IPC contract
 * ==================
 *
 * Messages are fixed-size structures copied at send time into a kernel-resident
 * queue. Senders may reuse / discard their buffer immediately after `ipc_send`
 * returns; receivers obtain their own copy via `ipc_recv` / `ipc_recv_block`.
 *
 * Wire layout per message:
 *   from / to  -- well-known service IDs (see service.h)
 *   type       -- opcode (ipc_type_t)
 *   arg0, arg1 -- type-specific scalar arguments
 *   data       -- type-specific payload, up to IPC_DATA_SIZE bytes
 *
 * Opcode ranges (explicit values are part of the IPC ABI; do not reorder):
 *   0x00 - 0x0F   system
 *   0x10 - 0x1F   console
 *   0x20 - 0x3F   fs / vfs
 *   0x40 - 0x5F   reserved (proc, net, ...)
 *
 * Response convention:
 *   For every request that yields a reply (currently IPC_FS_RESPONSE),
 *     arg0 = osnos_status_t (0 = OK, >0 = errno-like)
 *     arg1 = payload size in bytes when meaningful (e.g. IPC_FS_READ)
 *     data = optional textual or binary payload, null-terminated when textual
 *
 * Blocking model:
 *   ipc_recv         non-blocking poll; returns false if no matching message
 *   ipc_recv_block   marks the caller task BLOCKED if no message is ready;
 *                    the task is unblocked when a matching message is sent
 *
 * Queue:
 *   Fixed depth IPC_QUEUE_SIZE. `ipc_send` returns:
 *     OSNOS_OK      message enqueued
 *     OSNOS_EAGAIN  queue is full; caller may retry later
 *     OSNOS_ESRCH   target service is not registered
 *   Callers must check the return — silent failure stalls the requester.
 */

#define IPC_DATA_SIZE 1024
#define IPC_QUEUE_SIZE 64

typedef enum {
    IPC_NONE             = 0x00,
    IPC_KEY_EVENT        = 0x01,
    IPC_COMMAND_RUN      = 0x02,

    IPC_CONSOLE_WRITE    = 0x10,
    IPC_CONSOLE_CLEAR    = 0x11,

    IPC_FS_RESPONSE      = 0x20,
    IPC_FS_LIST          = 0x21,
    IPC_FS_READ          = 0x22,
    IPC_FS_WRITE         = 0x23,
    IPC_FS_APPEND        = 0x24,
    IPC_FS_TOUCH         = 0x25,
    IPC_FS_DELETE        = 0x26,
    IPC_FS_MKDIR         = 0x27,
    IPC_FS_RMDIR         = 0x28,
    IPC_FS_TREE          = 0x29,
    IPC_FS_COPY          = 0x2a,
    IPC_FS_MOVE          = 0x2b,

    IPC_PROC_EXITED      = 0x40,
    IPC_PROC_STOPPED     = 0x41,   /* Ctrl+Z hit a fg user task */
    IPC_PROC_CONTINUED   = 0x42    /* `fg` / `bg` resumed a stopped task */
} ipc_type_t;

typedef struct {
    uint64_t from;
    uint64_t to;
    ipc_type_t type;
    uint64_t arg0;
    uint64_t arg1;
    char data[IPC_DATA_SIZE];
} ipc_msg_t;

void ipc_init(void);

osnos_status_t ipc_send(const ipc_msg_t *msg);

/* Number of messages currently waiting in the shared queue. For sysfs. */
size_t ipc_pending(void);

bool ipc_recv(uint64_t to, ipc_msg_t *out);

bool ipc_recv_block(
    uint64_t to,
    ipc_msg_t *out
);
