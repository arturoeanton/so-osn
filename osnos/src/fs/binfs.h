#pragma once

#include "vfs.h"

/*
 * Synthetic read-only filesystem at "/bin" backed by the builtin registry.
 *
 * Each builtin appears as a file:
 *   stat /bin/hello    -> regular file, mode 0555
 *   cat  /bin/hello    -> "builtin: hello — prints hello, world\n"
 *   ls   /bin          -> hello echo true false init
 *   exec /bin/hello    -> dispatches the builtin via proc_exec
 *
 * When FASE 6 ELF support lands, /bin moves to a real backing store
 * (initrd or disk) and this synthetic FS goes away. The shape stays.
 */
extern const vfs_ops_t binfs_vfs_ops;
