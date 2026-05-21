#include "syscall.h"

#include <stddef.h>

#include "../drivers/framebuffer.h"
#include "../fs/vfs.h"
#include "../include/osnos_dirent.h"
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

int64_t sys_open(const char *path, int flags, uint32_t mode) {
    (void)mode;  /* permission bits not enforced yet */

    if (!path) return err(OSNOS_EFAULT);

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
    if (f->is_special) return err(OSNOS_EBADF);
    if (f->is_socket && f->sock_idx >= 0) {
        sock_close(f->sock_idx);
    }
    if (f->is_pipe && f->pipe_ref) {
        if (f->pipe_side == 0) pipe_close_reader(f->pipe_ref);
        else                   pipe_close_writer(f->pipe_ref);
    }
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
            if (n == 0 && count > 0) return err(OSNOS_EAGAIN);
            return (int64_t)n;
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

    if (f->is_dir) return err(OSNOS_EISDIR);

    int access = f->flags & O_ACCMODE;
    if (access == O_WRONLY) return err(OSNOS_EBADF);

    /*
     * Read entire file via VFS into a stack scratch buffer, then copy
     * the requested slice from offset. Inefficient but correct; will
     * become offset-native when backends grow read_at(offset).
     */
    char tmp[1024];
    size_t got = 0;
    osnos_status_t s = vfs_read(f->path, tmp, sizeof(tmp), &got);
    if (s != OSNOS_OK) return err(s);

    /* Character devices are streams — each backend call produces a
     * fresh batch of bytes (one keyboard event, the current FB span,
     * etc.). Bypass the offset-based slicing so the second read
     * doesn't see "offset >= got" and report a spurious EOF. */
    if (f->is_chr) {
        size_t n = (count < got) ? count : got;
        char *out = (char *)buf;
        for (size_t i = 0; i < n; i++) out[i] = tmp[i];
        return (int64_t)n;
    }

    if (f->offset >= got) return 0;

    size_t remaining = got - f->offset;
    size_t n = (count < remaining) ? count : remaining;

    char *out = (char *)buf;
    for (size_t i = 0; i < n; i++) out[i] = tmp[f->offset + i];
    f->offset += n;
    return (int64_t)n;
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

int64_t sys_kill(uint64_t pid, int sig) {
    (void)sig;
    task_t *t = task_by_pid(pid);
    if (!t || !t->pml4) return -(int64_t)OSNOS_ESRCH;
    t->kill_pending = 1;

    /*
     * If the target is BLOCKED (e.g., inside a long nanosleep) or
     * STOPPED (after Ctrl+Z), it never makes it back to a kernel
     * return point that checks kill_pending — so the kill wouldn't
     * fire until the user manually resumes it (which they probably
     * won't, because they wanted it dead). Force-wake here so the
     * scheduler dispatches it one more tick, where user_task_
     * trampoline notices kill_pending and routes through
     * proc_exit_current_user(130).
     */
    if (t->state == TASK_BLOCKED || t->state == TASK_STOPPED) {
        t->wakeup_at_ms = 0;
        t->state        = TASK_READY;
    }
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

    /* Pull path into kernel scratch so a faulting user pointer
     * unwinds cleanly via the extable. */
    char kpath[OSNOS_PATH_MAX];
    if (copy_from_user(kpath, path, OSNOS_PATH_MAX) != OSNOS_OK) {
        return err(OSNOS_EFAULT);
    }
    kpath[OSNOS_PATH_MAX - 1] = 0;

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
     * Move the requested slots in now: copy the full osnos_fd_t,
     * clear the caller's slot WITHOUT calling pipe_close_*. The
     * resource (pipe end, socket, file) is unchanged — ownership
     * has just transferred from caller to child. */
    task_t *child = task_by_pid((uint64_t)pid);
    if (!child) return pid;   /* defensive, shouldn't trigger */

    if (src_in) {
        child->fds[OSNOS_FD_STDIN] = *src_in;
        osnos_fd_t *cf = &caller->fds[stdin_fd];
        cf->used      = false;
        cf->is_pipe   = false;
        cf->is_socket = false;
        cf->is_chr    = false;
        cf->is_dir    = false;
        cf->is_special= false;
        cf->pipe_ref  = 0;
        cf->sock_idx  = -1;
        cf->path[0]   = 0;
    }
    if (src_out) {
        child->fds[OSNOS_FD_STDOUT] = *src_out;
        osnos_fd_t *cf = &caller->fds[stdout_fd];
        cf->used      = false;
        cf->is_pipe   = false;
        cf->is_socket = false;
        cf->is_chr    = false;
        cf->is_dir    = false;
        cf->is_special= false;
        cf->pipe_ref  = 0;
        cf->sock_idx  = -1;
        cf->path[0]   = 0;
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
    /* Map kernel task_state_t (numeric values may diverge from the
     * ABI in the future — translate explicitly). */
    switch (t->state) {
        case TASK_UNUSED:  info.state = OSNOS_TASK_UNUSED;  break;
        case TASK_READY:   info.state = OSNOS_TASK_READY;   break;
        case TASK_RUNNING: info.state = OSNOS_TASK_RUNNING; break;
        case TASK_BLOCKED: info.state = OSNOS_TASK_BLOCKED; break;
        case TASK_STOPPED: info.state = OSNOS_TASK_STOPPED; break;
        case TASK_DEAD:    info.state = OSNOS_TASK_DEAD;    break;
        default:           info.state = OSNOS_TASK_UNUSED;  break;
    }
    if (t->name) {
        size_t n = 0;
        while (t->name[n] && n < OSNOS_TASKINFO_NAME_MAX - 1) {
            info.name[n] = t->name[n];
            n++;
        }
        info.name[n] = 0;
    }

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
        /* No FD_CLOEXEC support — always 0. */
        return 0;
    case OSNOS_F_SETFD:
        /* Accept any value; CLOEXEC has no observable effect. */
        return 0;
    case OSNOS_F_GETFL:
        return (int64_t)f->flags;
    case OSNOS_F_SETFL: {
        /* Only O_APPEND + O_NONBLOCK are settable; rest stays. */
        int mutable_mask = OSNOS_O_APPEND | OSNOS_O_NONBLOCK;
        f->flags = (f->flags & ~mutable_mask) | ((int)arg & mutable_mask);
        return 0;
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
int64_t sys_ioctl(int fd, uint64_t request, void *arg) {
    osnos_fd_t *f = fd_get(task_current(), fd);
    if (!f) return err(OSNOS_EBADF);
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
        case SYS_BRK:
            return pack(sys_brk((uintptr_t)frame->rdi));
        case SYS_NANOSLEEP:
            return pack(sys_nanosleep(
                (const osnos_timespec_t *)frame->rdi,
                (osnos_timespec_t *)frame->rsi));
        case SYS_KILL:
            return pack(sys_kill(frame->rdi, (int)frame->rsi));
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
        default:
            return pack(-(int64_t)OSNOS_EINVAL);
    }
}
