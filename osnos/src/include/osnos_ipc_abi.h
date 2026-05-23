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
#define SERVER_OX       5   /* FASE 12 — mini-X window system        */

/*
 * Opcode ranges (numeric values are part of the ABI; do not reorder):
 *   0x00 - 0x0F   system
 *   0x10 - 0x1F   console
 *   0x20 - 0x3F   fs / vfs
 *   0x40 - 0x5F   process lifecycle
 *   0x60 - 0x7F   ox window system (FASE 12)
 */
typedef enum {
    IPC_NONE             = 0x00,
    IPC_KEY_EVENT        = 0x01,
    IPC_COMMAND_RUN      = 0x02,
    IPC_KEYBOARD_SUSPEND = 0x03,   /* GUI grabs the keyboard          */
    IPC_KEYBOARD_RESUME  = 0x04,   /* GUI gone — TTY/shell again      */

    IPC_CONSOLE_WRITE    = 0x10,
    IPC_CONSOLE_CLEAR    = 0x11,
    IPC_CONSOLE_SUSPEND  = 0x12,   /* GUI compositor takes the FB     */
    IPC_CONSOLE_RESUME   = 0x13,   /* GUI gone — shell text again     */

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
    IPC_PROC_CONTINUED   = 0x42,   /* `fg` / `bg` resumed it back   */

    /*
     * Ox window system (FASE 12). Client→Server (C→S) and
     * Server→Client (S→C) messages share the same numeric space.
     *
     * Wire layout for OX requests:
     *   arg0 = win_id (or 0 for global)
     *   arg1 = command-specific scalar
     *   data = command-specific blob (text, BGRA tile, etc.)
     *
     * Wire layout for OX events (S→C):
     *   arg0 = win_id (event target)
     *   arg1 = packed event scalars (ascii<<16 | keycode for KEY,
     *          x<<16|y for MOUSE move, etc.)
     *   data = empty
     *
     * Wire layout for OX_RESPONSE (S→C reply):
     *   arg0 = status (osnos_status_t, 0=OK)
     *   arg1 = returned value (win_id on CREATE, etc.)
     */
    IPC_OX_CONNECT        = 0x60,  /* C→S: client identifies itself        */
    IPC_OX_WINDOW_CREATE  = 0x61,  /* C→S: arg0=w<<16|h, data=title        */
    IPC_OX_WINDOW_DESTROY = 0x62,  /* C→S: arg0=win_id                     */
    IPC_OX_DRAW_RECT      = 0x63,  /* C→S: arg0=win_id arg1=rgba           */
                                   /* data = packed { x,y,w,h } uint32 ×4  */
    IPC_OX_DRAW_TEXT      = 0x64,  /* C→S: arg0=win_id arg1=rgba           */
                                   /* data = packed { x,y } uint32 ×2 + str */
    IPC_OX_DRAW_IMAGE     = 0x65,  /* C→S: arg0=win_id                     */
                                   /* data = { x,y,w,h, BGRA tile <=120px } */
    IPC_OX_PRESENT        = 0x66,  /* C→S: arg0=win_id                     */
    IPC_OX_SET_TITLE      = 0x67,  /* C→S: arg0=win_id, data=title         */
    IPC_OX_EVENT_KEY      = 0x68,  /* S→C: arg0=win_id, data[0]=ascii      */
                                   /*      data[1..2]=keycode LE, [3]=mods */
    IPC_OX_EVENT_MOUSE    = 0x69,  /* S→C: arg0=win_id, arg1=x<<32|y       */
                                   /*      data[0]=buttons, data[1]=type   */
    IPC_OX_EVENT_EXPOSE   = 0x6a,  /* S→C: arg0=win_id, arg1=x<<32|y       */
                                   /*      data=packed{w,h}                */
    IPC_OX_EVENT_CLOSE    = 0x6b,  /* S→C: arg0=win_id                     */
    IPC_OX_RELOAD_SETTINGS= 0x6c,  /* C→S: re-read /home/.oxrc             */
    IPC_OX_RESPONSE       = 0x6f   /* S→C: arg0=status arg1=value          */
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
