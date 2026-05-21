#pragma once

#include <stdint.h>

/*
 * OSnOS IPC ABI — shared between the kernel and ring-3 servers
 * ============================================================
 *
 * This header is the **boundary** between kernel internals and user
 * code. Any type or constant here is wire-visible and must not be
 * reordered or renumbered: ring-3 servers (FASE 10+) read and write
 * `ipc_msg_t` directly via SYS_IPC_SEND / SYS_IPC_RECV (added in
 * 10.1), and the kernel keeps the same layout for in-process IPC
 * between server tasks.
 *
 * What lives here:
 *   - `ipc_msg_t` — the over-the-wire message struct
 *   - `ipc_type_t` — opcode enum (numeric ranges grouped by domain)
 *   - `SERVER_*` IDs — well-known service identifiers
 *   - `IPC_DATA_SIZE` / `IPC_QUEUE_SIZE` — capacity constants
 *
 * What does NOT live here (kernel-internal, may change at will):
 *   - `ipc_send`, `ipc_recv`, `ipc_recv_block`, `ipc_init`,
 *     `ipc_pending` — function prototypes (see src/micro/ipc.h)
 *
 * Files terminating in `_abi.h` under src/include/ are the ABI
 * frontier. Changing one of these requires recompiling kernel + libc
 * + every ring-3 ELF, so the change should be backed by a matching
 * note in PLAN_FASE10.md and STATUS.md.
 */

#define IPC_DATA_SIZE  1024
#define IPC_QUEUE_SIZE 64

/*
 * Well-known service IDs. Servers self-register against these via
 * service_register (kernel) or SYS_SERVICE_REGISTER (ring 3, FASE
 * 10.1+). Numeric values are part of the ABI.
 */
#define SERVER_KEYBOARD 1
#define SERVER_SHELL    2
#define SERVER_CONSOLE  3
#define SERVER_FS       4

/*
 * Opcode ranges (numeric values are part of the ABI; do not reorder):
 *   0x00 - 0x0F   system
 *   0x10 - 0x1F   console
 *   0x20 - 0x3F   fs / vfs
 *   0x40 - 0x5F   process lifecycle
 */
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
    IPC_PROC_STOPPED     = 0x41,   /* Ctrl+Z hit a fg user task     */
    IPC_PROC_CONTINUED   = 0x42    /* `fg` / `bg` resumed it back   */
} ipc_type_t;

/*
 * Wire layout:
 *   from / to  -- service IDs of sender + intended receiver
 *   type       -- opcode (ipc_type_t)
 *   arg0, arg1 -- type-specific scalar arguments. By convention
 *                 responses encode arg0 = osnos_status_t (errno-like;
 *                 0 = OK) and arg1 = payload size when meaningful.
 *   data       -- inline payload, NUL-terminated when textual.
 */
typedef struct {
    uint64_t   from;
    uint64_t   to;
    ipc_type_t type;
    uint64_t   arg0;
    uint64_t   arg1;
    char       data[IPC_DATA_SIZE];
} ipc_msg_t;
