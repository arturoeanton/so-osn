#include "syscall.h"

#include <stddef.h>

#include "../drivers/framebuffer.h"
#include "../drivers/serial.h"
#include "../fs/vfs.h"
#include "../include/osnos_dirent.h"
#include "../include/osnos_fb_abi.h"
#include "../include/osnos_fcntl.h"
#include "../include/osnos_status.h"
#include "../include/osnos_taskinfo.h"
#include "../lib/string.h"
#include "../net/socket.h"
#include "../net/tcp.h"
#include "../proc/exec.h"
#include "tty.h"
#include "uaccess.h"
#include "fd.h"
#include "ipc.h"
#include "kmalloc.h"
#include "pipe.h"
#include "pmm.h"
#include "pty.h"
#include "scheduler.h"
#include "task.h"
#include "timer.h"
#include "vmm.h"

void syscall_init(void) {
    /* fd tables live inside task_t now (FASE 10.0.a) — task_create
     * invokes fd_init_for_task per slot. Nothing kernel-global to
     * initialise here. */
    tty_init();
}

/* Wrap an osnos_status_t into a syscall return: 0 on OK, -errno on error. */
static int64_t err(osnos_status_t s) {
    return -(int64_t)s;
}

/* ------------------------------------------------------------------ */
/* sys_open                                                           */
/* ------------------------------------------------------------------ */

/*
 * Special-case PTY path opens.
 *
 *   open("/dev/ptmx")    → allocate a fresh pty_pair, return master fd
 *                          (is_pty + pty_side=0 + pty_ref).
 *   open("/dev/pts/N")   → find existing pty_pair[N], return slave fd
 *                          (is_pty + pty_side=1 + pty_ref).
 *
 * The PTY layer has lifecycle semantics totally different from
 * regular char devices (each /dev/ptmx open spawns a new object;
 * /dev/pts/N is dynamic), so we bypass devfs's read/write callback
 * model here and wire the OFD directly to a pty_pair_t.
 *
 * Returns >=0 fd on success, negative -errno on failure, or -1 to
 * signal "not a PTY path, fall through to normal sys_open logic".
 */
static int64_t try_open_pty(const char *path, int flags) {
    (void)flags;
    if (!path) return -1;
    if (os_streq(path, "/dev/ptmx")) {
        pty_pair_t *p = pty_alloc();
        if (!p) return err(OSNOS_ENFILE);
        int fd = fd_alloc(task_current());
        if (fd < 0) { pty_master_unref(p); return err(OSNOS_EMFILE); }
        osnos_fd_t *o = fd_get(task_current(), fd);
        o->is_pty   = true;
        o->pty_ref  = p;
        o->pty_side = 0;
        o->flags    = flags;
        os_strlcpy(o->path, path, OSNOS_PATH_MAX);
        return fd;
    }
    if (os_streq(path, "/dev/tty")) {
        /* The controlling terminal — open returns a fresh OFD with
         * is_special=true so the rest of the fd path (sys_read /
         * sys_write / sys_ioctl) routes to the same kernel TTY layer
         * that backs the default fd 0/1/2. Used by pipe-mode pagers
         * (`cat foo | less` does `dup2(open("/dev/tty"), 0)` to get
         * the keyboard back after stdin was redirected to a pipe).
         *
         * Each open allocates its own OFD (NOT shared with the
         * default stdio OFDs) — they're all routing aliases of the
         * same singleton kernel TTY ring, so the duplication is just
         * bookkeeping with no real cost. */
        int fd = fd_alloc(task_current());
        if (fd < 0) return err(OSNOS_EMFILE);
        osnos_fd_t *o = fd_get(task_current(), fd);
        o->is_special = true;
        o->flags      = flags;
        os_strlcpy(o->path, path, OSNOS_PATH_MAX);
        return fd;
    }
    if (os_strstarts(path, "/dev/pts/")) {
        /* Parse N. Be conservative: only decimal, no leading zeros
         * past 0, no trailing junk. */
        const char *n_str = path + 9;
        if (!*n_str) return err(OSNOS_ENOENT);
        int n = 0;
        for (const char *p2 = n_str; *p2; p2++) {
            if (*p2 < '0' || *p2 > '9') return err(OSNOS_ENOENT);
            n = n * 10 + (*p2 - '0');
            if (n >= MAX_PTYS) return err(OSNOS_ENOENT);
        }
        pty_pair_t *p = pty_get(n);
        if (!p) return err(OSNOS_ENOENT);
        int fd = fd_alloc(task_current());
        if (fd < 0) return err(OSNOS_EMFILE);
        pty_slave_ref(p);
        osnos_fd_t *o = fd_get(task_current(), fd);
        o->is_pty   = true;
        o->pty_ref  = p;
        o->pty_side = 1;
        o->flags    = flags;
        os_strlcpy(o->path, path, OSNOS_PATH_MAX);
        return fd;
    }
    return -1;     /* not a PTY path */
}

int64_t sys_open(const char *path, int flags, uint32_t mode) {
    (void)mode;  /* permission bits not enforced yet */

    if (!path) return err(OSNOS_EFAULT);

    /* PTY special-case: handled by the dedicated layer, not VFS. */
    int64_t pty_rc = try_open_pty(path, flags);
    if (pty_rc != -1) return pty_rc;

    vfs_stat_t st;
    osnos_status_t s = vfs_stat(path, &st);
    bool exists = (s == OSNOS_OK);

    bool want_create = (flags & O_CREAT) != 0;
    bool want_excl   = (flags & O_EXCL) != 0;
    bool want_trunc  = (flags & O_TRUNC) != 0;

    if (!exists && !want_create)               return err(OSNOS_ENOENT);
    if (exists && want_create && want_excl)    return err(OSNOS_EEXIST);

    bool is_dir = exists && st.type == VFS_NODE_DIR;

    if (is_dir) {
        /* Directories may only be opened read-only — they're for
         * getdents, not write/truncate/create-over. */
        int access = flags & O_ACCMODE;
        if (access != O_RDONLY) return err(OSNOS_EISDIR);
        if (want_create || want_trunc) return err(OSNOS_EISDIR);
    }

    /* Create or truncate before allocating the fd so we fail cleanly. */
    if (!exists && want_create) {
        s = vfs_write(path, "", 0);
        if (s != OSNOS_OK) return err(s);
    } else if (want_trunc && !is_dir) {
        s = vfs_write(path, "", 0);
        if (s != OSNOS_OK) return err(s);
    }

    int fd = fd_alloc(task_current());
    if (fd < 0) return err(OSNOS_EMFILE);

    osnos_fd_t *f = fd_get(task_current(), fd);
    os_strlcpy(f->path, path, OSNOS_PATH_MAX);
    f->flags  = flags;
    f->offset = 0;
    f->is_dir = is_dir;
    /* Character devices need stream semantics — sys_read must NOT
     * apply offset-based slicing because the backend produces fresh
     * data on every call (keyboard event, fb scrollback, etc.). */
    f->is_chr = exists && (st.type == VFS_NODE_CHR);
    return fd;
}

/* ------------------------------------------------------------------ */
/* sys_close                                                          */
/* ------------------------------------------------------------------ */

int64_t sys_close(int fd) {
    osnos_fd_t *f = fd_get(task_current(), fd);
    if (!f) return err(OSNOS_EBADF);
    /* Refuse to close the default fd 0/1/2 stdio slots. Higher slots
     * marked is_special (e.g. an OFD obtained via open("/dev/tty")
     * for pipe-mode pagers) ARE closeable — the protection only
     * applies to the implicit "every task starts with stdio" slots. */
    if (f->is_special && fd < 3) return err(OSNOS_EBADF);
    /* Post-OFD refactor: fd_free decrements the OFD refcount.
     * When the refcount hits 0, ofd_unref dispatches the backend
     * cleanup (pipe_close_reader/writer, sock_close, pty_*_unref).
     * No manual backend cleanup here — that would double-close on
     * dup'd or fork-inherited fds. */
    fd_free(task_current(), fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_write                                                          */
/* ------------------------------------------------------------------ */

static int64_t write_to_console(const char *buf, size_t count) {
    if (count == 0) return 0;

    /*
     * One IPC message per call. If count is larger than the payload, we
     * write a short prefix and let the caller loop. Linux is allowed to
     * return short writes; standard libc loops to fill.
     */
    size_t n = count;
    if (n > IPC_DATA_SIZE - 1) n = IPC_DATA_SIZE - 1;

    ipc_msg_t msg;
    task_t *t = task_current();
    msg.from = t ? t->pid : 0;
    msg.to   = SERVER_CONSOLE;
    msg.type = IPC_CONSOLE_WRITE;
    msg.arg0 = 0xffffff;
    msg.arg1 = 0;

    for (size_t i = 0; i < n; i++) msg.data[i] = buf[i];
    msg.data[n] = 0;

    osnos_status_t s = ipc_send(&msg);
    if (s != OSNOS_OK) return err(s);
    return (int64_t)n;
}

int64_t sys_write(int fd, const void *buf, size_t count) {
    if (!buf && count > 0) return err(OSNOS_EFAULT);

    if (fd == OSNOS_FD_STDIN) return err(OSNOS_EBADF);

    if (fd == OSNOS_FD_STDOUT || fd == OSNOS_FD_STDERR) {
        task_t *cur = task_current();
        osnos_fd_t *sout = cur ? fd_get(cur, fd) : 0;
        /* Pipe end attached to fd 1 (set by proc_execve_pipeline or
         * sys_spawn fd inheritance). */
        if (sout && sout->is_pipe && sout->pipe_ref && sout->pipe_side == 1) {
            return pipe_write(sout->pipe_ref, buf, count);
        }
        /* Legacy task-level stdout redirect (used by the in-kernel
         * shell pre-FASE-10.4; ring-3 shellsrv uses fd inheritance
         * instead). */
        if (cur && cur->stdout_redir[0] != 0) {
            osnos_status_t s = vfs_append(cur->stdout_redir,
                                           (const char *)buf, count);
            if (s != OSNOS_OK) return err(s);
            cur->stdout_redir_off += count;
            return (int64_t)count;
        }
        /* Default stdin/stdout/stderr → console. Anything else (file
         * fd handed to the child via osn_spawn) falls through to the
         * regular VFS path below. */
        if (!sout || sout->is_special) {
            return write_to_console((const char *)buf, count);
        }
        /* fall through with `sout` as our fd entry */
    }

    osnos_fd_t *f = fd_get(task_current(), fd);
    if (!f || f->is_special) return err(OSNOS_EBADF);

    /* User-side pipe end via sys_pipe — short-circuit to the kernel
     * pipe object. Only the write side is legal here. */
    if (f->is_pipe) {
        if (f->pipe_side != 1 || !f->pipe_ref) return err(OSNOS_EBADF);
        return pipe_write(f->pipe_ref, buf, count);
    }

    /* PTY: dispatch to master/slave write. The other side's reader
     * picks bytes up via pty_*_read; canonical mode line-buffering
     * + echo are applied in pty_master_write itself. */
    if (f->is_pty && f->pty_ref) {
        return (f->pty_side == 0)
            ? pty_master_write(f->pty_ref, buf, count)
            : pty_slave_write (f->pty_ref, buf, count);
    }

    int access = f->flags & O_ACCMODE;
    if (access == O_RDONLY) return err(OSNOS_EBADF);
    if (count == 0) return 0;

    /*
     * sys_write at arbitrary file offsets.
     *
     * Backends don't have a write_at(offset) primitive yet, so we
     * emulate via read-modify-write:
     *   1. Stat the file to get its current size.
     *   2. Fast path: if write lands exactly at EOF (or O_APPEND
     *      forces it there), call vfs_append — no read needed.
     *   3. Slow path: read the whole file into a kmalloc'd scratch
     *      buffer, splice the new bytes at f->offset (zero-filling
     *      any sparse hole between existing EOF and f->offset),
     *      and replace the file with vfs_write.
     *
     * Bounded by SYS_WRITE_RMW_MAX so a stray giant write doesn't
     * monopolise the kernel heap. Enough headroom for TCC-sized
     * ELFs (~50–200 KiB) and any normal editor save.
     */
    #define SYS_WRITE_RMW_MAX (4 * 1024 * 1024)   /* 4 MiB */

    vfs_stat_t st;
    size_t existing = 0;
    bool is_chr = false;
    if (vfs_stat(f->path, &st) == OSNOS_OK) {
        if (st.type == VFS_NODE_DIR) return err(OSNOS_EISDIR);
        is_chr   = (st.type == VFS_NODE_CHR);
        /* Character devices are streams — size is meaningless; the
         * RMW slow path doesn't apply. Force the fast-path "off ==
         * existing" branch so bytes flow straight to the backend. */
        existing = is_chr ? f->offset : st.size;
    }

    size_t off = f->offset;
    if (f->flags & O_APPEND) off = existing;   /* POSIX: O_APPEND wins */

    /* Fast path: pure append. */
    if (off == existing) {
        osnos_status_t s = vfs_append(f->path, (const char *)buf, count);
        if (s != OSNOS_OK) return err(s);
        f->offset = off + count;
        return (int64_t)count;
    }

    /* Slow path: read-modify-write. */
    size_t total = (off + count > existing) ? off + count : existing;
    if (total > SYS_WRITE_RMW_MAX) return err(OSNOS_EFBIG);

    char *scratch = (char *)kmalloc(total);
    if (!scratch) return err(OSNOS_ENOMEM);

    /* Pull existing bytes. vfs_read fills as much as the file holds;
     * anything beyond `got` we zero-fill so the sparse-hole region
     * is well-defined. */
    if (existing > 0) {
        size_t got = 0;
        osnos_status_t s = vfs_read(f->path, scratch, total, &got);
        if (s != OSNOS_OK) { kfree(scratch); return err(s); }
        for (size_t i = got; i < existing; i++) scratch[i] = 0;
    }
    for (size_t i = existing; i < off; i++) scratch[i] = 0;
    const char *src = (const char *)buf;
    for (size_t i = 0; i < count; i++) scratch[off + i] = src[i];

    osnos_status_t s = vfs_write(f->path, scratch, total);
    kfree(scratch);
    if (s != OSNOS_OK) return err(s);

    f->offset = off + count;
    return (int64_t)count;
}

/* ------------------------------------------------------------------ */
/* sys_read                                                           */
/* ------------------------------------------------------------------ */

/*
 * POSIX-style restart_syscall: park the calling user task in the
 * blocked queue with iret RIP rewound 2 bytes (past the preceding
 * `syscall` or `int 0x80` instruction, both 2-byte) and saved_rax set
 * to the original syscall number so the CPU re-executes the same call
 * on wake-up. Returns 0 = OK (caller MUST treat as "this function
 * never returned — control transferred out") or non-zero on inability
 * to block (kernel-mode caller, no kstack, etc).
 *
 * After this returns control to sched_resume_jump, the next time the
 * task is dispatched the user-space RIP points at the syscall
 * instruction → the syscall is re-issued → kernel checks readiness
 * again. This is how Linux implements blocking I/O on top of a
 * non-preemptive in-kernel control flow.
 */
static int block_restart_syscall(uint64_t wakeup_at_ms, uint64_t syscall_nr) {
    task_t *tc = task_current();
    if (!tc || !tc->pml4 || !tc->kernel_stack_top) return -1;
    if (tc->kill_pending) return -1;

    uint64_t *iret = (uint64_t *)(tc->kernel_stack_top - 40);
    tc->saved_iret_rip    = iret[0] - 2;  /* re-execute syscall insn */
    tc->saved_iret_cs     = iret[1];
    tc->saved_iret_rflags = iret[2];
    tc->saved_iret_rsp    = iret[3];
    tc->saved_iret_ss     = iret[4];

    syscall_frame_t *sf =
        (syscall_frame_t *)(tc->kernel_stack_top - 40 - sizeof(*sf));
    tc->saved_rax = syscall_nr;
    tc->saved_rbx = sf->rbx;
    tc->saved_rcx = sf->rcx;
    tc->saved_rdx = sf->rdx;
    tc->saved_rsi = sf->rsi;
    tc->saved_rdi = sf->rdi;
    tc->saved_rbp = sf->rbp;
    tc->saved_r8  = sf->r8;
    tc->saved_r9  = sf->r9;
    tc->saved_r10 = sf->r10;
    tc->saved_r11 = sf->r11;
    tc->saved_r12 = sf->r12;
    tc->saved_r13 = sf->r13;
    tc->saved_r14 = sf->r14;
    tc->saved_r15 = sf->r15;
    tc->saved_valid  = 1;
    tc->wakeup_at_ms = wakeup_at_ms;
    tc->state        = TASK_BLOCKED;
    sched_resume_jump();                   /* never returns */
    return 0;                              /* unreachable */
}

int64_t sys_read(int fd, void *buf, size_t count) {
    if (!buf && count > 0) return err(OSNOS_EFAULT);

    if (fd == OSNOS_FD_STDIN) {
        task_t *cur = task_current();
        osnos_fd_t *sin = cur ? fd_get(cur, OSNOS_FD_STDIN) : 0;
        /* Pipe end attached to fd 0 (set by proc_execve_pipeline or
         * sys_spawn fd inheritance). */
        if (sin && sin->is_pipe && sin->pipe_ref && sin->pipe_side == 0) {
            return pipe_read(sin->pipe_ref, buf, count);
        }
        /* Legacy task-level stdin redirect (kernel shell). */
        if (cur && cur->stdin_redir[0] != 0) {
            static char rd_scratch[1024];
            size_t got = 0;
            osnos_status_t s = vfs_read(cur->stdin_redir,
                                         rd_scratch, sizeof(rd_scratch), &got);
            if (s != OSNOS_OK) return err(s);
            if (cur->stdin_redir_off >= got) return 0;   /* EOF */
            size_t remaining = got - cur->stdin_redir_off;
            size_t n = (count < remaining) ? count : remaining;
            char *out = (char *)buf;
            for (size_t i = 0; i < n; i++) {
                out[i] = rd_scratch[cur->stdin_redir_off + i];
            }
            cur->stdin_redir_off += n;
            return (int64_t)n;
        }

        /* If fd 0 has been overridden with a regular file fd (via
         * osn_spawn fd inheritance), fall through to the file-read
         * path below. Only the default is_special slot drains the
         * TTY. */
        if (!sin || sin->is_special) {
            size_t n = stdin_pop((char *)buf, count);
            if (n > 0) return (int64_t)n;
            if (count == 0) return 0;
            /* No data. If fd is explicitly O_NONBLOCK, return EAGAIN
             * (libc/app handles retry). Otherwise BLOCK by snapshotting
             * the iret frame with RIP rewound 2 bytes (past the
             * preceding `syscall` or `int 0x80` instruction, both
             * 2 bytes) and saved_rax = SYS_READ (0). The scheduler
             * will wake us after a short timeout and the CPU re-
             * executes the same read(), draining whatever input has
             * arrived. This is the POSIX restart_syscall pattern.
             * Critical for musl (and any libc that doesn't auto-retry
             * on EAGAIN): without this BusyBox ash thinks the read
             * returned 0 = EOF and exits with code 0. */
            int nonblock = (sin && (sin->flags & 0x800 /* O_NONBLOCK */));
            if (nonblock) return err(OSNOS_EAGAIN);
            if (block_restart_syscall(timer_ms() + 10, SYS_READ) != 0)
                return err(OSNOS_EAGAIN);
        }
        /* fall through with `sin` as our fd */
    }
    if (fd == OSNOS_FD_STDOUT || fd == OSNOS_FD_STDERR) {
        return err(OSNOS_EBADF);
    }

    osnos_fd_t *f = fd_get(task_current(), fd);
    if (!f || f->is_special) return err(OSNOS_EBADF);

    /* User-side pipe end via sys_pipe — short-circuit to the kernel
     * pipe object. Only the read side is legal here. */
    if (f->is_pipe) {
        if (f->pipe_side != 0 || !f->pipe_ref) return err(OSNOS_EBADF);
        return pipe_read(f->pipe_ref, buf, count);
    }

    /* PTY read — master and slave have different semantics:
     *   master reads s2m_buf raw (slave's output)
     *   slave  reads m2s_buf with canonical/raw processing
     * Both return -EAGAIN when their respective buffer is empty
     * (and the peer is still alive). libc loops via nanosleep. */
    if (f->is_pty && f->pty_ref) {
        return (f->pty_side == 0)
            ? pty_master_read(f->pty_ref, buf, count)
            : pty_slave_read (f->pty_ref, buf, count);
    }

    if (f->is_dir) return err(OSNOS_EISDIR);

    int access = f->flags & O_ACCMODE;
    if (access == O_WRONLY) return err(OSNOS_EBADF);

    /*
     * Char device: each backend call returns a fresh batch (one
     * keyboard event, the current FB span, etc.). Bypass the
     * offset-based slicing — a small stack scratch is enough since
     * char-device payloads are by design small per call.
     */
    if (f->is_chr) {
        char tmp[1024];
        size_t got = 0;
        osnos_status_t s = vfs_read(f->path, tmp, sizeof(tmp), &got);
        if (s != OSNOS_OK) return err(s);
        size_t n = (count < got) ? count : got;
        char *out = (char *)buf;
        for (size_t i = 0; i < n; i++) out[i] = tmp[i];
        return (int64_t)n;
    }

    /* Regular file: offset-native read straight into the user
     * buffer via the backend. No more "slurp whole file into a
     * heap scratch and slice" — that was O(file_size) per read
     * call and broke TCC's 50 KB header reads. Now O(count). */
    size_t got = 0;
    osnos_status_t s = vfs_read_at(f->path, f->offset,
                                    (char *)buf, count, &got);
    if (s != OSNOS_OK) return err(s);
    f->offset += got;
    return (int64_t)got;
}

/* ------------------------------------------------------------------ */
/* sys_lseek                                                          */
/* ------------------------------------------------------------------ */

int64_t sys_lseek(int fd, int64_t offset, int whence) {
    osnos_fd_t *f = fd_get(task_current(), fd);
    if (!f || f->is_special) return err(OSNOS_EBADF);

    int64_t new_offset;
    switch (whence) {
        case SEEK_SET:
            new_offset = offset;
            break;
        case SEEK_CUR:
            new_offset = (int64_t)f->offset + offset;
            break;
        case SEEK_END: {
            vfs_stat_t st;
            osnos_status_t s = vfs_stat(f->path, &st);
            if (s != OSNOS_OK) return err(s);
            new_offset = (int64_t)st.size + offset;
            break;
        }
        default:
            return err(OSNOS_EINVAL);
    }

    if (new_offset < 0) return err(OSNOS_EINVAL);
    f->offset = (uint64_t)new_offset;
    return new_offset;
}

/* ------------------------------------------------------------------ */
/* sys_fstat                                                          */
/* ------------------------------------------------------------------ */

int64_t sys_fstat(int fd, osnos_stat_t *out) {
    if (!out) return err(OSNOS_EFAULT);

    osnos_fd_t *f = fd_get(task_current(), fd);
    if (!f) return err(OSNOS_EBADF);

    for (size_t i = 0; i < sizeof(*out); i++) ((char *)out)[i] = 0;

    if (f->is_special) {
        /* stdin/stdout/stderr report as char devices. */
        out->st_mode    = (uint32_t)VFS_NODE_CHR | 0666;
        out->st_nlink   = 1;
        out->st_blksize = 1024;
        return 0;
    }

    vfs_stat_t st;
    osnos_status_t s = vfs_stat(f->path, &st);
    if (s != OSNOS_OK) return err(s);

    out->st_ino     = st.inode;
    out->st_nlink   = 1;
    out->st_mode    = (uint32_t)st.type | (st.mode & 07777);
    out->st_size    = (int64_t)st.size;
    out->st_blksize = 512;
    out->st_blocks  = (int64_t)((st.size + 511) / 512);
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_isatty                                                         */
/* ------------------------------------------------------------------ */

int64_t sys_isatty(int fd) {
    osnos_fd_t *f = fd_get(task_current(), fd);
    if (!f) return err(OSNOS_EBADF);
    return f->is_special ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* sys_exit                                                           */
/* ------------------------------------------------------------------ */

int64_t sys_exit(int code) {
    task_t *t = task_current();
    if (!t) return 0;

    /*
     * Ring-3 user task: delegate to proc_exit_current_user, which
     * tears down the AS, hands the per-task kstack to the reaper,
     * notifies the shell, and longjmps back to the scheduler. Never
     * returns.
     */
    if (t->pml4) proc_exit_current_user(code);

    /* Kernel-mode builtin: mark DEAD; the trampoline catches it. */
    t->state = TASK_DEAD;
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_reboot — power off / restart via ACPI                           */
/* ------------------------------------------------------------------ */

/* Linux reboot(2) command codes — we accept the canonical magic but
 * skip the cookie-magic dance (magic1 / magic2). osnos is hobby; if
 * userland made it to this syscall it was deliberate. */
#define OSNOS_RB_POWER_OFF    0x4321FEDC
#define OSNOS_RB_RESTART      0x01234567
#define OSNOS_RB_HALT_SYSTEM  0xCDEF0123

static inline void io_outw(uint16_t port, uint16_t v) {
    __asm__ volatile ("outw %0, %1" :: "a"(v), "Nd"(port));
}

static inline void io_outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(port));
}

static void halt_forever(void) {
    for (;;) __asm__ volatile ("cli; hlt");
}

int64_t sys_reboot(uint32_t cmd) {
    switch (cmd) {
    case OSNOS_RB_POWER_OFF:
        /* Try the QEMU shutdown vectors in order of likelihood:
         *   - PIIX4 (`-M pc`, our default boot): port 0xB004 + 0x2000
         *   - ICH9 (`-M q35`): port 0x604 + 0x2000
         *   - VirtualBox: port 0x4004 + 0x3400
         *   - Bochs / QEMU `isa-debug-exit`: port 0x501 + 0
         * Whichever the host responds to fires first; the rest are
         * harmless writes to unused I/O ports. */
        io_outw(0xB004, 0x2000);
        io_outw(0x0604, 0x2000);
        io_outw(0x4004, 0x3400);
        io_outb(0x0501, 0x00);
        halt_forever();          /* never reached on QEMU */
        break;
    case OSNOS_RB_RESTART:
        /* 8042 keyboard controller reset line — universally
         * supported, works on real HW too. */
        io_outb(0x64, 0xFE);
        halt_forever();
        break;
    case OSNOS_RB_HALT_SYSTEM:
        halt_forever();
        break;
    default:
        return err(OSNOS_EINVAL);
    }
    return 0;                    /* unreachable */
}

/* ------------------------------------------------------------------ */
/* sys_brk — grow / shrink the per-task user heap                     */
/* ------------------------------------------------------------------ */

#define PAGE_DOWN(x) ((x) & ~(uint64_t)(PAGE_SIZE - 1))
#define PAGE_UP(x)   (((x) + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1))

int64_t sys_brk(uintptr_t new_brk) {
    task_t *t = task_current();

    /*
     * Kernel-mode tasks (no pml4) have no per-process heap. Returning 0
     * keeps libc honest (it sees "couldn't grow" and falls back to
     * its emergency NULL).
     */
    if (!t || !t->pml4 || !t->heap_start) return 0;

    uint64_t cur = t->heap_brk;

    /* brk(0) — just report. */
    if (new_brk == 0) return (int64_t)cur;

    /* Out-of-range requests are silently refused (Linux convention). */
    if (new_brk < t->heap_start)            return (int64_t)cur;
    if (new_brk >= OSNOS_USER_VIRT_MAX - PAGE_SIZE) return (int64_t)cur;

    /* No motion -> nothing to do. */
    if (new_brk == cur) return (int64_t)cur;

    if (new_brk > cur) {
        /*
         * Grow: allocate + map every page in
         *   [PAGE_UP(cur), PAGE_UP(new_brk))
         * (we never re-allocate the page that already owns `cur`).
         * On any failure, roll back the pages we just allocated.
         */
        uint64_t lo = PAGE_UP(cur == t->heap_start ? cur : cur);
        uint64_t hi = PAGE_UP(new_brk);

        for (uint64_t va = lo; va < hi; va += PAGE_SIZE) {
            if (vmm_lookup(t->pml4, va) != 0) continue;  /* already mapped */

            uint64_t phys = pmm_alloc_page();
            if (!phys) goto rollback;

            uint8_t *zp = (uint8_t *)(phys + pmm_hhdm_offset());
            for (size_t i = 0; i < PAGE_SIZE; i++) zp[i] = 0;

            if (!vmm_map(t->pml4, va, phys, PTE_W | PTE_U)) {
                pmm_free_page(phys);
                goto rollback;
            }
            continue;

        rollback:
            for (uint64_t r = lo; r < va; r += PAGE_SIZE) {
                uint64_t p = vmm_lookup(t->pml4, r) & PTE_ADDR_MASK;
                if (p) {
                    vmm_unmap(t->pml4, r);
                    pmm_free_page(p);
                }
            }
            return (int64_t)cur;
        }
    } else {
        /*
         * Shrink: free every page entirely above the new break. We
         * unmap pages at [PAGE_UP(new_brk), PAGE_UP(cur)) — the page
         * containing the new brk byte stays mapped.
         */
        uint64_t lo = PAGE_UP(new_brk);
        uint64_t hi = PAGE_UP(cur);

        for (uint64_t va = lo; va < hi; va += PAGE_SIZE) {
            uint64_t p = vmm_lookup(t->pml4, va) & PTE_ADDR_MASK;
            if (!p) continue;
            vmm_unmap(t->pml4, va);
            pmm_free_page(p);
        }
    }

    t->heap_brk = new_brk;
    return (int64_t)new_brk;
}

/* ------------------------------------------------------------------ */
/* sys_nanosleep — cooperative hlt-loop until timer_ms() >= target    */
/* ------------------------------------------------------------------ */

/*
 * Suspend the current user task until wakeup_at_ms.
 *
 * Snapshots the iret frame (sitting at kernel_stack_top - 40) plus
 * the 15-GPR syscall_frame_t pushed by int80_entry / syscall_entry,
 * stashes them on the task, marks it BLOCKED, and longjmps back to
 * the scheduler via sched_resume_jump. The kernel stack of THIS
 * task is abandoned for the duration — it'll be fresh on the next
 * dispatch.
 *
 * user_task_trampoline detects t->saved_valid on the wake-up
 * dispatch and replays the iret frame + GPRs (with rax overwritten
 * by t->saved_return) so the user-mode instruction right after the
 * syscall continues with the right state.
 */
int64_t sys_nanosleep(const osnos_timespec_t *req, osnos_timespec_t *rem) {
    (void)rem;
    if (!req) return -(int64_t)OSNOS_EFAULT;

    int64_t sec  = req->tv_sec;
    int64_t nsec = req->tv_nsec;
    if (sec < 0 || nsec < 0 || nsec >= 1000000000) return -(int64_t)OSNOS_EINVAL;

    uint64_t ms = (uint64_t)sec * 1000ULL + (uint64_t)nsec / 1000000ULL;
    if (ms == 0) return 0;

    task_t *t = task_current();
    if (!t || !t->pml4 || !t->kernel_stack_top) {
        /* Kernel-mode caller (test path) — fall back to busy-hlt. */
        uint64_t target = timer_ms() + ms;
        __asm__ volatile ("sti");
        while (timer_ms() < target) __asm__ volatile ("hlt");
        return 0;
    }

    /* Iret frame is at the very top of the per-task kernel stack;
     * the CPU pushed it on syscall entry, layout: rip, cs, rflags,
     * rsp, ss going up from (kstack_top - 40). */
    uint64_t *iret = (uint64_t *)(t->kernel_stack_top - 40);
    t->saved_iret_rip    = iret[0];
    t->saved_iret_cs     = iret[1];
    t->saved_iret_rflags = iret[2];
    t->saved_iret_rsp    = iret[3];
    t->saved_iret_ss     = iret[4];

    /* GPRs come from the syscall_frame_t pushed by int80_entry just
     * below the iret frame: 15 * 8 = 120 bytes. rax gets overwritten
     * with 0 — the return value of nanosleep(2) on success. */
    syscall_frame_t *sf = (syscall_frame_t *)(t->kernel_stack_top - 40 - sizeof(*sf));
    t->saved_rax = 0;                        /* return value visible to user */
    t->saved_rbx = sf->rbx;
    t->saved_rcx = sf->rcx;
    t->saved_rdx = sf->rdx;
    t->saved_rsi = sf->rsi;
    t->saved_rdi = sf->rdi;
    t->saved_rbp = sf->rbp;
    t->saved_r8  = sf->r8;
    t->saved_r9  = sf->r9;
    t->saved_r10 = sf->r10;
    t->saved_r11 = sf->r11;
    t->saved_r12 = sf->r12;
    t->saved_r13 = sf->r13;
    t->saved_r14 = sf->r14;
    t->saved_r15 = sf->r15;

    t->saved_valid  = 1;
    t->wakeup_at_ms = timer_ms() + ms;
    t->state        = TASK_BLOCKED;

    sched_resume_jump();                     /* never returns */
}

/* ------------------------------------------------------------------ */
/* sys_kill — Linux kill(2). Today signal is ignored; we just flip    */
/* kill_pending on the target so the next kernel return path delivers */
/* the death. Refuses kernel tasks (pml4 == NULL).                    */
/* ------------------------------------------------------------------ */

/* Deliver `sig` to one task. Mutating helper used by both the
 * single-pid path and the broadcast path below. Returns 1 if
 * delivered (target was a live ring-3 task), 0 otherwise. */
static int kill_one_task(task_t *t, int sig) {
    if (!t || !t->pml4) return 0;
    if (t->state == TASK_UNUSED || t->state == TASK_DEAD ||
        t->state == TASK_ZOMBIE) return 0;
    t->sig_pending |= 1u << (sig - 1);
    if (sig == 2 /* SIGINT */ ||
        sig == 9 /* SIGKILL */ ||
        sig == 15 /* SIGTERM */) {
        t->kill_pending = 1;
    }
    /* Wake from BLOCKED/STOPPED so the signal lands promptly.
     * EINTR semantics (see longer comment in old sys_kill body): the
     * snapshotted saved_rax becomes -EINTR for the interrupted
     * blocking syscall to surface POSIX-correctly. */
    if (t->state == TASK_BLOCKED) {
        if (t->saved_valid) {
            t->saved_rax = (uint64_t)(int64_t)-(int64_t)OSNOS_EINTR;
        }
        t->wakeup_at_ms = 0;
        t->state        = TASK_READY;
    } else if (t->state == TASK_STOPPED) {
        /* SIGCONT specifically un-stops the task. SIGKILL also
         * force-wakes (POSIX uncatchable — can't be ignored even
         * while stopped). Other signals queue on sig_pending and
         * land when the task next dispatches (e.g. after SIGCONT). */
        if (sig == 18 /* SIGCONT */ || sig == 9 /* SIGKILL */) {
            t->state       = TASK_READY;
            t->wait_change = 2 /* WAIT_CONTINUED */;
            notify_parent_stop_continue(t);
        }
    }
    return 1;
}

int64_t sys_kill(uint64_t pid, int sig) {
    if (sig < 1 || sig > 31) return -(int64_t)OSNOS_EINVAL;

    /* Linux kill(2) overloads `pid` semantically:
     *   pid >  0  → that specific pid
     *   pid == 0  → all tasks in the caller's process group
     *   pid == -1 → all ring-3 tasks (best-effort broadcast)
     *   pid <  -1 → all tasks in process group (-pid)
     * uint64_t carries the sign bit transparently — cast back to
     * int64_t to recover the signed semantics. */
    int64_t spid = (int64_t)pid;

    if (spid > 0) {
        task_t *t = task_by_pid(pid);
        if (!t || !t->pml4) return -(int64_t)OSNOS_ESRCH;
        if (!kill_one_task(t, sig)) return -(int64_t)OSNOS_ESRCH;
        return 0;
    }

    /* Broadcast paths: walk the task table. Skip the caller for
     * pid==-1 broadcast (POSIX: shouldn't include self by default —
     * but we deliver to self too because our scope is "all live ring-3
     * tasks", and the caller can opt-out via a SIG_IGN install). */
    task_t *self = task_current();
    uint64_t target_pgid;
    if (spid == 0) {
        if (!self) return -(int64_t)OSNOS_ESRCH;
        target_pgid = self->pgid;
    } else {
        /* spid < 0: |spid| is the pgid (with spid==-1 a wildcard
         * broadcast). Use 0 as the wildcard sentinel internally. */
        target_pgid = (spid == -1) ? 0 : (uint64_t)(-spid);
    }

    int delivered = 0;
    for (size_t i = 0; i < MAX_TASKS; i++) {
        task_t *u = (task_t *)task_slot(i);
        if (!u) continue;
        if (u->state == TASK_UNUSED || !u->pml4) continue;
        if (target_pgid != 0 && u->pgid != target_pgid) continue;
        delivered += kill_one_task(u, sig);
    }
    if (delivered == 0) return -(int64_t)OSNOS_ESRCH;
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_getpid — current task's pid. Always non-zero in user context.  */
/* ------------------------------------------------------------------ */

int64_t sys_getpid(void) {
    task_t *t = task_current();
    if (!t) return 0;
    return (int64_t)t->pid;
}

/* ------------------------------------------------------------------ */
/* Process group + session syscalls (POSIX job-control).              */
/*                                                                    */
/* All read-only lookups: getppid / getpgid / getsid / getpgrp.       */
/* Mutators: setpgid / setsid. Linux x86_64 numbers throughout.       */
/*                                                                    */
/* Today these only update the task_t fields and gate kill(-pgid).    */
/* Signal routing on Ctrl+C still targets kernel_fg_pid (single pid)  */
/* — fan-out to whole pgid is a future enhancement that doesn't       */
/* require ABI changes here.                                          */
/* ------------------------------------------------------------------ */

int64_t sys_getppid(void) {
    task_t *t = task_current();
    if (!t) return 0;
    return (int64_t)t->parent_pid;
}

int64_t sys_getpgrp(void) {
    task_t *t = task_current();
    if (!t) return 0;
    return (int64_t)t->pgid;
}

int64_t sys_getpgid(uint64_t pid) {
    task_t *t = (pid == 0) ? task_current() : task_by_pid(pid);
    if (!t || t->state == TASK_UNUSED) return err(OSNOS_ESRCH);
    return (int64_t)t->pgid;
}

int64_t sys_getsid(uint64_t pid) {
    task_t *t = (pid == 0) ? task_current() : task_by_pid(pid);
    if (!t || t->state == TASK_UNUSED) return err(OSNOS_ESRCH);
    return (int64_t)t->sid;
}

/* setpgid(pid, pgid) — sets the process-group ID of `pid` (0 = self)
 * to `pgid` (0 = pid). POSIX restrictions we honour:
 *   - target must exist and be a ring-3 task
 *   - cannot move a process across sessions: requested pgid's
 *     existing leader (if any) must share our session
 *   - cannot setpgid a session leader (its pid == sid)
 * Returns 0 on success or -EPERM / -ESRCH / -EINVAL. */
int64_t sys_setpgid(uint64_t pid, uint64_t pgid) {
    task_t *self = task_current();
    if (!self) return err(OSNOS_ESRCH);

    task_t *t = (pid == 0) ? self : task_by_pid(pid);
    if (!t || t->state == TASK_UNUSED || !t->pml4) return err(OSNOS_ESRCH);

    uint64_t new_pgid = (pgid == 0) ? t->pid : pgid;

    /* Session leader can't change its pgid (POSIX EPERM). */
    if (t->pid == t->sid && new_pgid != t->pgid) {
        return err(OSNOS_EPERM);
    }

    /* If new_pgid already exists as a group somewhere, it must live
     * in the same session as `t`. Walk the task table looking for a
     * pgid leader (pid == pgid) with that group id. */
    if (new_pgid != t->pid) {
        int found_match = 0;
        for (size_t i = 0; i < MAX_TASKS; i++) {
            task_t *u = (task_t *)task_slot(i);
            if (!u || u->state == TASK_UNUSED) continue;
            if (u->pgid != new_pgid) continue;
            if (u->sid != t->sid) return err(OSNOS_EPERM);
            found_match = 1;
            break;
        }
        if (!found_match) return err(OSNOS_EPERM);
    }

    t->pgid = new_pgid;
    return 0;
}

/* setsid() — create a new session: current task becomes session
 * leader (sid = pid) AND process-group leader (pgid = pid). Fails
 * with -EPERM if the caller is already a process-group leader
 * (POSIX, prevents a leader from "escaping" its group). */
int64_t sys_setsid(void) {
    task_t *t = task_current();
    if (!t) return err(OSNOS_ESRCH);
    if (t->pgid == t->pid) {
        /* Already a process-group leader → setsid forbidden. */
        return err(OSNOS_EPERM);
    }
    t->sid  = t->pid;
    t->pgid = t->pid;
    return (int64_t)t->sid;
}

/* ------------------------------------------------------------------ */
/* sys_mkdir / sys_rmdir / sys_unlink / sys_rename                    */
/* ------------------------------------------------------------------ */

int64_t sys_mkdir(const char *path, uint32_t mode) {
    (void)mode;  /* permission bits not enforced yet */
    if (!path) return err(OSNOS_EFAULT);
    return err(vfs_mkdir(path));
}

int64_t sys_rmdir(const char *path) {
    if (!path) return err(OSNOS_EFAULT);
    return err(vfs_rmdir(path));
}

int64_t sys_unlink(const char *path) {
    if (!path) return err(OSNOS_EFAULT);
    return err(vfs_unlink(path));
}

int64_t sys_rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return err(OSNOS_EFAULT);
    return err(vfs_move(oldpath, newpath));
}

/* ------------------------------------------------------------------ */
/* sys_getdents — Linux getdents64 layout                              */
/* ------------------------------------------------------------------ */

static uint8_t vfs_type_to_dt(vfs_node_type_t t) {
    switch (t) {
        case VFS_NODE_FIFO: return OSNOS_DT_FIFO;
        case VFS_NODE_CHR:  return OSNOS_DT_CHR;
        case VFS_NODE_DIR:  return OSNOS_DT_DIR;
        case VFS_NODE_BLK:  return OSNOS_DT_BLK;
        case VFS_NODE_REG:  return OSNOS_DT_REG;
        case VFS_NODE_LNK:  return OSNOS_DT_LNK;
        case VFS_NODE_SOCK: return OSNOS_DT_SOCK;
        default:            return OSNOS_DT_UNKNOWN;
    }
}

int64_t sys_getdents(int fd, void *buf, size_t buf_size) {
    if (!buf && buf_size > 0) return err(OSNOS_EFAULT);

    osnos_fd_t *f = fd_get(task_current(), fd);
    if (!f || f->is_special) return err(OSNOS_EBADF);
    if (!f->is_dir)          return err(OSNOS_ENOTDIR);

    char *out = (char *)buf;
    size_t written = 0;
    size_t cursor = (size_t)f->offset;

    for (;;) {
        vfs_dirent_t ent;
        size_t prev_cursor = cursor;
        osnos_status_t s = vfs_readdir(f->path, &cursor, &ent);
        if (s == OSNOS_ENOENT) break;       /* end of dir */
        if (s != OSNOS_OK) {
            if (written > 0) break;         /* return what we have */
            return err(s);
        }

        size_t name_len = os_strlen(ent.name);
        /*
         * record layout:
         *   d_ino (8) + d_off (8) + d_reclen (2) + d_type (1) + name + '\0'
         * then pad to 8-byte alignment.
         */
        size_t raw = 8 + 8 + 2 + 1 + name_len + 1;
        size_t reclen = (raw + 7) & ~(size_t)7;

        if (written + reclen > buf_size) {
            /* Not enough room — rewind the cursor so the caller retries
             * with a bigger buffer (or after consuming what we wrote). */
            cursor = prev_cursor;
            break;
        }

        osnos_dirent_t *d = (osnos_dirent_t *)(out + written);
        d->d_ino    = ent.type == VFS_NODE_DIR ? 0 : 0;  /* TODO: real inode */
        d->d_off    = (int64_t)cursor;
        d->d_reclen = (uint16_t)reclen;
        d->d_type   = vfs_type_to_dt(ent.type);
        for (size_t i = 0; i < name_len; i++) d->d_name[i] = ent.name[i];
        d->d_name[name_len] = 0;
        /* zero the alignment padding */
        for (size_t i = name_len + 1; i < reclen - (8 + 8 + 2 + 1); i++) {
            d->d_name[i] = 0;
        }

        written += reclen;
    }

    f->offset = (uint64_t)cursor;
    return (int64_t)written;
}

/* ------------------------------------------------------------------ */
/* Frame dispatcher (ring3 entry point eventually)                    */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Linux socket syscalls (8.5.4b)                                     */
/* ------------------------------------------------------------------ */

#define AF_INET_LX  2

/*
 * sockaddr_in helpers — fixed Linux layout. The user buffer is read
 * byte-by-byte to keep us neutral about alignment / strict aliasing.
 */
static bool unpack_sockaddr_in(const void *addr, uint32_t addrlen,
                                 uint32_t *ip_out, uint16_t *port_out,
                                 int64_t *err_out) {
    if (!addr || addrlen < 16) { *err_out = err(OSNOS_EINVAL); return false; }
    const uint8_t *p = (const uint8_t *)addr;
    uint16_t fam = (uint16_t)(p[0] | (p[1] << 8));
    if (fam != AF_INET_LX) { *err_out = err(OSNOS_EAFNOSUPPORT); return false; }
    *port_out = (uint16_t)((p[2] << 8) | p[3]);
    *ip_out   = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
                ((uint32_t)p[6] << 8)  | (uint32_t)p[7];
    return true;
}

static void pack_sockaddr_in(void *addr, uint32_t ip, uint16_t port) {
    uint8_t *p = (uint8_t *)addr;
    p[0] = AF_INET_LX; p[1] = 0;
    p[2] = (uint8_t)(port >> 8);
    p[3] = (uint8_t)port;
    p[4] = (uint8_t)(ip >> 24);
    p[5] = (uint8_t)(ip >> 16);
    p[6] = (uint8_t)(ip >> 8);
    p[7] = (uint8_t)ip;
    for (int i = 8; i < 16; i++) p[i] = 0;
}

int64_t sys_socket(int domain, int type, int protocol) {
    (void)protocol;
    if (domain != AF_INET_LX) return err(OSNOS_EAFNOSUPPORT);
    if (type != OSNOS_SOCK_DGRAM && type != OSNOS_SOCK_STREAM) {
        return err(OSNOS_EAFNOSUPPORT);
    }

    int sd = sock_create(type);
    if (sd < 0) return err(OSNOS_EMFILE);

    int fd = fd_alloc(task_current());
    if (fd < 0) {
        sock_close(sd);
        return err(OSNOS_EMFILE);
    }

    osnos_fd_t *f = fd_get(task_current(), fd);
    f->is_socket = true;
    f->sock_idx  = sd;
    return (int64_t)fd;
}

int64_t sys_bind(int fd, const void *addr, uint32_t addrlen) {
    osnos_fd_t *f = fd_get(task_current(), fd);
    if (!f || !f->is_socket) return err(OSNOS_EBADF);

    uint32_t ip = 0;
    uint16_t port = 0;
    int64_t e;
    if (!unpack_sockaddr_in(addr, addrlen, &ip, &port, &e)) return e;

    if (sock_bind(f->sock_idx, ip, port) != 0) return err(OSNOS_EADDRINUSE);
    return 0;
}

int64_t sys_listen(int fd, int backlog) {
    osnos_fd_t *f = fd_get(task_current(), fd);
    if (!f || !f->is_socket) return err(OSNOS_EBADF);
    if (sock_listen(f->sock_idx, backlog) != 0) return err(OSNOS_EINVAL);
    return 0;
}

int64_t sys_connect(int fd, const void *addr, uint32_t addrlen) {
    osnos_fd_t *f = fd_get(task_current(), fd);
    if (!f || !f->is_socket) return err(OSNOS_EBADF);

    uint32_t ip = 0;
    uint16_t port = 0;
    int64_t e;
    if (!unpack_sockaddr_in(addr, addrlen, &ip, &port, &e)) return e;

    /* Non-blocking single-step: 0 = ESTABLISHED, -2 = still in progress
     * (SYN_SENT), -1 = refused / bad. libc retries on EINPROGRESS with
     * nanosleep in between, which lets the scheduler dispatch other
     * tasks (keyboard, shell) → Ctrl+C is deliverable mid-connect. */
    int r = sock_connect(f->sock_idx, ip, port, 0);
    if (r == 0)  return 0;
    if (r == -2) return err(OSNOS_EINPROGRESS);
    return err(OSNOS_ECONNREFUSED);
}

/*
 * setsockopt: only SO_REUSEADDR (level=SOL_SOCKET=1, optname=2) is a
 * no-op success — enough to let Beej-style servers run. Other options
 * report ENOSYS so the caller knows they're not implemented yet.
 */
int64_t sys_setsockopt(int fd, int level, int optname,
                        const void *optval, uint32_t optlen) {
    (void)optval; (void)optlen;
    osnos_fd_t *f = fd_get(task_current(), fd);
    if (!f || !f->is_socket) return err(OSNOS_EBADF);
    /* SOL_SOCKET = 1, SO_REUSEADDR = 2 (Linux numbers, see libc). */
    if (level == 1 && optname == 2) return 0;
    return err(OSNOS_EINVAL);
}

/* ----- select(2) ----- */

#define FDSET_NWORDS  16          /* 1024 bits = 16 * uint64_t */

static bool fd_readable(int fd) {
    osnos_fd_t *f = fd_get(task_current(), fd);
    if (!f) return false;
    if (f->is_special) {
        return (fd == OSNOS_FD_STDIN) ? stdin_readable() : false;
    }
    if (f->is_socket) return sock_readable(f->sock_idx);
    if (f->is_pty && f->pty_ref) {
        return (f->pty_side == 0)
            ? pty_master_readable(f->pty_ref)
            : pty_slave_readable (f->pty_ref);
    }
    /* Regular files / dirs are always readable up to EOF. */
    return true;
}

/*
 * sys_getcwd — copy the current task's cwd into `buf`. Returns the
 * byte count INCLUDING the trailing NUL (matches Linux). -ERANGE if
 * the caller's buffer is too small; -EFAULT on bad pointer.
 */
int64_t sys_getcwd(char *buf, size_t size) {
    task_t *t = task_current();
    if (!t || !t->pml4) return err(OSNOS_ESRCH);
    if (!buf && size > 0) return err(OSNOS_EFAULT);

    const char *src = t->cwd[0] ? t->cwd : "/";
    size_t len = 0;
    while (src[len]) len++;
    if (size <= len) return err(OSNOS_ERANGE);

    if (copy_to_user(buf, src, len + 1) != OSNOS_OK) return err(OSNOS_EFAULT);
    return (int64_t)(len + 1);
}

/*
 * sys_chdir — change the current task's cwd. Validates that the path
 * exists and is a directory before adopting it.
 */
int64_t sys_chdir(const char *path) {
    task_t *t = task_current();
    if (!t || !t->pml4) return err(OSNOS_ESRCH);
    if (!path) return err(OSNOS_EFAULT);

    char kpath[OSNOS_PATH_MAX];
    if (copy_from_user(kpath, path, OSNOS_PATH_MAX) != OSNOS_OK) {
        return err(OSNOS_EFAULT);
    }
    kpath[OSNOS_PATH_MAX - 1] = 0;

    vfs_stat_t st;
    osnos_status_t s = vfs_stat(kpath, &st);
    if (s != OSNOS_OK) return err(s);
    if (st.type != VFS_NODE_DIR) return err(OSNOS_ENOTDIR);

    os_strlcpy(t->cwd, kpath, OSNOS_PATH_MAX);
    return 0;
}

/*
 * sys_stat — like fstat but takes an absolute path. Fills the
 * Linux-layout osnos_stat_t. Same field semantics as fstat (st_mode
 * carries type | perms; st_size in bytes; timestamps zeroed since
 * we have no RTC).
 */
int64_t sys_stat(const char *path, void *out) {
    if (!path || !out) return err(OSNOS_EFAULT);

    /* Pull path into kernel scratch one byte at a time so we stop at
     * the NUL terminator instead of pulling a full PATH_MAX block.
     * The earlier "copy_from_user(kpath, path, OSNOS_PATH_MAX)"
     * pattern faulted whenever the user string sat near a page
     * boundary — perfectly legal for a 2-byte string like "/", but
     * we'd try to read 128 bytes and hit an unmapped page. EFAULT
     * back to musl made `ls /` fail with "cannot open /". */
    char kpath[OSNOS_PATH_MAX];
    size_t i = 0;
    for (; i < OSNOS_PATH_MAX - 1; i++) {
        char c;
        if (copy_from_user(&c, path + i, 1) != OSNOS_OK)
            return err(OSNOS_EFAULT);
        kpath[i] = c;
        if (c == 0) break;
    }
    kpath[i] = 0;

    vfs_stat_t st;
    osnos_status_t s = vfs_stat(kpath, &st);
    if (s != OSNOS_OK) return err(s);

    osnos_stat_t kout;
    for (size_t i = 0; i < sizeof(kout); i++) ((char *)&kout)[i] = 0;
    kout.st_ino     = st.inode;
    kout.st_nlink   = 1;
    kout.st_mode    = (uint32_t)st.type | (st.mode & 07777);
    kout.st_size    = (int64_t)st.size;
    kout.st_blksize = 512;
    kout.st_blocks  = (int64_t)((st.size + 511) / 512);

    if (copy_to_user(out, &kout, sizeof(kout)) != OSNOS_OK) {
        return err(OSNOS_EFAULT);
    }
    return 0;
}

/*
 * sys_access — POSIX access(2). osnos doesn't enforce permission bits
 * yet, so the only relevant check is "does the path resolve?". `mode`
 * (R_OK / W_OK / X_OK / F_OK) is accepted but ignored.
 */
int64_t sys_access(const char *path, int mode) {
    (void)mode;
    if (!path) return err(OSNOS_EFAULT);

    char kpath[OSNOS_PATH_MAX];
    if (copy_from_user(kpath, path, OSNOS_PATH_MAX) != OSNOS_OK) {
        return err(OSNOS_EFAULT);
    }
    kpath[OSNOS_PATH_MAX - 1] = 0;

    vfs_stat_t st;
    osnos_status_t s = vfs_stat(kpath, &st);
    if (s != OSNOS_OK) return err(s);
    return 0;
}

/*
 * sys_time — POSIX time(2). Returns seconds "since the epoch" — but
 * osnos has no RTC, so we report seconds since boot. Good enough for
 * elapsed-time arithmetic; absolute clocks will need a real RTC.
 */
int64_t sys_time(int64_t *t) {
    int64_t secs = (int64_t)(timer_ms() / 1000);
    if (t) {
        if (copy_to_user(t, &secs, sizeof(secs)) != OSNOS_OK) {
            return err(OSNOS_EFAULT);
        }
    }
    return secs;
}

/* Linux clock IDs (subset). */
#define OSNOS_CLOCK_REALTIME  0
#define OSNOS_CLOCK_MONOTONIC 1

struct osnos_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/*
 * sys_clock_gettime — POSIX clock_gettime(2). Today both REALTIME
 * and MONOTONIC report the same value (ticks since boot, no RTC).
 * Other clock IDs return -EINVAL.
 */
int64_t sys_clock_gettime(int clk_id, void *tp) {
    if (clk_id != OSNOS_CLOCK_REALTIME &&
        clk_id != OSNOS_CLOCK_MONOTONIC) return err(OSNOS_EINVAL);
    if (!tp) return err(OSNOS_EFAULT);

    uint64_t ms = timer_ms();
    struct osnos_timespec ts;
    ts.tv_sec  = (int64_t)(ms / 1000);
    ts.tv_nsec = (int64_t)((ms % 1000) * 1000000);

    if (copy_to_user(tp, &ts, sizeof(ts)) != OSNOS_OK) return err(OSNOS_EFAULT);
    return 0;
}

/* ----- mmap / munmap (anonymous only) ----- */

#define USER_MMAP_BASE  0x20000000ULL  /* between heap and stack */
#define USER_MMAP_LIMIT 0x40000000ULL  /* 1 GiB window — plenty for now */

/* mmap flag bits — Linux asm-generic/mman-common.h. */
#define OSNOS_MAP_SHARED    0x01
#define OSNOS_MAP_PRIVATE   0x02
#define OSNOS_MAP_FIXED     0x10
#define OSNOS_MAP_ANONYMOUS 0x20

/* mmap prot bits. */
#define OSNOS_PROT_NONE  0x0
#define OSNOS_PROT_READ  0x1
#define OSNOS_PROT_WRITE 0x2
#define OSNOS_PROT_EXEC  0x4

#define MMAP_MAP_FAILED ((int64_t)-1)

int64_t sys_mmap(void *addr, size_t length, int prot, int flags,
                  int fd, int64_t offset) {
    (void)addr;   /* hint ignored (no MAP_FIXED) */
    (void)offset;

    task_t *t = task_current();
    if (!t || !t->pml4) return -(int64_t)OSNOS_EPERM;

    if (length == 0) return -(int64_t)OSNOS_EINVAL;
    /* File-backed mmap not yet implemented. */
    if (fd != -1 || !(flags & OSNOS_MAP_ANONYMOUS)) {
        return -(int64_t)OSNOS_ENOSYS;
    }
    /* MAP_FIXED ignored: the bump cursor decides where everything
     * lands. Real FIXED needs a free-page tracker. */

    size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) return -(int64_t)OSNOS_EINVAL;

    /* Initialise bump cursor on first call. */
    if (t->mmap_next == 0) t->mmap_next = USER_MMAP_BASE;

    if (t->mmap_next + pages * PAGE_SIZE > USER_MMAP_LIMIT) {
        return -(int64_t)OSNOS_ENOMEM;
    }

    /* Reserve a region slot. */
    int slot = -1;
    for (int i = 0; i < TASK_MMAP_MAX; i++) {
        if (t->mmap_regions[i].addr == 0) { slot = i; break; }
    }
    if (slot < 0) return -(int64_t)OSNOS_ENOMEM;

    uint64_t pte_flags = PTE_U;
    if (prot & OSNOS_PROT_WRITE) pte_flags |= PTE_W;
    /* PROT_READ is always implicit on x86_64 paged memory; PROT_EXEC
     * needs NX management, postponed. */

    uint64_t base_va = t->mmap_next;

    /* Allocate + map page by page. On any failure, unwind. */
    for (size_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            /* Undo. */
            for (size_t j = 0; j < i; j++) {
                uint64_t va = base_va + j * PAGE_SIZE;
                uint64_t p  = vmm_lookup(t->pml4, va) & PTE_ADDR_MASK;
                vmm_unmap(t->pml4, va);
                if (p) pmm_free_page(p);
            }
            return -(int64_t)OSNOS_ENOMEM;
        }
        /* Zero-fill — mmap'd memory is observably zero. */
        uint8_t *kvirt = (uint8_t *)(phys + pmm_hhdm_offset());
        for (size_t b = 0; b < PAGE_SIZE; b++) kvirt[b] = 0;

        if (!vmm_map(t->pml4, base_va + i * PAGE_SIZE, phys, pte_flags)) {
            pmm_free_page(phys);
            /* Same unwind as above. */
            for (size_t j = 0; j < i; j++) {
                uint64_t va = base_va + j * PAGE_SIZE;
                uint64_t p  = vmm_lookup(t->pml4, va) & PTE_ADDR_MASK;
                vmm_unmap(t->pml4, va);
                if (p) pmm_free_page(p);
            }
            return -(int64_t)OSNOS_ENOMEM;
        }
    }

    t->mmap_regions[slot].addr = base_va;
    t->mmap_regions[slot].len  = pages * PAGE_SIZE;
    t->mmap_next += pages * PAGE_SIZE;
    return (int64_t)base_va;
}

int64_t sys_munmap(void *addr, size_t length) {
    task_t *t = task_current();
    if (!t || !t->pml4) return -(int64_t)OSNOS_EPERM;
    if (length == 0) return -(int64_t)OSNOS_EINVAL;

    uint64_t target = (uint64_t)addr;
    /* Find a region whose start matches. POSIX accepts partial
     * unmaps (`munmap(addr + N, len - N)`); we don't — keeps the
     * bookkeeping trivial. */
    for (int i = 0; i < TASK_MMAP_MAX; i++) {
        if (t->mmap_regions[i].addr != target) continue;
        uint64_t reg_len = t->mmap_regions[i].len;
        size_t pages = (reg_len + PAGE_SIZE - 1) / PAGE_SIZE;
        for (size_t p = 0; p < pages; p++) {
            uint64_t va  = target + p * PAGE_SIZE;
            uint64_t pte = vmm_lookup(t->pml4, va);
            uint64_t phys = pte & PTE_ADDR_MASK;
            vmm_unmap(t->pml4, va);
            if (phys) pmm_free_page(phys);
        }
        t->mmap_regions[i].addr = 0;
        t->mmap_regions[i].len  = 0;
        return 0;
    }
    return -(int64_t)OSNOS_EINVAL;
}

/* fcntl cmds — Linux asm-generic/fcntl.h. */
#define OSNOS_F_DUPFD 0
#define OSNOS_F_GETFD 1
#define OSNOS_F_SETFD 2
#define OSNOS_F_GETFL 3
#define OSNOS_F_SETFL 4

/* O_APPEND and O_NONBLOCK are the only flags F_SETFL is allowed to
 * mutate — everything else (access mode, O_CREAT, O_TRUNC, ...) is
 * fixed at open time. */
#define OSNOS_O_APPEND   01000   /* matches Linux's #define */
#define OSNOS_O_NONBLOCK 04000

int64_t sys_dup(int fd) {
    int r = fd_dup(task_current(), fd);
    if (r < 0) return err(OSNOS_EBADF);
    return r;
}

int64_t sys_dup2(int oldfd, int newfd) {
    /* fd_dup2 validates oldfd internally; we just gate the newfd
     * range here to surface a clean EBADF instead of -1. */
    if (newfd < 0 || newfd >= OSNOS_MAX_FDS) return err(OSNOS_EBADF);
    int r = fd_dup2(task_current(), oldfd, newfd);
    if (r < 0) return err(OSNOS_EBADF);
    return r;
}

/* ------------------------------------------------------------------ */
/* sys_ipc_send — Linux-style copy_from_user wrapper around ipc_send. */
/* arg0 is the user pointer to a fully-formed ipc_msg_t. We always    */
/* override the `from` field with the caller's pid so a malicious     */
/* sender can't impersonate another service.                          */
/* ------------------------------------------------------------------ */

int64_t sys_ipc_send(const ipc_msg_t *user_msg) {
    if (!user_msg) return err(OSNOS_EFAULT);

    ipc_msg_t kmsg;
    if (copy_from_user(&kmsg, user_msg, sizeof(kmsg)) != OSNOS_OK) {
        return err(OSNOS_EFAULT);
    }

    task_t *t = task_current();
    if (t) kmsg.from = t->pid;

    osnos_status_t s = ipc_send(&kmsg);
    if (s != OSNOS_OK) return err(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_ipc_recv — non-blocking pop from the shared queue. If nothing  */
/* matches the caller's pid, returns -EAGAIN so libc can spin via     */
/* nanosleep. Always copies the full ipc_msg_t out to user.           */
/* `blocking` arg accepted but unused today (libc handles it).        */
/* ------------------------------------------------------------------ */

int64_t sys_ipc_recv(ipc_msg_t *user_out, int blocking) {
    (void)blocking;
    if (!user_out) return err(OSNOS_EFAULT);

    task_t *t = task_current();
    if (!t) return err(OSNOS_ESRCH);

    ipc_msg_t kmsg;
    if (!ipc_recv(t->pid, &kmsg)) {
        return err(OSNOS_EAGAIN);
    }
    if (copy_to_user(user_out, &kmsg, sizeof(kmsg)) != OSNOS_OK) {
        return err(OSNOS_EFAULT);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_service_register — bind SERVER_* sid to caller's pid. The      */
/* registry is a flat lookup keyed by sid; later registrations win.   */
/* ------------------------------------------------------------------ */

int64_t sys_service_register(int sid) {
    if (sid < 0 || sid >= 16) return err(OSNOS_EINVAL);
    task_t *t = task_current();
    if (!t) return err(OSNOS_ESRCH);
    service_register((uint64_t)sid, t->pid);
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_service_lookup — returns the pid associated with sid, or       */
/* -ENOENT if none registered.                                        */
/* ------------------------------------------------------------------ */

int64_t sys_service_lookup(int sid) {
    if (sid < 0 || sid >= 16) return err(OSNOS_EINVAL);
    uint64_t pid = service_get_pid((uint64_t)sid);
    if (pid == 0) return err(OSNOS_ENOENT);
    return (int64_t)pid;
}

/* ------------------------------------------------------------------ */
/* sys_tty_input — feed one byte into the kernel TTY line discipline. */
/* Restricted: only the task currently holding SERVER_KEYBOARD in the */
/* registry (the ring-3 kbdsrv from FASE 10.2) may call. Other tasks  */
/* get -EPERM so a random ELF can't impersonate keystrokes.           */
/* ------------------------------------------------------------------ */

int64_t sys_tty_input(int c) {
    task_t *t = task_current();
    if (!t) return err(OSNOS_ESRCH);
    uint64_t kbd_pid = service_get_pid(SERVER_KEYBOARD);
    if (kbd_pid == 0 || t->pid != kbd_pid) return err(OSNOS_EPERM);
    tty_input((char)c);
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_set_fg — publish the "currently foreground task" pid so tty.c  */
/* can route Ctrl+C / Ctrl+Z signals from a ring-3 shell. pid=0       */
/* clears the override (falls back to legacy shell_fg_pid()).         */
/* ------------------------------------------------------------------ */

uint64_t kernel_fg_pid = 0;

int64_t sys_set_fg(uint64_t pid) {
    /* Open today — any task may set. Restrict to SERVER_SHELL once
     * we have proper session/process-group bookkeeping. */
    kernel_fg_pid = pid;
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_resume — flip TASK_STOPPED back to TASK_READY without setting  */
/* kill_pending. SIGCONT-style. Used by the ring-3 shell's fg/bg.     */
/* ------------------------------------------------------------------ */

int64_t sys_resume(uint64_t pid) {
    task_t *t = task_by_pid(pid);
    if (!t || !t->pml4) return err(OSNOS_ESRCH);
    if (t->state == TASK_STOPPED) {
        t->state       = TASK_READY;
        t->wait_change = 2 /* WAIT_CONTINUED */;
        notify_parent_stop_continue(t);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_fork (#57) — Linux fork(2). Deep-clones the current task.      */
/* ------------------------------------------------------------------ */

#define USER_KSTACK_BYTES 16384

int64_t sys_fork(void) {
    task_t *parent = task_current();
    if (!parent || !parent->pml4 || !parent->kernel_stack_top) {
        return err(OSNOS_ESRCH);
    }

    /* 1. Deep-copy the user address space. Largest single allocation;
     *    do it first so we can bail cheaply on OOM. */
    uint64_t *child_pml4 = address_space_clone(parent->pml4);
    if (!child_pml4) return err(OSNOS_ENOMEM);

    /* 2. Alloc child kstack. */
    void *child_kstack = kmalloc(USER_KSTACK_BYTES);
    if (!child_kstack) {
        address_space_destroy(child_pml4);
        return err(OSNOS_ENOMEM);
    }

    /* 3. Reserve a fresh task slot. task_create starts the entry as
     *    user_task_trampoline (same as task_create_user_elf). */
    int child_pid_int = task_create(parent->name, parent->entry);
    if (child_pid_int < 0) {
        kfree(child_kstack);
        address_space_destroy(child_pml4);
        return err(OSNOS_EMFILE);
    }

    task_t *child = task_by_pid((uint64_t)child_pid_int);
    if (!child) {
        /* Shouldn't happen — task_create returned a valid pid. */
        kfree(child_kstack);
        address_space_destroy(child_pml4);
        return err(OSNOS_ESRCH);
    }

    /* 4. Wire up the AS + kstack. */
    child->pml4              = child_pml4;
    child->kernel_stack_top  = (uint64_t)child_kstack + USER_KSTACK_BYTES;
    child->kernel_stack_base = child_kstack;

    /* fork(2) parent-child link — enables wait(2) on the parent. */
    child->parent_pid        = parent->pid;
    /* POSIX: fork inherits the parent's process group + session.
     * execve preserves them (we don't touch pgid/sid in
     * proc_execve_replace). setsid() / setpgid() are the explicit
     * mutators. Overrides the default `pgid=pid; sid=pid;` that
     * task_create_user_elf seeded. */
    child->pgid              = parent->pgid;
    child->sid               = parent->sid;
    /* Child starts NOT waiting and with no inherited signal state.
     * POSIX says fd table inherited (done above) + sigactions
     * inherited (preserved across fork; CLOSED on execve). */
    child->waiting_for_pid   = 0;
    child->wait_options      = 0;
    child->wait_status_ptr   = 0;
    /* Inherit sa_handler[] / sa_restorer[] from parent — POSIX rule:
     * forked child keeps the parent's signal dispositions. */
    for (int i = 0; i < 32; i++) {
        child->sa_handler [i] = parent->sa_handler [i];
        child->sa_restorer[i] = parent->sa_restorer[i];
    }
    child->sig_pending = 0;     /* signals not inherited (POSIX) */

    /* 5. Clone fd table — POSIX-strict via OFD layer.
     *
     *    task_create_user_elf already ran fd_init_for_task on the
     *    child, allocating fresh stdin/stdout/stderr OFDs. We're
     *    about to overwrite those slots with the parent's references,
     *    so we must release the just-allocated child OFDs first to
     *    avoid leaking them.
     *
     *    After that, for every used slot in the parent: copy the
     *    slot (used + ofd_idx + fd_flags) AS-IS into the child, then
     *    bump the OFD's refcount. Parent and child end up pointing at
     *    the SAME ofd_idx — POSIX-correct: they share file offset,
     *    file flags, and pipe role. When one closes, ofd_unref
     *    decrements; when both have closed, the OFD's backend is
     *    released. No pipe_dup_reader/writer needed — the OFD counts
     *    as ONE reader/writer of the pipe regardless of how many fd
     *    slots reference it. */
    for (int fd = 0; fd < OSNOS_MAX_FDS; fd++) {
        /* Drop whatever the child's task_create wired up. */
        if (child->fds[fd].used) {
            int old = child->fds[fd].ofd_idx;
            child->fds[fd].used     = false;
            child->fds[fd].ofd_idx  = -1;
            child->fds[fd].fd_flags = 0;
            if (old >= 0) ofd_unref(old);
        }
        /* Inherit from parent. */
        if (!parent->fds[fd].used) continue;
        child->fds[fd] = parent->fds[fd];       /* slot struct copy */
        if (parent->fds[fd].ofd_idx >= 0) {
            ofd_ref(parent->fds[fd].ofd_idx);
        }
    }

    /* 6. Copy cwd, redirects, brk, mmap region table, FPU state.
     *    name was already set by task_create (= parent->name). */
    for (int i = 0; i < OSNOS_PATH_MAX; i++) child->cwd[i] = parent->cwd[i];
    for (int i = 0; i < OSNOS_PATH_MAX; i++) child->stdin_redir [i] = parent->stdin_redir [i];
    for (int i = 0; i < OSNOS_PATH_MAX; i++) child->stdout_redir[i] = parent->stdout_redir[i];
    child->stdout_append    = parent->stdout_append;
    child->stdin_redir_off  = parent->stdin_redir_off;
    child->stdout_redir_off = parent->stdout_redir_off;

    child->heap_start = parent->heap_start;
    child->heap_brk   = parent->heap_brk;
    child->user_entry = parent->user_entry;
    child->user_stack_top = parent->user_stack_top;

    child->mmap_next  = parent->mmap_next;
    for (int i = 0; i < TASK_MMAP_MAX; i++) {
        child->mmap_regions[i] = parent->mmap_regions[i];
    }
    for (int i = 0; i < (int)sizeof(child->fpu_state); i++) {
        child->fpu_state[i] = parent->fpu_state[i];
    }
    /* fork copy: child inherits parent's TLS pointer. Esto es POSIX-
     * correcto cuando el child no hace exec — la TLS vive en el
     * address space CLONADO (mismas direcciones), así que el FS_BASE
     * del parent sigue siendo válido en el child. SI el child hace
     * execve después, proc_execve resetea fs_base a 0 explícitamente
     * porque ahí el address space CAMBIA. */
    child->fs_base = parent->fs_base;

    /* Both kill_pending / stop_pending stay 0 in the child. Even if
     * the parent had them set (mid-Ctrl+C), fork should give the
     * child a clean slate so a forked grandchild can survive. */
    child->kill_pending = 0;
    child->stop_pending = 0;

    /* 7. Snapshot the syscall context from the parent's kstack into
     *    child->saved_*. Identical recipe to sys_nanosleep, but the
     *    payload goes to a *different* task. iret frame is at
     *    parent_kstack_top - 40, syscall_frame_t just below. */
    uint64_t *iret = (uint64_t *)(parent->kernel_stack_top - 40);
    child->saved_iret_rip    = iret[0];
    child->saved_iret_cs     = iret[1];
    child->saved_iret_rflags = iret[2];
    child->saved_iret_rsp    = iret[3];
    child->saved_iret_ss     = iret[4];

    syscall_frame_t *sf =
        (syscall_frame_t *)(parent->kernel_stack_top - 40 - sizeof(*sf));
    child->saved_rax = 0;                /* fork() returns 0 in child */
    child->saved_rbx = sf->rbx;
    child->saved_rcx = sf->rcx;
    child->saved_rdx = sf->rdx;
    child->saved_rsi = sf->rsi;
    child->saved_rdi = sf->rdi;
    child->saved_rbp = sf->rbp;
    child->saved_r8  = sf->r8;
    child->saved_r9  = sf->r9;
    child->saved_r10 = sf->r10;
    child->saved_r11 = sf->r11;
    child->saved_r12 = sf->r12;
    child->saved_r13 = sf->r13;
    child->saved_r14 = sf->r14;
    child->saved_r15 = sf->r15;

    child->saved_valid = 1;
    child->state       = TASK_READY;

    /* 8. Parent returns the child's pid. The child wakes up via
     *    user_task_trampoline's saved_valid path with rax=0. Both
     *    execute the instruction right after the `syscall`. */
    return (int64_t)child->pid;
}

/* ------------------------------------------------------------------ */
/* sys_wait4 (#61) — Linux waitpid(2) / wait4(2). Blocks the caller   */
/* until one of its children transitions to TASK_ZOMBIE, then reaps. */
/* ------------------------------------------------------------------ */

#define WNOHANG     1
#define WUNTRACED   2
#define WCONTINUED  8

/* task_t.wait_change values. Keep in sync with comment in task.h. */
#define WAIT_NONE      0
#define WAIT_STOPPED   1
#define WAIT_CONTINUED 2

/* Encode an exit code into POSIX wait status word.
 *  - Normal exit:   (code & 0xff) << 8     WIFEXITED == 1
 *  - Signal kill:   sig & 0x7f             WIFSIGNALED == 1 (low 7 bits)
 *  - Stopped child: (sig << 8) | 0x7f      WIFSTOPPED == 1
 *  - Continued:     0xffff                 WIFCONTINUED == 1
 * exit_code conventions in osnos:
 *   < 128       — normal exit code from sys_exit (clamped to 0..255)
 *   128 + sig   — signal-terminated (e.g. 130 = SIGINT, 137 = SIGKILL).
 */
static int encode_wait_status(int exit_code) {
    if (exit_code >= 128 && exit_code <= 128 + 31) {
        int sig = exit_code - 128;
        return sig & 0x7f;                    /* WIFSIGNALED + WTERMSIG */
    }
    return (exit_code & 0xff) << 8;           /* WIFEXITED + WEXITSTATUS */
}

/* WIFSTOPPED status: low 8 bits = 0x7f, bits 8..15 = stop signal.
 * SIGSTOP=19 / SIGTSTP=20 / SIGTTIN=21 / SIGTTOU=22 are the typical
 * stoppers. osnos's Ctrl+Z path uses SIGSTOP semantically. */
static int encode_stopped_status(int sig) {
    return ((sig & 0xff) << 8) | 0x7f;
}

/* WIFCONTINUED status: Linux uses the magic 0xffff. */
#define WCONTINUED_STATUS 0xffff

/*
 * Walk the task table looking for a child of `parent` whose state
 * transitioned recently and is wait-reportable:
 *
 *   - TASK_ZOMBIE                      → always reportable (exit/signal)
 *   - TASK_STOPPED + WAIT_STOPPED      → reportable iff WUNTRACED
 *   - any state + WAIT_CONTINUED       → reportable iff WCONTINUED
 *
 * `out_have_any_child` tracks whether the parent has ANY waitable
 * descendant at all (used to return -ECHILD). DEAD / UNUSED slots
 * are skipped since they're already-consumed.
 *
 * Returns the matching task or NULL. Caller is responsible for
 * encoding status, transitioning the slot (ZOMBIE→DEAD for normal
 * reap; STOPPED stays STOPPED; clear wait_change after report).
 */
static task_t *find_waitable_child(uint64_t parent_pid,
                                    int64_t want_pid,
                                    int options,
                                    int *out_have_any_child) {
    *out_have_any_child = 0;
    for (size_t i = 0; i < MAX_TASKS; i++) {
        task_t *c = (task_t *)task_slot(i);
        if (!c) continue;
        if (c->state == TASK_UNUSED || c->state == TASK_DEAD) continue;
        if (c->parent_pid != parent_pid) continue;
        if (want_pid > 0 && (int64_t)c->pid != want_pid) continue;
        *out_have_any_child = 1;

        /* ZOMBIE: always wait-reportable. */
        if (c->state == TASK_ZOMBIE) return c;
        /* Stop transition not yet reported, parent asked for WUNTRACED. */
        if ((options & WUNTRACED) && c->wait_change == WAIT_STOPPED &&
            c->state == TASK_STOPPED) return c;
        /* Continue transition not yet reported. */
        if ((options & WCONTINUED) && c->wait_change == WAIT_CONTINUED)
            return c;
    }
    return 0;
}

/* Wake the parent (if any) currently blocked in wait4 with options
 * compatible with the kind of state change `t` just had. Used at
 * STOPPED / CONTINUED transitions to mirror what
 * proc_exit_current_user already does for ZOMBIE.
 *
 * Writes the encoded status into the parent's user *status pointer
 * (via vmm_lookup on the parent's pml4), populates saved_rax with
 * the child pid, flips parent state → READY. Does NOT clear
 * t->wait_change — caller decides when (a successful waitpid sweep
 * clears it; otherwise the change stays pending). */
void notify_parent_stop_continue(task_t *t) {
    if (!t || t->parent_pid == 0) return;
    task_t *parent = task_by_pid(t->parent_pid);
    if (!parent || !parent->pml4 || parent->state != TASK_BLOCKED) return;
    if (parent->waiting_for_pid != -1 &&
        parent->waiting_for_pid != (int)t->pid) return;

    /* Only wake if parent asked for the kind of change we have. */
    int opts = parent->wait_options;
    int change = t->wait_change;
    if (change == WAIT_STOPPED   && !(opts & WUNTRACED))  return;
    if (change == WAIT_CONTINUED && !(opts & WCONTINUED)) return;

    int status = (change == WAIT_STOPPED)
        ? encode_stopped_status(19 /* SIGSTOP */)
        : WCONTINUED_STATUS;

    if (parent->wait_status_ptr) {
        uint64_t va = (uint64_t)parent->wait_status_ptr;
        uint64_t phys = vmm_lookup(parent->pml4, va & ~0xFFFULL);
        if (phys) {
            int *kva = (int *)((phys & 0xFFFFFFFFFFFFF000ULL) +
                                (va & 0xFFFu) +
                                pmm_hhdm_offset());
            *kva = status;
        }
    }
    parent->saved_rax       = t->pid;
    parent->waiting_for_pid = 0;
    parent->wait_options    = 0;
    parent->wait_status_ptr = 0;
    parent->state           = TASK_READY;
    /* The change is now reported — clear it so a re-wait doesn't
     * double-report the same transition. */
    t->wait_change = WAIT_NONE;
}

int64_t sys_wait4(int64_t pid, int *u_status, int options, void *u_rusage) {
    (void)u_rusage;                           /* no rusage in osnos yet */

    task_t *t = task_current();
    if (!t || !t->pml4) return err(OSNOS_ESRCH);

    /* First sweep — synchronous reap if a waitable child is ready
     * (ZOMBIE, or STOPPED-transitioned with WUNTRACED, or CONTINUED
     * with WCONTINUED). */
    int have_any;
    task_t *child = find_waitable_child(t->pid, pid, options, &have_any);
    if (!have_any) return err(OSNOS_ECHILD);

    if (child) {
        int status;
        uint64_t reaped_pid = child->pid;
        if (child->state == TASK_ZOMBIE) {
            /* Normal reap path: exit / signal-kill. Encode and
             * transition ZOMBIE → DEAD so reaper recycles slot. */
            status = encode_wait_status(child->exit_code);
            child->state = TASK_DEAD;
        } else if (child->wait_change == WAIT_STOPPED) {
            /* WUNTRACED report: child stays in TASK_STOPPED. Clear
             * the pending-change flag so a re-wait doesn't report
             * the same transition again (POSIX: report each stop
             * exactly once). */
            status = encode_stopped_status(19 /* SIGSTOP */);
            child->wait_change = WAIT_NONE;
        } else {
            /* WAIT_CONTINUED: child is back in READY (or already
             * RUNNING). Just clear the flag and report. */
            status = WCONTINUED_STATUS;
            child->wait_change = WAIT_NONE;
        }
        if (u_status) {
            if (copy_to_user(u_status, &status, sizeof(status)) != OSNOS_OK) {
                return err(OSNOS_EFAULT);
            }
        }
        return (int64_t)reaped_pid;
    }

    /* No waitable child ready. WNOHANG → poll-style return-0. */
    if (options & WNOHANG) return 0;

    /*
     * Block. Same recipe as sys_nanosleep: snapshot the iret frame
     * + GPRs from our kernel stack, mark BLOCKED with the wait
     * parameters recorded so proc_exit_current_user can find us
     * when a matching child dies, then sched_resume_jump.
     *
     * The wake-up path (proc_exit_current_user) writes:
     *   - status into *u_status via copy_to_user equivalent
     *   - child pid into our saved_rax
     *   - state → READY
     * Then user_task_trampoline replays our iret frame and we
     * return to user with rax = child pid (positive).
     */
    uint64_t *iret = (uint64_t *)(t->kernel_stack_top - 40);
    t->saved_iret_rip    = iret[0];
    t->saved_iret_cs     = iret[1];
    t->saved_iret_rflags = iret[2];
    t->saved_iret_rsp    = iret[3];
    t->saved_iret_ss     = iret[4];

    syscall_frame_t *sf =
        (syscall_frame_t *)(t->kernel_stack_top - 40 - sizeof(*sf));
    /* saved_rax overwritten by waker (child's pid). Default to
     * -EINTR-equivalent in case we wake via signal (we'll re-check
     * sig_pending in user_task_resume; for now safer to default
     * 0 and let the wake-up path write the real value). */
    t->saved_rax = 0;
    t->saved_rbx = sf->rbx;
    t->saved_rcx = sf->rcx;
    t->saved_rdx = sf->rdx;
    t->saved_rsi = sf->rsi;
    t->saved_rdi = sf->rdi;
    t->saved_rbp = sf->rbp;
    t->saved_r8  = sf->r8;
    t->saved_r9  = sf->r9;
    t->saved_r10 = sf->r10;
    t->saved_r11 = sf->r11;
    t->saved_r12 = sf->r12;
    t->saved_r13 = sf->r13;
    t->saved_r14 = sf->r14;
    t->saved_r15 = sf->r15;

    t->saved_valid       = 1;
    t->waiting_for_pid   = (pid > 0) ? (int)pid : -1;
    t->wait_options      = options;
    t->wait_status_ptr   = u_status;
    t->state             = TASK_BLOCKED;

    sched_resume_jump();                      /* never returns */
}

/* ------------------------------------------------------------------ */
/* sys_rt_sigaction (#13) + sys_rt_sigprocmask (#14) + rt_sigreturn   */
/* (#15) — sa_handler-only POSIX signals.                              */
/* ------------------------------------------------------------------ */

/* Layout the libc sees (lib/libc/include/signal.h). MUST stay in
 * sync with that struct's field order. */
typedef struct {
    uint64_t sa_handler;           /* SIG_DFL=0, SIG_IGN=1, else fn */
    uint32_t sa_mask;              /* ignored today */
    uint32_t sa_flags;             /* ignored today */
    uint64_t sa_restorer;          /* trampoline epilogue (libc __sigtramp) */
} kuser_sigaction_t;

/* On-user-stack sigframe pushed by user_task_resume on signal
 * delivery and consumed by sys_rt_sigreturn. Layout chosen to match
 * the iret/GPR snapshot fields in task_t, so the kernel just memcpys
 * back into task->saved_* on return. */
typedef struct {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8,  r9,  r10, r11, r12, r13, r14, r15;
} sigframe_t;

int64_t sys_rt_sigaction(int signum, const void *u_act, void *u_oldact,
                          size_t sigsetsize) {
    (void)sigsetsize;
    task_t *t = task_current();
    if (!t || !t->pml4) return err(OSNOS_ESRCH);
    if (signum < 1 || signum > 31) return err(OSNOS_EINVAL);
    /* SIGKILL (9) and SIGSTOP (19) are POSIX-uncatchable. */
    if (signum == 9 || signum == 19) return err(OSNOS_EINVAL);

    /* Return old disposition first, if requested. */
    if (u_oldact) {
        kuser_sigaction_t old = { 0 };
        old.sa_handler  = t->sa_handler [signum - 1];
        old.sa_restorer = t->sa_restorer[signum - 1];
        if (copy_to_user(u_oldact, &old, sizeof(old)) != OSNOS_OK) {
            return err(OSNOS_EFAULT);
        }
    }

    /* Install new disposition. */
    if (u_act) {
        kuser_sigaction_t k;
        if (copy_from_user(&k, u_act, sizeof(k)) != OSNOS_OK) {
            return err(OSNOS_EFAULT);
        }
        t->sa_handler [signum - 1] = k.sa_handler;
        t->sa_restorer[signum - 1] = k.sa_restorer;
    }
    return 0;
}

int64_t sys_rt_sigprocmask(int how, const void *u_set, void *u_oldset,
                            size_t sigsetsize) {
    (void)how; (void)u_set; (void)u_oldset; (void)sigsetsize;
    /* No mask infrastructure yet — accept the call but do nothing.
     * Returning success keeps glibc programs happy; correctness for
     * signal-blocking races will land if/when SA_NOCLDSTOP etc are
     * implemented. */
    return 0;
}

int64_t sys_rt_sigreturn(void) {
    task_t *t = task_current();
    if (!t || !t->pml4 || !t->kernel_stack_top) return err(OSNOS_ESRCH);

    /* The trampoline does `syscall` with the user RSP pointing just
     * above the sigframe (the sa_restorer return slot is already
     * popped by `ret`). Read the iret frame on OUR kernel stack to
     * get that RSP value. */
    uint64_t *iret = (uint64_t *)(t->kernel_stack_top - 40);
    uint64_t user_rsp = iret[3];                  /* RSP at syscall entry */

    sigframe_t f;
    if (copy_from_user(&f, (const void *)user_rsp, sizeof(f)) != OSNOS_OK) {
        /* The trampoline's stack frame is corrupt — kill the task to
         * avoid resuming with garbage. */
        proc_exit_current_user(128 + 11 /* SIGSEGV-like */);
        __builtin_unreachable();
    }

    /* Restore the pre-signal user context into the saved_* slots.
     * user_task_resume will replay these on next dispatch. */
    t->saved_iret_rip    = f.rip;
    t->saved_iret_cs     = f.cs;
    t->saved_iret_rflags = f.rflags;
    t->saved_iret_rsp    = f.rsp;
    t->saved_iret_ss     = f.ss;
    t->saved_rax = f.rax; t->saved_rbx = f.rbx; t->saved_rcx = f.rcx;
    t->saved_rdx = f.rdx; t->saved_rsi = f.rsi; t->saved_rdi = f.rdi;
    t->saved_rbp = f.rbp;
    t->saved_r8  = f.r8;  t->saved_r9  = f.r9;  t->saved_r10 = f.r10;
    t->saved_r11 = f.r11; t->saved_r12 = f.r12; t->saved_r13 = f.r13;
    t->saved_r14 = f.r14; t->saved_r15 = f.r15;
    t->saved_valid = 1;
    t->state       = TASK_READY;

    sched_resume_jump();                           /* never returns */
}

/* ------------------------------------------------------------------ */
/* sys_execve (#59) — Linux execve(2). Replaces the current task's    */
/* user-mode image in place. Same pid, fds, cwd; new pml4 + entry.    */
/* ------------------------------------------------------------------ */

#define SYS_EXECVE_MAX_ARGV 32
#define SYS_EXECVE_ARGS_BUF 1024
#define SYS_EXECVE_ENVP_BUF 4096

int64_t sys_execve(const char *u_path,
                    char *const *u_argv,
                    char *const *u_envp) {
    task_t *t = task_current();
    if (!t || !t->pml4) return err(OSNOS_ESRCH);
    if (!u_path)        return err(OSNOS_EFAULT);

    /* Copy path. Use the string-aware variant so a short path near a
     * page boundary doesn't EFAULT on the read-past-NUL. */
    char kpath[OSNOS_PATH_MAX];
    if (copy_string_from_user(kpath, u_path, OSNOS_PATH_MAX) != OSNOS_OK) {
        return err(OSNOS_EFAULT);
    }

    /* Walk argv: build a single space-separated args string from
     * argv[1..N]. argv[0] is the program name — proc_execve_replace
     * derives that from the path basename anyway. */
    static char args_kbuf[SYS_EXECVE_ARGS_BUF];
    args_kbuf[0] = 0;
    size_t args_pos = 0;
    if (u_argv) {
        for (int i = 0; i < SYS_EXECVE_MAX_ARGV; i++) {
            char *p_user = 0;
            if (copy_from_user(&p_user, &u_argv[i], sizeof(p_user)) != OSNOS_OK) {
                return err(OSNOS_EFAULT);
            }
            if (!p_user) break;
            if (i == 0) continue;       /* skip argv[0] (program name) */

            /* Copy this arg string into a small staging buffer. Use
             * the string-aware copy so short strings near a page
             * boundary don't trigger spurious EFAULT. */
            char arg[128];
            if (copy_string_from_user(arg, p_user, sizeof(arg)) != OSNOS_OK) {
                return err(OSNOS_EFAULT);
            }

            size_t arg_len = 0;
            while (arg[arg_len]) arg_len++;
            if (args_pos > 0) {
                if (args_pos + 1 >= sizeof(args_kbuf)) return err(OSNOS_E2BIG);
                args_kbuf[args_pos++] = ' ';
            }
            if (args_pos + arg_len + 1 > sizeof(args_kbuf)) return err(OSNOS_E2BIG);
            for (size_t k = 0; k < arg_len; k++) args_kbuf[args_pos++] = arg[k];
            args_kbuf[args_pos] = 0;
        }
    }

    /* Walk envp: copy pointer + each string into a buffer, build a
     * NULL-terminated kernel array pointing into it. */
    static const char *envp_arr[SYS_EXECVE_MAX_ARGV + 1];
    static char        envp_buf[SYS_EXECVE_ENVP_BUF];
    int envc = 0;
    size_t envp_pos = 0;
    if (u_envp) {
        for (int i = 0; i < SYS_EXECVE_MAX_ARGV; i++) {
            char *p_user = 0;
            if (copy_from_user(&p_user, &u_envp[i], sizeof(p_user)) != OSNOS_OK) {
                return err(OSNOS_EFAULT);
            }
            if (!p_user) break;
            char ent[256];
            /* String-aware copy: stops at NUL, won't over-read into
             * unmapped memory past the last env string (common when
             * envp lives near the top of the user stack page). */
            if (copy_string_from_user(ent, p_user, sizeof(ent)) != OSNOS_OK) {
                return err(OSNOS_EFAULT);
            }
            size_t l = 0;
            while (ent[l]) l++;
            if (envp_pos + l + 1 > sizeof(envp_buf)) return err(OSNOS_E2BIG);
            envp_arr[envc] = envp_buf + envp_pos;
            for (size_t k = 0; k <= l; k++) envp_buf[envp_pos++] = ent[k];
            envc++;
        }
    }
    envp_arr[envc] = 0;

    /* All inputs in kernel memory — safe to tear down the user image
     * inside proc_execve_replace without losing our args/envp. On
     * success, that function never returns. */
    int64_t rc = proc_execve_replace(kpath, args_kbuf,
                                       envc > 0 ? envp_arr : 0);
    return rc;     /* only reached on failure */
}

/* ------------------------------------------------------------------ */
/* sys_spawn — ring-3 wrapper for proc_execve with optional fd        */
/* inheritance. Used by the future ring-3 shell (FASE 10.4) to set up */
/* pipelines + redirects before exec'ing children.                    */
/* ------------------------------------------------------------------ */

#define SYS_SPAWN_ENVP_MAX 32

int64_t sys_spawn(const char *path, const char *args,
                   const char *envp_flat,
                   int stdin_fd, int stdout_fd) {
    if (!path) return err(OSNOS_EFAULT);
    if (!args) args = "";

    /* Validate the caller's fds before spawning so a bad fd doesn't
     * leave a half-created child behind. */
    task_t *caller = task_current();
    if (!caller) return err(OSNOS_ESRCH);
    osnos_fd_t *src_in  = (stdin_fd  >= 0) ? fd_get(caller, stdin_fd)  : 0;
    osnos_fd_t *src_out = (stdout_fd >= 0) ? fd_get(caller, stdout_fd) : 0;
    if (stdin_fd  >= 0 && !src_in)  return err(OSNOS_EBADF);
    if (stdout_fd >= 0 && !src_out) return err(OSNOS_EBADF);

    /* Unpack the flat envp blob into a temporary pointer array that
     * proc_execve / build_argv_block can iterate. The strings stay
     * pointing into user memory; proc_execve copies them onto the
     * child's user stack so the caller is free to release the
     * buffer after return. */
    const char *envp_arr[SYS_SPAWN_ENVP_MAX + 1];
    int envp_n = 0;
    if (envp_flat) {
        const char *p = envp_flat;
        while (*p && envp_n < SYS_SPAWN_ENVP_MAX) {
            envp_arr[envp_n++] = p;
            while (*p) p++;
            p++;
        }
    }
    envp_arr[envp_n] = 0;

    int64_t pid = proc_execve(path, args, envp_n > 0 ? envp_arr : 0);
    if (pid < 0) return pid;

    /* Child was created in READY state but hasn't been dispatched
     * yet, so its fds[] still hold the fd_init_for_task defaults.
     * Move the requested slots in now (MOVE semantics — NOT a dup,
     * the resource transfers from caller to child).
     *
     * OFD layer makes this a transfer of (slot.ofd_idx + fd_flags):
     *   1. Free child's default OFD on that slot (decrement; the
     *      default is_special OFD just goes away).
     *   2. Copy caller's slot into child's.
     *   3. Clear caller's slot WITHOUT calling ofd_unref — we want
     *      ownership transferred, not dropped. The OFD's refcount
     *      stays the same (no net change). */
    task_t *child = task_by_pid((uint64_t)pid);
    if (!child) return pid;   /* defensive, shouldn't trigger */

    if (src_in) {
        if (child->fds[OSNOS_FD_STDIN].used) {
            int old = child->fds[OSNOS_FD_STDIN].ofd_idx;
            child->fds[OSNOS_FD_STDIN].used     = false;
            child->fds[OSNOS_FD_STDIN].ofd_idx  = -1;
            child->fds[OSNOS_FD_STDIN].fd_flags = 0;
            if (old >= 0) ofd_unref(old);
        }
        child->fds[OSNOS_FD_STDIN] = caller->fds[stdin_fd];   /* slot copy */
        caller->fds[stdin_fd].used     = false;
        caller->fds[stdin_fd].ofd_idx  = -1;
        caller->fds[stdin_fd].fd_flags = 0;
    }
    if (src_out) {
        if (child->fds[OSNOS_FD_STDOUT].used) {
            int old = child->fds[OSNOS_FD_STDOUT].ofd_idx;
            child->fds[OSNOS_FD_STDOUT].used     = false;
            child->fds[OSNOS_FD_STDOUT].ofd_idx  = -1;
            child->fds[OSNOS_FD_STDOUT].fd_flags = 0;
            if (old >= 0) ofd_unref(old);
        }
        child->fds[OSNOS_FD_STDOUT] = caller->fds[stdout_fd];
        caller->fds[stdout_fd].used     = false;
        caller->fds[stdout_fd].ofd_idx  = -1;
        caller->fds[stdout_fd].fd_flags = 0;
    }

    return pid;
}

/* ------------------------------------------------------------------ */
/* sys_taskinfo — read-only inspection of a task slot. Safe to expose */
/* to ring 3: copies a small struct out, hides kernel-internal fields */
/* like saved iret frames, kstacks, and pml4 pointers.                */
/* ------------------------------------------------------------------ */

int64_t sys_taskinfo(size_t idx, struct osnos_taskinfo *out) {
    if (!out) return err(OSNOS_EFAULT);

    const task_t *t = task_slot(idx);
    if (!t) return err(OSNOS_ENOENT);

    osnos_taskinfo_t info;
    for (size_t i = 0; i < sizeof(info); i++) ((char *)&info)[i] = 0;
    info.pid        = t->pid;
    info.is_user    = (t->pml4 != 0) ? 1 : 0;
    info.dispatches = t->dispatches;
    info.exit_code  = t->exit_code;
    /* Map kernel task_state_t (numeric values may diverge from the
     * ABI in the future — translate explicitly). */
    switch (t->state) {
        case TASK_UNUSED:  info.state = OSNOS_TASK_UNUSED;  break;
        case TASK_READY:   info.state = OSNOS_TASK_READY;   break;
        case TASK_RUNNING: info.state = OSNOS_TASK_RUNNING; break;
        case TASK_BLOCKED: info.state = OSNOS_TASK_BLOCKED; break;
        case TASK_STOPPED: info.state = OSNOS_TASK_STOPPED; break;
        case TASK_ZOMBIE:  info.state = OSNOS_TASK_ZOMBIE;  break;
        case TASK_DEAD:    info.state = OSNOS_TASK_DEAD;    break;
        default:           info.state = OSNOS_TASK_UNUSED;  break;
    }
    size_t n = 0;
    while (t->name[n] && n < OSNOS_TASKINFO_NAME_MAX - 1) {
        info.name[n] = t->name[n];
        n++;
    }
    info.name[n] = 0;

    if (copy_to_user(out, &info, sizeof(info)) != OSNOS_OK) {
        return err(OSNOS_EFAULT);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* sys_pipe — Linux pipe(2). Allocates a kernel pipe + two fds in the */
/* caller's table: [0] = read end, [1] = write end.                   */
/* ------------------------------------------------------------------ */

int64_t sys_pipe(int *pipefd) {
    if (!pipefd) return err(OSNOS_EFAULT);

    task_t *t = task_current();
    if (!t) return err(OSNOS_ESRCH);

    /* Grab a fresh pipe object from the kernel pool. */
    pipe_t *p = pipe_create();
    if (!p) return err(OSNOS_ENFILE);

    /* Reserve two fd slots in the caller's table. If the second alloc
     * fails after the first succeeded, roll back so the table stays
     * consistent + the pipe gets freed. */
    int rfd = fd_alloc(t);
    if (rfd < 0) {
        pipe_close_reader(p);
        pipe_close_writer(p);
        return err(OSNOS_EMFILE);
    }
    int wfd = fd_alloc(t);
    if (wfd < 0) {
        fd_free(t, rfd);
        pipe_close_reader(p);
        pipe_close_writer(p);
        return err(OSNOS_EMFILE);
    }

    osnos_fd_t *rf = fd_get(t, rfd);
    osnos_fd_t *wf = fd_get(t, wfd);
    rf->is_pipe   = true;
    rf->pipe_ref  = p;
    rf->pipe_side = 0;   /* read end */
    rf->flags     = O_RDONLY;
    wf->is_pipe   = true;
    wf->pipe_ref  = p;
    wf->pipe_side = 1;   /* write end */
    wf->flags     = O_WRONLY;

    /* Copy [rfd, wfd] back to user. */
    int kbuf[2] = { rfd, wfd };
    if (copy_to_user(pipefd, kbuf, sizeof(kbuf)) != OSNOS_OK) {
        fd_free(t, rfd);
        fd_free(t, wfd);
        pipe_close_reader(p);
        pipe_close_writer(p);
        return err(OSNOS_EFAULT);
    }
    return 0;
}

int64_t sys_fcntl(int fd, int cmd, int64_t arg) {
    osnos_fd_t *f = fd_get(task_current(), fd);
    if (!f) return err(OSNOS_EBADF);

    switch (cmd) {
    case OSNOS_F_DUPFD: {
        int r = fd_dup_min(task_current(), fd, (int)arg);
        if (r < 0) return err(OSNOS_EMFILE);
        return r;
    }
    case OSNOS_F_GETFD:
        /* FD_CLOEXEC lives in the per-slot fd_flags (POSIX: NOT
         * shared with dup'd copies — that's why it's not in OFD). */
        return (int64_t)fd_get_flags(task_current(), fd);
    case OSNOS_F_SETFD:
        /* Only FD_CLOEXEC (bit 0) is recognised; ignore other bits
         * to stay forward-compatible. */
        fd_set_flags(task_current(), fd,
                     (int)arg & OSNOS_FD_CLOEXEC);
        return 0;
    case OSNOS_F_GETFL:
        return (int64_t)f->flags;
    case OSNOS_F_SETFL: {
        /* Only O_APPEND + O_NONBLOCK are settable; rest stays. */
        int mutable_mask = OSNOS_O_APPEND | OSNOS_O_NONBLOCK;
        f->flags = (f->flags & ~mutable_mask) | ((int)arg & mutable_mask);
        return 0;
    }
    case 5:  /* F_GETLK — pretend lock is free */
    case 6:  /* F_SETLK — pretend we got the lock */
    case 7:  /* F_SETLKW — same */
    case 36: /* F_OFD_GETLK */
    case 37: /* F_OFD_SETLK */
    case 38: /* F_OFD_SETLKW */
        /* osnos es single-process effectivamente (multi-task pero sin
         * cross-process file sharing real); SQLite + cualquier app
         * que use POSIX advisory locks puede asumir success. Linux
         * permite F_SETLK retornar 0 sin tocar nada. */
        return 0;
    case 1030: /* F_DUPFD_CLOEXEC — like F_DUPFD pero set CLOEXEC */ {
        int r = fd_dup_min(task_current(), fd, (int)arg);
        if (r < 0) return err(OSNOS_EMFILE);
        fd_set_flags(task_current(), r, OSNOS_FD_CLOEXEC);
        return r;
    }
    default:
        return err(OSNOS_EINVAL);
    }
}

/*
 * Today the only ioctl device is the controlling TTY at fd 0/1/2.
 * Anything else returns -ENOTTY. termios I/O goes via copy_*_user so
 * a faulting pointer becomes EFAULT, not a kernel panic.
 */
/* Linux ioctl numbers for PTY master operations. */
#define TTY_TIOCGPTN   0x80045430u    /* get pts number */
#define TTY_TIOCSPTLCK 0x40045431u    /* lock pts (no-op for us) */

int64_t sys_ioctl(int fd, uint64_t request, void *arg) {
    osnos_fd_t *f = fd_get(task_current(), fd);
    if (!f) return err(OSNOS_EBADF);

    /* /dev/fb0 ioctls (FASE 12 — ox window system).
     * Detected by exact path match so we don't have to add a new
     * is_fb flag to the OFD struct. */
    if (f->is_chr && os_streq(f->path, "/dev/fb0")) {
        switch (request) {
        case OSNOS_FBIOGET_VSCREENINFO: {
            struct osnos_fb_var_screeninfo info = {0};
            uint32_t w, h, pitch_bytes, bpp;
            framebuffer_get_info(&w, &h, &pitch_bytes, &bpp);
            info.xres           = w;
            info.yres           = h;
            info.xres_virtual   = w;
            info.yres_virtual   = h;
            info.bits_per_pixel = bpp;
            info.line_length    = pitch_bytes;
            /* Limine on x86_64 lays out 32 bpp as BGRA: byte 0 = B,
             * byte 1 = G, byte 2 = R, byte 3 = A. */
            info.red_offset     = 16;
            info.green_offset   = 8;
            info.blue_offset    = 0;
            info.alpha_offset   = 24;
            if (copy_to_user(arg, &info, sizeof(info)) != OSNOS_OK)
                return err(OSNOS_EFAULT);
            return 0;
        }
        case OSNOS_FBIO_BLIT: {
            struct osnos_fb_blit_req req;
            if (copy_from_user(&req, arg, sizeof(req)) != OSNOS_OK)
                return err(OSNOS_EFAULT);
            if (req.w == 0 || req.h == 0) return 0;
            if (req.w > 4096 || req.h > 4096) return err(OSNOS_EINVAL);
            size_t row_bytes = (size_t)req.w * 4;
            void *scratch = kmalloc(row_bytes);
            if (!scratch) return err(OSNOS_ENOMEM);
            const uint8_t *src_p = (const uint8_t *)req.src;
            for (uint32_t row = 0; row < req.h; row++) {
                if (copy_from_user(scratch,
                                   src_p + (size_t)row * req.src_pitch,
                                   row_bytes) != OSNOS_OK) {
                    kfree(scratch);
                    return err(OSNOS_EFAULT);
                }
                framebuffer_blit_kernel(req.x, req.y + row,
                                         req.w, 1,
                                         scratch, row_bytes);
            }
            kfree(scratch);
            return 0;
        }
        default:
            return -(int64_t)OSNOS_ENOTTY;
        }
    }

    /* PTY ioctls — work on master OR slave fds opened via
     * /dev/ptmx and /dev/pts/N. The pair has its OWN termios so
     * TCGETS/TCSETS on a PTY don't touch the kernel TTY's state. */
    if (f->is_pty && f->pty_ref) {
        pty_pair_t *p = f->pty_ref;
        switch (request) {
        case TTY_TCGETS: {
            if (copy_to_user(arg, &p->termios,
                             sizeof(p->termios)) != OSNOS_OK)
                return err(OSNOS_EFAULT);
            return 0;
        }
        case TTY_TCSETS:
        case TTY_TCSETSW:
        case TTY_TCSETSF: {
            struct osnos_termios t;
            if (copy_from_user(&t, arg, sizeof(t)) != OSNOS_OK)
                return err(OSNOS_EFAULT);
            p->termios = t;
            return 0;
        }
        case TTY_TIOCGPTN: {
            /* Master ioctl: ptsname implementation calls this to
             * learn the pts index, then formats "/dev/pts/<N>". */
            if (f->pty_side != 0) return -(int64_t)OSNOS_ENOTTY;
            int n = p->index;
            if (copy_to_user(arg, &n, sizeof(n)) != OSNOS_OK)
                return err(OSNOS_EFAULT);
            return 0;
        }
        case TTY_TIOCSPTLCK: {
            /* No-op: glibc calls this to unlock the slave before
             * opening it. We don't implement locking. */
            return 0;
        }
        case TTY_TIOCGWINSZ: {
            /* PTY pair doesn't track winsize yet — return 0x0. A
             * future TIOCSWINSZ + SIGWINCH would belong here. */
            struct osnos_winsize ws = { 0, 0, 0, 0 };
            if (copy_to_user(arg, &ws, sizeof(ws)) != OSNOS_OK)
                return err(OSNOS_EFAULT);
            return 0;
        }
        default:
            return -(int64_t)OSNOS_ENOTTY;
        }
    }

    /* Only the special fds 0/1/2 (the TTY) accept these requests. */
    if (!f->is_special) return -(int64_t)OSNOS_ENOTTY;

    switch (request) {
    case TTY_TCGETS: {
        struct osnos_termios t;
        if (tty_get_termios(&t) != 0) return err(OSNOS_EIO);
        if (copy_to_user(arg, &t, sizeof(t)) != OSNOS_OK) return err(OSNOS_EFAULT);
        return 0;
    }
    case TTY_TCSETS:
    case TTY_TCSETSW: {
        struct osnos_termios t;
        if (copy_from_user(&t, arg, sizeof(t)) != OSNOS_OK) return err(OSNOS_EFAULT);
        tty_set_termios(&t);
        return 0;
    }
    case TTY_TCSETSF: {
        struct osnos_termios t;
        if (copy_from_user(&t, arg, sizeof(t)) != OSNOS_OK) return err(OSNOS_EFAULT);
        tty_set_termios(&t);
        tty_clear();      /* TCSETSF also drops pending input */
        return 0;
    }
    case TTY_TIOCGWINSZ: {
        struct osnos_winsize ws;
        ws.ws_row    = framebuffer_rows();
        ws.ws_col    = framebuffer_cols();
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        if (copy_to_user(arg, &ws, sizeof(ws)) != OSNOS_OK) return err(OSNOS_EFAULT);
        return 0;
    }
    default:
        return -(int64_t)OSNOS_ENOTTY;
    }
}

/* ------------------------------------------------------------------ */
/* sys_poll — Linux poll(2). BusyBox ash, sshd, screen, many POSIX     */
/* programs use poll instead of select.                                 */
/* ------------------------------------------------------------------ */

/* poll event bits (Linux <poll.h>). */
#define POLL_POLLIN    0x0001
#define POLL_POLLPRI   0x0002
#define POLL_POLLOUT   0x0004
#define POLL_POLLERR   0x0008
#define POLL_POLLHUP   0x0010
#define POLL_POLLNVAL  0x0020

struct osnos_pollfd {
    int   fd;
    short events;
    short revents;
};

int64_t sys_poll(void *u_fds, uint64_t nfds, int timeout_ms) {
    /* Empty array — sleep timeout (or return 0 if non-blocking). */
    if (nfds == 0) {
        if (timeout_ms > 0) {
            osnos_timespec_t ts = {
                .tv_sec  = timeout_ms / 1000,
                .tv_nsec = (int64_t)(timeout_ms % 1000) * 1000000
            };
            sys_nanosleep(&ts, 0);
        }
        return 0;
    }
    if (nfds > 1024) return err(OSNOS_EINVAL);
    /* Static buffer = 8 KiB; fine for kernel BSS. */
    static struct osnos_pollfd kfds[1024];
    size_t bytes = (size_t)nfds * sizeof(struct osnos_pollfd);
    if (copy_from_user(kfds, u_fds, bytes) != OSNOS_OK)
        return err(OSNOS_EFAULT);

    uint64_t now = timer_ms();
    uint64_t deadline = (timeout_ms > 0) ? now + (uint64_t)timeout_ms : 0;

    for (;;) {
        int ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            kfds[i].revents = 0;
            if (kfds[i].fd < 0) continue;
            osnos_fd_t *f = fd_get(task_current(), kfds[i].fd);
            if (!f) {
                kfds[i].revents = POLL_POLLNVAL;
                ready++;
                continue;
            }
            if ((kfds[i].events & POLL_POLLIN) && fd_readable(kfds[i].fd)) {
                kfds[i].revents |= POLL_POLLIN;
            }
            if (kfds[i].events & POLL_POLLOUT) {
                /* Conservative: assume writable. Pipes/sockets/PTYs that
                 * are full would still EAGAIN on actual write, but
                 * poll-then-write loops accept that. */
                kfds[i].revents |= POLL_POLLOUT;
            }
            if (kfds[i].revents) ready++;
        }

        if (ready > 0) {
            if (copy_to_user(u_fds, kfds, bytes) != OSNOS_OK)
                return err(OSNOS_EFAULT);
            return ready;
        }

        if (timeout_ms == 0) {
            /* Non-blocking poll — return 0 immediately. */
            if (copy_to_user(u_fds, kfds, bytes) != OSNOS_OK)
                return err(OSNOS_EFAULT);
            return 0;
        }

        if (timeout_ms > 0 && timer_ms() >= deadline) {
            if (copy_to_user(u_fds, kfds, bytes) != OSNOS_OK)
                return err(OSNOS_EFAULT);
            return 0;
        }

        /* Block via restart_syscall — when the task wakes (after the
         * short timeout or sooner via signal) the CPU re-issues the
         * same poll() and we re-check readiness. Cannot call
         * sys_nanosleep here: it longjumps to the scheduler and the
         * task resumes in user-space with rax=0, making poll appear
         * to time out spuriously (ash interprets 0 with timeout=-1
         * as "stdin closed" and exits). */
        uint64_t wake = timer_ms() + 5;
        if (block_restart_syscall(wake, SYS_POLL) != 0) {
            if (copy_to_user(u_fds, kfds, bytes) != OSNOS_OK)
                return err(OSNOS_EFAULT);
            return 0;
        }
    }
}

int64_t sys_select(int nfds,
                    void *readfds, void *writefds, void *exceptfds,
                    const void *timeout) {
    if (nfds < 0) return err(OSNOS_EINVAL);
    if (nfds > OSNOS_MAX_FDS) nfds = OSNOS_MAX_FDS;

    /*
     * Compute deadline. NULL timeout = wait forever (cap at INT_MAX
     * via a long-but-finite poll). { 0, 0 } = single poll (deadline =
     * now, loop runs once).
     */
    uint64_t now = timer_ms();
    uint64_t deadline;
    bool     wait_forever = false;
    if (!timeout) {
        wait_forever = true;
        deadline = now;
    } else {
        const uint8_t *tv = (const uint8_t *)timeout;
        uint64_t sec  = *(const uint64_t *)(tv + 0);
        uint64_t usec = *(const uint64_t *)(tv + 8);
        uint64_t ms = sec * 1000 + usec / 1000;
        deadline = now + ms;
    }

    /* Snapshot the input bitmaps so we can re-evaluate each iteration. */
    uint64_t in_read[FDSET_NWORDS]  = {0};
    uint64_t in_write[FDSET_NWORDS] = {0};
    uint64_t in_except[FDSET_NWORDS]= {0};
    if (readfds) {
        const uint64_t *p = (const uint64_t *)readfds;
        for (int i = 0; i < FDSET_NWORDS; i++) in_read[i] = p[i];
    }
    if (writefds) {
        const uint64_t *p = (const uint64_t *)writefds;
        for (int i = 0; i < FDSET_NWORDS; i++) in_write[i] = p[i];
    }
    if (exceptfds) {
        const uint64_t *p = (const uint64_t *)exceptfds;
        for (int i = 0; i < FDSET_NWORDS; i++) in_except[i] = p[i];
    }

    uint64_t out_read[FDSET_NWORDS];
    uint64_t out_write[FDSET_NWORDS];
    uint64_t out_except[FDSET_NWORDS];

    /*
     * Non-blocking single-pass. The libc wrapper is the one that loops
     * with nanosleep between polls — that way cooperative servers
     * (keyboard, shell) get to run and a user Ctrl+C can actually
     * reach the foreground task.
     *
     * The `timeout` arg is mostly informational here; we always do one
     * poll regardless. The wrapper enforces deadline.
     */
    (void)wait_forever;
    (void)deadline;
    (void)now;

    for (int i = 0; i < FDSET_NWORDS; i++) {
        out_read[i] = 0;
        out_write[i] = 0;
        out_except[i] = 0;
    }

    int count = 0;
    for (int fd = 0; fd < nfds; fd++) {
        int word = fd >> 6;
        uint64_t bit = 1ULL << (fd & 63);

        if ((in_read[word] & bit) && fd_readable(fd)) {
            out_read[word] |= bit;
            count++;
        }
        if (in_write[word] & bit) {
            osnos_fd_t *f = fd_get(task_current(), fd);
            if (f) { out_write[word] |= bit; count++; }
        }
        (void)in_except;
    }

    if (readfds) {
        uint64_t *p = (uint64_t *)readfds;
        for (int i = 0; i < FDSET_NWORDS; i++) p[i] = out_read[i];
    }
    if (writefds) {
        uint64_t *p = (uint64_t *)writefds;
        for (int i = 0; i < FDSET_NWORDS; i++) p[i] = out_write[i];
    }
    if (exceptfds) {
        uint64_t *p = (uint64_t *)exceptfds;
        for (int i = 0; i < FDSET_NWORDS; i++) p[i] = out_except[i];
    }
    return (int64_t)count;
}

int64_t sys_accept(int fd, void *addr, void *addrlen_ptr) {
    osnos_fd_t *f = fd_get(task_current(), fd);
    if (!f || !f->is_socket) return err(OSNOS_EBADF);

    uint32_t peer_ip = 0;
    uint16_t peer_port = 0;
    /* Non-blocking single-shot. -2 = nothing in accept queue → EAGAIN.
     * libc retries with nanosleep so other tasks run between polls. */
    int child_sd = sock_accept(f->sock_idx, &peer_ip, &peer_port, 0);
    if (child_sd == -2) return err(OSNOS_EAGAIN);
    if (child_sd < 0)   return err(OSNOS_EBADF);

    int new_fd = fd_alloc(task_current());
    if (new_fd < 0) {
        sock_close(child_sd);
        return err(OSNOS_EMFILE);
    }
    osnos_fd_t *nf = fd_get(task_current(), new_fd);
    nf->is_socket = true;
    nf->sock_idx  = child_sd;

    if (addr && addrlen_ptr) {
        uint32_t *alenp = (uint32_t *)addrlen_ptr;
        if (*alenp >= 16) {
            pack_sockaddr_in(addr, peer_ip, peer_port);
            *alenp = 16;
        }
    }
    return (int64_t)new_fd;
}

int64_t sys_sendto(int fd, const void *buf, size_t len, int flags,
                    const void *dst_addr, uint32_t addrlen) {
    (void)flags;
    osnos_fd_t *f = fd_get(task_current(), fd);
    if (!f || !f->is_socket) return err(OSNOS_EBADF);
    if (!buf && len > 0)     return err(OSNOS_EFAULT);

    /* On a stream socket dst_addr is ignored — connection is already
     * pinned by accept/connect. Lets libc send() forward verbatim. */
    if (dst_addr == NULL || addrlen == 0) {
        int n = sock_send(f->sock_idx, buf, len);
        if (n < 0) {
            /* Distinguish "fd lost its socket" (EBADF) from "connection
             * went away while the user task held a live fd" (ECONNRESET)
             * — both signal "stop using this socket" but the second is
             * Linux's expected errno when send after RST. */
            int st = sock_tcp_state_int(f->sock_idx);
            if (st == TCP_CLOSED) return err(OSNOS_ECONNRESET);
            return err(OSNOS_EBADF);
        }
        return (int64_t)n;
    }

    uint32_t ip = 0;
    uint16_t port = 0;
    int64_t e;
    if (!unpack_sockaddr_in(dst_addr, addrlen, &ip, &port, &e)) return e;

    int n = sock_sendto(f->sock_idx, buf, len, ip, port);
    if (n < 0) return err(OSNOS_EIO);
    return (int64_t)n;
}

int64_t sys_recvfrom(int fd, void *buf, size_t len, int flags,
                      void *src_addr, void *addrlen_ptr) {
    (void)flags;
    osnos_fd_t *f = fd_get(task_current(), fd);
    if (!f || !f->is_socket) return err(OSNOS_EBADF);
    if (!buf && len > 0)     return err(OSNOS_EFAULT);

    /* Stream sockets ignore src_addr (use getpeername). Non-blocking
     * single-shot; libc loops with nanosleep on EAGAIN. */
    int n = sock_recv(f->sock_idx, buf, len, 0);
    if (n == -2) return err(OSNOS_EAGAIN);
    if (n >= 0) {
        if (src_addr && addrlen_ptr) {
            uint32_t *alenp = (uint32_t *)addrlen_ptr;
            *alenp = 0;
        }
        return (int64_t)n;
    }
    /* sock_recv returned -1 → not a stream socket; try datagram path. */

    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    n = sock_recvfrom(f->sock_idx, buf, len, &src_ip, &src_port, 0);
    if (n == -2) return err(OSNOS_EAGAIN);
    if (n < 0)   return err(OSNOS_EBADF);

    if (src_addr && addrlen_ptr) {
        uint32_t *alenp = (uint32_t *)addrlen_ptr;
        if (*alenp >= 16) {
            pack_sockaddr_in(src_addr, src_ip, src_port);
            *alenp = 16;
        }
    }
    return (int64_t)n;
}

static uint64_t pack(int64_t r) { return (uint64_t)r; }

uint64_t syscall_dispatch(syscall_frame_t *frame) {
    switch (frame->rax) {
        case SYS_OPEN:
            return pack(sys_open(
                (const char *)frame->rdi,
                (int)frame->rsi,
                (uint32_t)frame->rdx));
        case SYS_OPENAT: {
            /* musl opendir() / openat(). dirfd in rdi, path in rsi,
             * flags in rdx, mode in r10. We only support AT_FDCWD
             * (-100) — relative paths against the task cwd. Absolute
             * paths work for any dirfd because we ignore it. */
            int dirfd = (int)frame->rdi;
            const char *path = (const char *)frame->rsi;
            int flags = (int)frame->rdx;
            uint32_t mode = (uint32_t)frame->r10;
            if (!path) return pack(-(int64_t)OSNOS_EFAULT);
            /* Reject relative paths with a real dirfd — not yet
             * supported. Most coreutils only pass AT_FDCWD here. */
            if (path[0] != '/' && dirfd != -100 /* AT_FDCWD */)
                return pack(-(int64_t)OSNOS_ENOSYS);
            return pack(sys_open(path, flags, mode));
        }
        case SYS_LSTAT:
            /* osnos has no symbolic links — lstat is identical to stat. */
            return pack(sys_stat(
                (const char *)frame->rdi,
                (osnos_stat_t *)frame->rsi));
        case SYS_NEWFSTATAT: {
            /* musl `stat()` / `lstat()` on x86_64 → fstatat(AT_FDCWD,
             * path, buf, 0). dirfd in rdi, path in rsi, statbuf in
             * rdx, flags in r10. We support absolute paths and
             * AT_FDCWD; ignore the AT_SYMLINK_NOFOLLOW flag (no
             * symlinks in osnos). */
            int dirfd = (int)frame->rdi;
            const char *path = (const char *)frame->rsi;
            void *out = (void *)frame->rdx;
            if (!path || !out) return pack(-(int64_t)OSNOS_EFAULT);
            if (path[0] != '/' && dirfd != -100 /* AT_FDCWD */)
                return pack(-(int64_t)OSNOS_ENOSYS);
            return pack(sys_stat(path, out));
        }
        case SYS_CLOSE:
            return pack(sys_close((int)frame->rdi));
        case SYS_READ:
            return pack(sys_read(
                (int)frame->rdi,
                (void *)frame->rsi,
                (size_t)frame->rdx));
        case SYS_WRITE:
            return pack(sys_write(
                (int)frame->rdi,
                (const void *)frame->rsi,
                (size_t)frame->rdx));
        case SYS_LSEEK:
            return pack(sys_lseek(
                (int)frame->rdi,
                (int64_t)frame->rsi,
                (int)frame->rdx));
        case SYS_FSTAT:
            return pack(sys_fstat(
                (int)frame->rdi,
                (osnos_stat_t *)frame->rsi));
        case SYS_ISATTY:
            return pack(sys_isatty((int)frame->rdi));
        case SYS_EXIT:
            return pack(sys_exit((int)frame->rdi));
        case SYS_REBOOT:
            return pack(sys_reboot((uint32_t)frame->rdi));
        case SYS_BRK:
            return pack(sys_brk((uintptr_t)frame->rdi));
        case SYS_NANOSLEEP:
            return pack(sys_nanosleep(
                (const osnos_timespec_t *)frame->rdi,
                (osnos_timespec_t *)frame->rsi));
        case SYS_KILL:
            return pack(sys_kill(frame->rdi, (int)frame->rsi));
        case SYS_FORK:
            return pack(sys_fork());
        case SYS_SETPGID:
            return pack(sys_setpgid((uint64_t)frame->rdi, (uint64_t)frame->rsi));
        case SYS_GETPPID:
            return pack(sys_getppid());
        case SYS_GETPGRP:
            return pack(sys_getpgrp());
        case SYS_SETSID:
            return pack(sys_setsid());
        case SYS_GETPGID:
            return pack(sys_getpgid((uint64_t)frame->rdi));
        case SYS_GETSID:
            return pack(sys_getsid((uint64_t)frame->rdi));
        case SYS_WAIT4:
            return pack(sys_wait4(
                (int64_t)frame->rdi,
                (int *)frame->rsi,
                (int)frame->rdx,
                (void *)frame->r10));
        case SYS_RT_SIGACTION:
            return pack(sys_rt_sigaction(
                (int)frame->rdi,
                (const void *)frame->rsi,
                (void *)frame->rdx,
                (size_t)frame->r10));
        case SYS_RT_SIGPROCMASK:
            return pack(sys_rt_sigprocmask(
                (int)frame->rdi,
                (const void *)frame->rsi,
                (void *)frame->rdx,
                (size_t)frame->r10));
        case SYS_RT_SIGRETURN:
            return pack(sys_rt_sigreturn());
        case SYS_EXECVE:
            return pack(sys_execve(
                (const char *)frame->rdi,
                (char *const *)frame->rsi,
                (char *const *)frame->rdx));
        case SYS_GETPID:
            return pack(sys_getpid());
        case SYS_MKDIR:
            return pack(sys_mkdir((const char *)frame->rdi, (uint32_t)frame->rsi));
        case SYS_RMDIR:
            return pack(sys_rmdir((const char *)frame->rdi));
        case SYS_UNLINK:
            return pack(sys_unlink((const char *)frame->rdi));
        case SYS_RENAME:
            return pack(sys_rename(
                (const char *)frame->rdi,
                (const char *)frame->rsi));
        case SYS_GETDENTS:
            return pack(sys_getdents(
                (int)frame->rdi,
                (void *)frame->rsi,
                (size_t)frame->rdx));
        case SYS_SOCKET:
            return pack(sys_socket(
                (int)frame->rdi,
                (int)frame->rsi,
                (int)frame->rdx));
        case SYS_BIND:
            return pack(sys_bind(
                (int)frame->rdi,
                (const void *)frame->rsi,
                (uint32_t)frame->rdx));
        case SYS_LISTEN:
            return pack(sys_listen(
                (int)frame->rdi,
                (int)frame->rsi));
        case SYS_CONNECT:
            return pack(sys_connect(
                (int)frame->rdi,
                (const void *)frame->rsi,
                (uint32_t)frame->rdx));
        case SYS_SETSOCKOPT:
            return pack(sys_setsockopt(
                (int)frame->rdi,
                (int)frame->rsi,
                (int)frame->rdx,
                (const void *)frame->r10,
                (uint32_t)frame->r8));
        case SYS_SELECT:
            return pack(sys_select(
                (int)frame->rdi,
                (void *)frame->rsi,
                (void *)frame->rdx,
                (void *)frame->r10,
                (const void *)frame->r8));
        case SYS_IOCTL:
            return pack(sys_ioctl(
                (int)frame->rdi,
                (uint64_t)frame->rsi,
                (void *)frame->rdx));
        case SYS_GETCWD:
            return pack(sys_getcwd(
                (char *)frame->rdi,
                (size_t)frame->rsi));
        case SYS_CHDIR:
            return pack(sys_chdir(
                (const char *)frame->rdi));
        case SYS_STAT:
            return pack(sys_stat(
                (const char *)frame->rdi,
                (void *)frame->rsi));
        case SYS_ACCESS:
            return pack(sys_access(
                (const char *)frame->rdi,
                (int)frame->rsi));
        case SYS_TIME:
            return pack(sys_time(
                (int64_t *)frame->rdi));
        case SYS_CLOCK_GETTIME:
            return pack(sys_clock_gettime(
                (int)frame->rdi,
                (void *)frame->rsi));
        case SYS_GETTIMEOFDAY: {
            /* musl + sqlite usan esto. tv = {sec, usec}; tz ignorado.
             * Convertimos timer_ms a sec/usec. */
            struct { int64_t sec; int64_t usec; } tv;
            uint64_t ms = timer_ms();
            tv.sec  = (int64_t)(ms / 1000);
            tv.usec = (int64_t)((ms % 1000) * 1000);
            if (frame->rdi) {
                if (copy_to_user((void *)frame->rdi, &tv, sizeof(tv)) != OSNOS_OK)
                    return pack(-(int64_t)OSNOS_EFAULT);
            }
            return 0;
        }
        case SYS_FSYNC:
        case SYS_FDATASYNC: {
            /* osnos no tiene write-back cache (FAT16 escribe sync), así
             * que sync es no-op. Solo validamos que el fd existe. */
            int fd = (int)frame->rdi;
            osnos_fd_t *f = fd_get(task_current(), fd);
            if (!f) return pack(-(int64_t)OSNOS_EBADF);
            return 0;
        }
        case SYS_FTRUNCATE: {
            /* sqlite necesita truncar el journal file. fd → path, luego
             * vfs_truncate. */
            int fd = (int)frame->rdi;
            uint64_t len = (uint64_t)frame->rsi;
            osnos_fd_t *f = fd_get(task_current(), fd);
            if (!f) return pack(-(int64_t)OSNOS_EBADF);
            if (f->is_dir) return pack(-(int64_t)OSNOS_EISDIR);
            if (f->is_special || f->is_pipe || f->is_pty || f->is_socket || f->is_chr)
                return pack(-(int64_t)OSNOS_EINVAL);
            /* Truncate via vfs_write con buffer vacío + el size adecuado.
             * Si len == 0: vfs_write con "" + size=0 trunca. Para len > 0:
             * leemos los primeros len bytes y re-escribimos. */
            if (len == 0) {
                osnos_status_t s = vfs_write(f->path, "", 0);
                if (s != OSNOS_OK) return pack(-(int64_t)s);
                return 0;
            }
            /* Truncate-to-size > 0: read up to `len`, rewrite. Si el
             * archivo es más chico que len, hay que padding con zeros
             * (POSIX ftruncate). */
            extern void *kmalloc(size_t);
            extern void kfree(void *);
            uint64_t cap = len < (4 * 1024 * 1024) ? len : (4 * 1024 * 1024);
            char *buf = (char *)kmalloc((size_t)cap + 1);
            if (!buf) return pack(-(int64_t)OSNOS_ENOMEM);
            size_t got = 0;
            osnos_status_t s = vfs_read_at(f->path, 0, buf, (size_t)cap, &got);
            if (s != OSNOS_OK) { kfree(buf); return pack(-(int64_t)s); }
            /* Si needed > got, pad con zeros */
            if (len > got) {
                for (size_t i = got; i < (size_t)cap; i++) buf[i] = 0;
                got = (size_t)cap;
            } else if (len < got) {
                got = (size_t)len;
            }
            s = vfs_write(f->path, buf, got);
            kfree(buf);
            if (s != OSNOS_OK) return pack(-(int64_t)s);
            return 0;
        }
        case SYS_GETRANDOM: {
            /* musl + sqlite usan getrandom para entropy. flags ignorados.
             * Llenamos buffer con PRNG xorshift seeded por timer. No es
             * cryptographic-grade, pero sqlite solo lo usa para temp file
             * naming + sqlite_randomness(). */
            void *buf = (void *)frame->rdi;
            size_t len = (size_t)frame->rsi;
            /* flags = frame->rdx — ignored (GRND_NONBLOCK/GRND_RANDOM) */
            if (!buf && len > 0) return pack(-(int64_t)OSNOS_EFAULT);
            static uint64_t prng_state = 0;
            if (prng_state == 0) prng_state = timer_ms() | 1;
            char scratch[256];
            size_t total = 0;
            while (total < len) {
                size_t chunk = len - total;
                if (chunk > sizeof(scratch)) chunk = sizeof(scratch);
                for (size_t i = 0; i < chunk; i++) {
                    prng_state ^= prng_state << 13;
                    prng_state ^= prng_state >> 7;
                    prng_state ^= prng_state << 17;
                    scratch[i] = (char)(prng_state & 0xff);
                }
                if (copy_to_user((char *)buf + total, scratch, chunk) != OSNOS_OK)
                    return pack(-(int64_t)OSNOS_EFAULT);
                total += chunk;
            }
            return (int64_t)len;
        }
        case SYS_DUP:
            return pack(sys_dup((int)frame->rdi));
        case SYS_DUP2:
            return pack(sys_dup2(
                (int)frame->rdi, (int)frame->rsi));
        case SYS_PIPE:
            return pack(sys_pipe((int *)frame->rdi));
        case SYS_IPC_SEND:
            return pack(sys_ipc_send((const ipc_msg_t *)frame->rdi));
        case SYS_IPC_RECV:
            return pack(sys_ipc_recv(
                (ipc_msg_t *)frame->rdi, (int)frame->rsi));
        case SYS_SERVICE_REGISTER:
            return pack(sys_service_register((int)frame->rdi));
        case SYS_SERVICE_LOOKUP:
            return pack(sys_service_lookup((int)frame->rdi));
        case SYS_TTY_INPUT:
            return pack(sys_tty_input((int)frame->rdi));
        case SYS_SPAWN:
            return pack(sys_spawn(
                (const char *)frame->rdi,
                (const char *)frame->rsi,
                (const char *)frame->rdx,
                (int)frame->r10,
                (int)frame->r8));
        case SYS_SET_FG:
            return pack(sys_set_fg((uint64_t)frame->rdi));
        case SYS_RESUME:
            return pack(sys_resume((uint64_t)frame->rdi));
        case SYS_TASKINFO:
            return pack(sys_taskinfo(
                (size_t)frame->rdi,
                (struct osnos_taskinfo *)frame->rsi));
        case SYS_FCNTL:
            return pack(sys_fcntl(
                (int)frame->rdi,
                (int)frame->rsi,
                (int64_t)frame->rdx));
        case SYS_MMAP:
            return pack(sys_mmap(
                (void *)frame->rdi,
                (size_t)frame->rsi,
                (int)frame->rdx,
                (int)frame->r10,
                (int)frame->r8,
                (int64_t)frame->r9));
        case SYS_MUNMAP:
            return pack(sys_munmap(
                (void *)frame->rdi,
                (size_t)frame->rsi));
        case SYS_ACCEPT:
            return pack(sys_accept(
                (int)frame->rdi,
                (void *)frame->rsi,
                (void *)frame->rdx));
        case SYS_SENDTO:
            return pack(sys_sendto(
                (int)frame->rdi,
                (const void *)frame->rsi,
                (size_t)frame->rdx,
                (int)frame->r10,
                (const void *)frame->r8,
                (uint32_t)frame->r9));
        case SYS_RECVFROM:
            return pack(sys_recvfrom(
                (int)frame->rdi,
                (void *)frame->rsi,
                (size_t)frame->rdx,
                (int)frame->r10,
                (void *)frame->r8,
                (void *)frame->r9));
        case SYS_POLL:
            return pack(sys_poll(
                (void *)frame->rdi,
                (uint64_t)frame->rsi,
                (int)frame->rdx));
        case SYS_WRITEV: {
            /* Linux writev(fd, iovec[], iovcnt) — musl's stdio writes
             * via this path. Loop and reuse sys_write for each iov. */
            int    fd     = (int)frame->rdi;
            uint64_t uvec = frame->rsi;
            int    iovcnt = (int)frame->rdx;
            if (iovcnt <= 0) return 0;
            if (iovcnt > 16) return pack(-(int64_t)OSNOS_EINVAL);
            struct { uint64_t base; uint64_t len; } iov[16];
            if (copy_from_user(iov, (void *)uvec,
                                (size_t)iovcnt * sizeof(iov[0])) != OSNOS_OK)
                return pack(-(int64_t)OSNOS_EFAULT);
            int64_t total = 0;
            for (int i = 0; i < iovcnt; i++) {
                if (iov[i].len == 0) continue;
                int64_t w = sys_write(fd, (const void *)iov[i].base,
                                       (size_t)iov[i].len);
                if (w < 0) {
                    if (total > 0) return pack(total);
                    return pack(w);
                }
                total += w;
                if ((uint64_t)w < iov[i].len) break;
            }
            return pack(total);
        }
        case SYS_ARCH_PRCTL: {
            /* musl uses ARCH_SET_FS to install its TLS pointer.
             *   code 0x1002 = ARCH_SET_FS  → wrmsr MSR_FS_BASE = addr
             *   code 0x1003 = ARCH_GET_FS  → rdmsr to *addr (musl
             *                                rarely needs this)
             * Other codes return -EINVAL. */
            int      code = (int)frame->rdi;
            uint64_t addr = frame->rsi;
            if (code == 0x1002) {
                uint32_t lo = (uint32_t)addr;
                uint32_t hi = (uint32_t)(addr >> 32);
                __asm__ volatile (
                    "wrmsr"
                    :
                    : "c"((uint32_t)0xC0000100), "a"(lo), "d"(hi));
                return 0;
            }
            if (code == 0x1003) {
                uint32_t lo, hi;
                __asm__ volatile (
                    "rdmsr"
                    : "=a"(lo), "=d"(hi)
                    : "c"((uint32_t)0xC0000100));
                uint64_t v = ((uint64_t)hi << 32) | lo;
                if (copy_to_user((void *)addr, &v, sizeof(v)) != OSNOS_OK)
                    return pack(-(int64_t)OSNOS_EFAULT);
                return 0;
            }
            return pack(-(int64_t)OSNOS_EINVAL);
        }
        case SYS_SET_TID_ADDRESS: {
            /* musl calls this very early. We don't implement clear-
             * on-exit semantics — just return the caller's pid. */
            task_t *t = task_current();
            return pack(t ? (int64_t)t->pid : 1);
        }
        /* ---- BusyBox / POSIX userland stubs ---- */
        case SYS_GETUID:
        case SYS_GETGID:
        case SYS_GETEUID:
        case SYS_GETEGID:
            return 0;                        /* everyone is root        */
        case SYS_SETUID:
        case SYS_SETGID:
            return 0;                        /* accept silently         */
        case SYS_GETGROUPS:
            return 0;                        /* no supplementary groups */
        case SYS_SETGROUPS:
            return 0;                        /* accept silently         */
        case SYS_UMASK:
            return 022;                      /* always the standard mask*/
        case SYS_GETRLIMIT: {
            /* struct rlimit { uint64_t cur, max; } — both RLIM_INFINITY */
            uint64_t rl[2] = { (uint64_t)-1, (uint64_t)-1 };
            if (copy_to_user((void *)frame->rsi, rl, sizeof(rl)) != OSNOS_OK)
                return pack(-(int64_t)OSNOS_EFAULT);
            return 0;
        }
        case SYS_SETRLIMIT:
            return 0;                        /* accept silently         */
        case SYS_GETRUSAGE: {
            /* struct rusage is huge — zero it out (96 bytes is enough
             * for the {tv_sec,tv_usec} pair + counters BusyBox uses). */
            char zeros[144] = {0};
            if (copy_to_user((void *)frame->rsi, zeros, sizeof(zeros)) != OSNOS_OK)
                return pack(-(int64_t)OSNOS_EFAULT);
            return 0;
        }
        case SYS_TIMES: {
            /* struct tms { tms_utime, tms_stime, tms_cutime, tms_cstime } */
            uint64_t tms[4] = {0, 0, 0, 0};
            if (frame->rdi &&
                copy_to_user((void *)frame->rdi, tms, sizeof(tms)) != OSNOS_OK)
                return pack(-(int64_t)OSNOS_EFAULT);
            return (int64_t)(timer_ms() / 10);  /* clock ticks (100 Hz)  */
        }
        case SYS_SYSINFO: {
            char zeros[112] = {0};
            if (copy_to_user((void *)frame->rdi, zeros, sizeof(zeros)) != OSNOS_OK)
                return pack(-(int64_t)OSNOS_EFAULT);
            return 0;
        }
        case SYS_PRCTL:
            return 0;
        case SYS_UNAME: {
            /* struct utsname has 6 × 65-byte fields. */
            struct {
                char sysname[65];
                char nodename[65];
                char release[65];
                char version[65];
                char machine[65];
                char domainname[65];
            } un = {0};
            os_strlcpy(un.sysname,    "osnos",          65);
            os_strlcpy(un.nodename,   "osnos-vm",       65);
            os_strlcpy(un.release,    "0.13",           65);
            os_strlcpy(un.version,    "FASE13",         65);
            os_strlcpy(un.machine,    "x86_64",         65);
            os_strlcpy(un.domainname, "(none)",         65);
            if (copy_to_user((void *)frame->rdi, &un, sizeof(un)) != OSNOS_OK)
                return pack(-(int64_t)OSNOS_EFAULT);
            return 0;
        }
        case 231: /* SYS_EXIT_GROUP */
            return pack(sys_exit((int)frame->rdi));
        default:
            return pack(-(int64_t)OSNOS_ENOSYS);
    }
}
