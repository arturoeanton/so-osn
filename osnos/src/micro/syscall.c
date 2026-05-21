#include "syscall.h"

#include <stddef.h>

#include "../drivers/framebuffer.h"
#include "../fs/vfs.h"
#include "../include/osnos_dirent.h"
#include "../include/osnos_fcntl.h"
#include "../include/osnos_status.h"
#include "../lib/string.h"
#include "../net/socket.h"
#include "../net/tcp.h"
#include "../proc/exec.h"
#include "fd.h"
#include "ipc.h"
#include "pmm.h"
#include "scheduler.h"
#include "task.h"
#include "timer.h"
#include "vmm.h"

void syscall_init(void) {
    fd_init();
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

    int fd = fd_alloc();
    if (fd < 0) return err(OSNOS_EMFILE);

    osnos_fd_t *f = fd_get(fd);
    os_strlcpy(f->path, path, OSNOS_PATH_MAX);
    f->flags  = flags;
    f->offset = 0;
    f->is_dir = is_dir;
    return fd;
}

/* ------------------------------------------------------------------ */
/* sys_close                                                          */
/* ------------------------------------------------------------------ */

int64_t sys_close(int fd) {
    osnos_fd_t *f = fd_get(fd);
    if (!f) return err(OSNOS_EBADF);
    if (f->is_special) return err(OSNOS_EBADF);
    if (f->is_socket && f->sock_idx >= 0) {
        sock_close(f->sock_idx);
    }
    fd_free(fd);
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
        return write_to_console((const char *)buf, count);
    }

    osnos_fd_t *f = fd_get(fd);
    if (!f || f->is_special) return err(OSNOS_EBADF);

    int access = f->flags & O_ACCMODE;
    if (access == O_RDONLY) return err(OSNOS_EBADF);

    /*
     * Today every write is append-from-current-position. Combined with
     * O_TRUNC at open time, this matches `fopen("w")` and `fopen("a")`
     * semantics. Random-access write (lseek + write into the middle)
     * is not supported — backends would need offset-aware ops.
     */
    osnos_status_t s = vfs_append(f->path, (const char *)buf, count);
    if (s != OSNOS_OK) return err(s);

    f->offset += count;
    return (int64_t)count;
}

/* ------------------------------------------------------------------ */
/* sys_read                                                           */
/* ------------------------------------------------------------------ */

int64_t sys_read(int fd, void *buf, size_t count) {
    if (!buf && count > 0) return err(OSNOS_EFAULT);

    if (fd == OSNOS_FD_STDIN) {
        /*
         * Non-blocking: returns 0 when no input is buffered. Userland
         * loops. Real Linux would block here; we can't without a
         * preemptive scheduler (FASE 9).
         */
        return (int64_t)stdin_pop((char *)buf, count);
    }
    if (fd == OSNOS_FD_STDOUT || fd == OSNOS_FD_STDERR) {
        return err(OSNOS_EBADF);
    }

    osnos_fd_t *f = fd_get(fd);
    if (!f || f->is_special) return err(OSNOS_EBADF);
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
    osnos_fd_t *f = fd_get(fd);
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

    osnos_fd_t *f = fd_get(fd);
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
    osnos_fd_t *f = fd_get(fd);
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
     * If the target is BLOCKED (e.g., inside a long nanosleep), it
     * never makes it back to a kernel return point that checks
     * kill_pending — so the kill wouldn't fire until the natural
     * wake-up. Force-wake here so the scheduler can dispatch it
     * one more tick, where user_task_trampoline notices the flag
     * and routes it through proc_exit_current_user(130).
     */
    if (t->state == TASK_BLOCKED) {
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

    osnos_fd_t *f = fd_get(fd);
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

    int fd = fd_alloc();
    if (fd < 0) {
        sock_close(sd);
        return err(OSNOS_EMFILE);
    }

    osnos_fd_t *f = fd_get(fd);
    f->is_socket = true;
    f->sock_idx  = sd;
    return (int64_t)fd;
}

int64_t sys_bind(int fd, const void *addr, uint32_t addrlen) {
    osnos_fd_t *f = fd_get(fd);
    if (!f || !f->is_socket) return err(OSNOS_EBADF);

    uint32_t ip = 0;
    uint16_t port = 0;
    int64_t e;
    if (!unpack_sockaddr_in(addr, addrlen, &ip, &port, &e)) return e;

    if (sock_bind(f->sock_idx, ip, port) != 0) return err(OSNOS_EADDRINUSE);
    return 0;
}

int64_t sys_listen(int fd, int backlog) {
    osnos_fd_t *f = fd_get(fd);
    if (!f || !f->is_socket) return err(OSNOS_EBADF);
    if (sock_listen(f->sock_idx, backlog) != 0) return err(OSNOS_EINVAL);
    return 0;
}

int64_t sys_connect(int fd, const void *addr, uint32_t addrlen) {
    osnos_fd_t *f = fd_get(fd);
    if (!f || !f->is_socket) return err(OSNOS_EBADF);

    uint32_t ip = 0;
    uint16_t port = 0;
    int64_t e;
    if (!unpack_sockaddr_in(addr, addrlen, &ip, &port, &e)) return e;

    /* 5-second handshake timeout — Linux default is minutes, but for
     * a hobby stack on QEMU localhost that's overkill. */
    if (sock_connect(f->sock_idx, ip, port, 5000) != 0) {
        return err(OSNOS_ECONNREFUSED);
    }
    return 0;
}

/*
 * setsockopt: only SO_REUSEADDR (level=SOL_SOCKET=1, optname=2) is a
 * no-op success — enough to let Beej-style servers run. Other options
 * report ENOSYS so the caller knows they're not implemented yet.
 */
int64_t sys_setsockopt(int fd, int level, int optname,
                        const void *optval, uint32_t optlen) {
    (void)optval; (void)optlen;
    osnos_fd_t *f = fd_get(fd);
    if (!f || !f->is_socket) return err(OSNOS_EBADF);
    /* SOL_SOCKET = 1, SO_REUSEADDR = 2 (Linux numbers, see libc). */
    if (level == 1 && optname == 2) return 0;
    return err(OSNOS_EINVAL);
}

/* ----- select(2) ----- */

#define FDSET_NWORDS  16          /* 1024 bits = 16 * uint64_t */

static bool fd_readable(int fd) {
    osnos_fd_t *f = fd_get(fd);
    if (!f) return false;
    if (f->is_special) {
        return (fd == OSNOS_FD_STDIN) ? stdin_readable() : false;
    }
    if (f->is_socket) return sock_readable(f->sock_idx);
    /* Regular files / dirs are always readable up to EOF. */
    return true;
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
            osnos_fd_t *f = fd_get(fd);
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
    osnos_fd_t *f = fd_get(fd);
    if (!f || !f->is_socket) return err(OSNOS_EBADF);

    uint32_t peer_ip = 0;
    uint16_t peer_port = 0;
    int child_sd = sock_accept(f->sock_idx, &peer_ip, &peer_port, 0x7FFFFFFFu);
    if (child_sd < 0) return err(OSNOS_EBADF);

    int new_fd = fd_alloc();
    if (new_fd < 0) {
        sock_close(child_sd);
        return err(OSNOS_EMFILE);
    }
    osnos_fd_t *nf = fd_get(new_fd);
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

/* Diagnostic for the httpd-multi-curl bug — capture fd state the
 * moment send fails up here, before sock_send even gets called. */
static int g_sendto_fail_fd        = -1;
static int g_sendto_fail_fd_used   = -1;
static int g_sendto_fail_is_socket = -1;
static int g_sendto_fail_sock_idx  = -2;
int sys_sendto_fail_fd       (void) { return g_sendto_fail_fd; }
int sys_sendto_fail_fd_used  (void) { return g_sendto_fail_fd_used; }
int sys_sendto_fail_is_socket(void) { return g_sendto_fail_is_socket; }
int sys_sendto_fail_sock_idx (void) { return g_sendto_fail_sock_idx; }

int64_t sys_sendto(int fd, const void *buf, size_t len, int flags,
                    const void *dst_addr, uint32_t addrlen) {
    (void)flags;
    osnos_fd_t *f = fd_get(fd);
    if (!f || !f->is_socket) {
        /* Grab whatever the slot looks like even if 'used' is false. */
        extern osnos_fd_t *fd_peek_raw(int fd);
        osnos_fd_t *raw = (fd >= 0 && fd < OSNOS_MAX_FDS) ? fd_peek_raw(fd) : 0;
        g_sendto_fail_fd        = fd;
        g_sendto_fail_fd_used   = raw ? (int)raw->used : -1;
        g_sendto_fail_is_socket = raw ? (int)raw->is_socket : -1;
        g_sendto_fail_sock_idx  = raw ? raw->sock_idx : -2;
        return err(OSNOS_EBADF);
    }
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
    osnos_fd_t *f = fd_get(fd);
    if (!f || !f->is_socket) return err(OSNOS_EBADF);
    if (!buf && len > 0)     return err(OSNOS_EFAULT);

    /* Stream sockets ignore src_addr (use getpeername). Easiest check:
     * try sock_recv first; if it works, the socket is TCP. If it errors
     * with -1, fall back to sock_recvfrom (datagram path). */
    int n = sock_recv(f->sock_idx, buf, len, 0x7FFFFFFFu);
    if (n != -1) {
        /* Stream path used. peer info via getpeername in the future. */
        if (n == -2) n = 0;     /* timeout — should never happen with INT_MAX */
        if (src_addr && addrlen_ptr) {
            uint32_t *alenp = (uint32_t *)addrlen_ptr;
            *alenp = 0;          /* not filled */
        }
        return (int64_t)n;
    }

    /* Datagram path. */
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    n = sock_recvfrom(f->sock_idx, buf, len, &src_ip, &src_port, 0x7FFFFFFFu);
    if (n < 0) return err(OSNOS_EBADF);

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
