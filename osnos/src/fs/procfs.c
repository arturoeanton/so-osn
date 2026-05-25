#include "procfs.h"

#include "../lib/string.h"
#include "../micro/pmm.h"
#include "../micro/task.h"
#include "../micro/timer.h"
#include "vfs.h"

#define PROCFS_BUF 1024

/* ------------------------------------------------------------------ */
/* Tiny format helpers (kernel no tiene snprintf; usamos os_strlcat   */
/* + os_format_u64 + un cursor).                                       */
/* ------------------------------------------------------------------ */

static void pf_str(char *out, size_t cap, const char *s) {
    os_strlcat(out, s, cap);
}

static void pf_u64(char *out, size_t cap, uint64_t v) {
    char num[24];
    os_format_u64(v, num, sizeof(num));
    os_strlcat(out, num, cap);
}

/* ------------------------------------------------------------------ */
/* Path parsing                                                        */
/* ------------------------------------------------------------------ */

static const char *strip_proc(const char *path) {
    if (!os_strstarts(path, "/proc")) return 0;
    if (path[5] == 0) return "";
    if (path[5] != '/') return 0;
    return path + 6;
}

static int parse_uint_prefix(const char *s, const char **rest) {
    if (!s || s[0] < '0' || s[0] > '9') { if (rest) *rest = s; return -1; }
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    if (rest) *rest = s;
    return v;
}

/* ------------------------------------------------------------------ */
/* Top-level generators                                                */
/* ------------------------------------------------------------------ */

static void gen_meminfo(char *out, size_t cap) {
    uint64_t total_pages = pmm_total_pages();
    uint64_t free_pages  = pmm_free_pages();
    uint64_t total_kib = (total_pages * PAGE_SIZE) / 1024;
    uint64_t free_kib  = (free_pages  * PAGE_SIZE) / 1024;
    uint64_t used_kib  = total_kib - free_kib;

    out[0] = 0;
    pf_str(out, cap, "MemTotal:       "); pf_u64(out, cap, total_kib); pf_str(out, cap, " kB\n");
    pf_str(out, cap, "MemFree:        "); pf_u64(out, cap, free_kib);  pf_str(out, cap, " kB\n");
    pf_str(out, cap, "MemAvailable:   "); pf_u64(out, cap, free_kib);  pf_str(out, cap, " kB\n");
    pf_str(out, cap, "Buffers:               0 kB\n");
    pf_str(out, cap, "Cached:                0 kB\n");
    pf_str(out, cap, "SwapCached:            0 kB\n");
    pf_str(out, cap, "Active:                0 kB\n");
    pf_str(out, cap, "Inactive:              0 kB\n");
    pf_str(out, cap, "SwapTotal:             0 kB\n");
    pf_str(out, cap, "SwapFree:              0 kB\n");
    pf_str(out, cap, "Slab:           "); pf_u64(out, cap, used_kib);  pf_str(out, cap, " kB\n");
}

static void gen_uptime(char *out, size_t cap) {
    uint64_t ms  = timer_ms();
    uint64_t sec = ms / 1000;
    uint64_t cs  = (ms % 1000) / 10;

    out[0] = 0;
    pf_u64(out, cap, sec);
    pf_str(out, cap, ".");
    if (cs < 10) pf_str(out, cap, "0");
    pf_u64(out, cap, cs);
    pf_str(out, cap, " ");
    pf_u64(out, cap, sec);
    pf_str(out, cap, ".");
    if (cs < 10) pf_str(out, cap, "0");
    pf_u64(out, cap, cs);
    pf_str(out, cap, "\n");
}

static void gen_loadavg(char *out, size_t cap) {
    int running = 0, total = 0;
    for (size_t i = 0; i < 16; i++) {
        const task_t *t = task_slot(i);
        if (!t) continue;
        total++;
        if (t->state == TASK_RUNNING || t->state == TASK_READY) running++;
    }
    out[0] = 0;
    pf_str(out, cap, "0.00 0.00 0.00 ");
    pf_u64(out, cap, (uint64_t)running);
    pf_str(out, cap, "/");
    pf_u64(out, cap, (uint64_t)total);
    pf_str(out, cap, " 1\n");
}

static void gen_cpuinfo(char *out, size_t cap) {
    os_strlcpy(out,
        "processor\t: 0\n"
        "vendor_id\t: osnos\n"
        "cpu family\t: 6\n"
        "model name\t: osnos x86_64\n"
        "cpu MHz\t\t: 0\n"
        "cache size\t: 0 KB\n"
        "flags\t\t: fpu sse sse2\n"
        "\n", cap);
}

static void gen_stat(char *out, size_t cap) {
    uint64_t up = timer_ms() / 10;   /* clock_ticks unit */

    out[0] = 0;
    pf_str(out, cap, "cpu  ");
    pf_u64(out, cap, up); pf_str(out, cap, " 0 ");
    pf_u64(out, cap, up); pf_str(out, cap, " 0 0 0 0 0 0 0\n");
    pf_str(out, cap, "cpu0 ");
    pf_u64(out, cap, up); pf_str(out, cap, " 0 ");
    pf_u64(out, cap, up); pf_str(out, cap, " 0 0 0 0 0 0 0\n");
    pf_str(out, cap, "intr 0\n");
    pf_str(out, cap, "ctxt 0\n");
    pf_str(out, cap, "btime 0\n");
    pf_str(out, cap, "processes 0\n");
    pf_str(out, cap, "procs_running 1\n");
    pf_str(out, cap, "procs_blocked 0\n");
}

static void gen_version(char *out, size_t cap) {
    os_strlcpy(out, "osnos 0.0.1 (microkernel x86_64) #1 SMP\n", cap);
}

/* ------------------------------------------------------------------ */
/* Per-pid generators                                                  */
/* ------------------------------------------------------------------ */

static const char *task_state_letter(task_state_t s) {
    switch (s) {
    case TASK_RUNNING: return "R";
    case TASK_READY:   return "R";
    case TASK_BLOCKED: return "S";
    case TASK_STOPPED: return "T";
    case TASK_ZOMBIE:  return "Z";
    case TASK_DEAD:    return "X";
    default:           return "?";
    }
}

static void gen_pid_comm(task_t *t, char *out, size_t cap) {
    out[0] = 0;
    pf_str(out, cap, t->name);
    pf_str(out, cap, "\n");
}

/* cmdline: NUL-separated. Hoy solo tenemos t->name como argv[0]. */
static void gen_pid_cmdline(task_t *t, char *out, size_t cap) {
    size_t n = 0;
    while (t->name[n] && n + 1 < cap) { out[n] = t->name[n]; n++; }
    if (n < cap) out[n++] = 0;
    if (n < cap) out[n] = 0;
}

static void gen_pid_stat(task_t *t, char *out, size_t cap) {
    /* Linux format (truncado a primeros campos relevantes; padding
     * con 0 para los que ps/top quizás leen). */
    out[0] = 0;
    pf_u64(out, cap, t->pid);
    pf_str(out, cap, " (");
    pf_str(out, cap, t->name);
    pf_str(out, cap, ") ");
    pf_str(out, cap, task_state_letter(t->state));
    pf_str(out, cap, " ");
    pf_u64(out, cap, t->parent_pid);
    pf_str(out, cap, " ");
    pf_u64(out, cap, t->pgid);
    pf_str(out, cap, " ");
    pf_u64(out, cap, t->sid);
    pf_str(out, cap, " 0 -1 0 0 0 0 0 0 0 0 0 0 1 0 0 "
                     "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
}

static void gen_pid_status(task_t *t, char *out, size_t cap) {
    out[0] = 0;
    pf_str(out, cap, "Name:\t");  pf_str(out, cap, t->name); pf_str(out, cap, "\n");
    pf_str(out, cap, "State:\t"); pf_str(out, cap, task_state_letter(t->state)); pf_str(out, cap, "\n");
    pf_str(out, cap, "Pid:\t");   pf_u64(out, cap, t->pid); pf_str(out, cap, "\n");
    pf_str(out, cap, "PPid:\t");  pf_u64(out, cap, t->parent_pid); pf_str(out, cap, "\n");
    pf_str(out, cap, "PGid:\t");  pf_u64(out, cap, t->pgid); pf_str(out, cap, "\n");
    pf_str(out, cap, "SId:\t");   pf_u64(out, cap, t->sid); pf_str(out, cap, "\n");
    pf_str(out, cap, "VmRSS:\t   1024 kB\n");
    pf_str(out, cap, "Threads:\t1\n");
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    void (*gen)(char *out, size_t cap);
} proc_top_entry_t;

static const proc_top_entry_t top_entries[] = {
    { "meminfo",  gen_meminfo  },
    { "uptime",   gen_uptime   },
    { "loadavg",  gen_loadavg  },
    { "cpuinfo",  gen_cpuinfo  },
    { "stat",     gen_stat     },
    { "version",  gen_version  },
};
#define TOP_COUNT (sizeof(top_entries) / sizeof(top_entries[0]))

typedef struct {
    const char *name;
    void (*gen)(task_t *t, char *out, size_t cap);
} proc_pid_entry_t;

static const proc_pid_entry_t pid_entries[] = {
    { "cmdline", gen_pid_cmdline },
    { "comm",    gen_pid_comm    },
    { "stat",    gen_pid_stat    },
    { "status",  gen_pid_status  },
};
#define PID_COUNT (sizeof(pid_entries) / sizeof(pid_entries[0]))

typedef enum { PROC_NONE, PROC_ROOT, PROC_TOP, PROC_PID_DIR, PROC_PID_FILE } proc_kind_t;

static proc_kind_t classify(const char *sub, int *out_pid,
                             const proc_top_entry_t **out_top,
                             const proc_pid_entry_t **out_pidf) {
    if (out_pid)  *out_pid  = -1;
    if (out_top)  *out_top  = 0;
    if (out_pidf) *out_pidf = 0;

    if (!sub) return PROC_NONE;
    if (sub[0] == 0) return PROC_ROOT;

    for (size_t i = 0; i < TOP_COUNT; i++) {
        if (os_streq(sub, top_entries[i].name)) {
            if (out_top) *out_top = &top_entries[i];
            return PROC_TOP;
        }
    }

    /* "self" → alias del task actual (sentinel pid=-2). */
    if (os_strstarts(sub, "self") && (sub[4] == 0 || sub[4] == '/')) {
        if (out_pid) *out_pid = -2;
        if (sub[4] == 0) return PROC_PID_DIR;
        for (size_t i = 0; i < PID_COUNT; i++) {
            if (os_streq(sub + 5, pid_entries[i].name)) {
                if (out_pidf) *out_pidf = &pid_entries[i];
                return PROC_PID_FILE;
            }
        }
        return PROC_NONE;
    }

    const char *rest;
    int pid = parse_uint_prefix(sub, &rest);
    if (pid < 0) return PROC_NONE;
    if (out_pid) *out_pid = pid;
    if (rest[0] == 0) return PROC_PID_DIR;
    if (rest[0] != '/') return PROC_NONE;
    /* Trailing-slash form "/proc/<pid>/" → también dir. busybox top
     * hace stat("/proc/PID/") con trailing slash; sin esta rama
     * obteníamos ENOENT y top reportaba "no process info in /proc". */
    if (rest[1] == 0) return PROC_PID_DIR;
    for (size_t i = 0; i < PID_COUNT; i++) {
        if (os_streq(rest + 1, pid_entries[i].name)) {
            if (out_pidf) *out_pidf = &pid_entries[i];
            return PROC_PID_FILE;
        }
    }
    return PROC_NONE;
}

static task_t *resolve_pid(int pid) {
    if (pid == -2) return task_current();
    if (pid <  0)  return 0;
    return task_by_pid((uint64_t)pid);
}

/* ------------------------------------------------------------------ */
/* VFS ops                                                             */
/* ------------------------------------------------------------------ */

static osnos_status_t procfs_stat(void *priv, const char *path, vfs_stat_t *out) {
    (void)priv;
    const char *sub = strip_proc(path);
    int pid;
    const proc_top_entry_t *top = 0;
    const proc_pid_entry_t *pidf = 0;
    proc_kind_t k = classify(sub, &pid, &top, &pidf);

    out->inode = 0;
    out->size  = 0;

    switch (k) {
    case PROC_ROOT:
        out->type = VFS_NODE_DIR;
        out->mode = 0555;
        return OSNOS_OK;
    case PROC_TOP: {
        char tmp[PROCFS_BUF];
        top->gen(tmp, sizeof(tmp));
        out->type = VFS_NODE_REG;
        out->mode = 0444;
        out->size = os_strlen(tmp);
        return OSNOS_OK;
    }
    case PROC_PID_DIR: {
        task_t *t = resolve_pid(pid);
        if (!t || t->state == TASK_UNUSED) return OSNOS_ENOENT;
        out->type = VFS_NODE_DIR;
        out->mode = 0555;
        return OSNOS_OK;
    }
    case PROC_PID_FILE: {
        task_t *t = resolve_pid(pid);
        if (!t || t->state == TASK_UNUSED) return OSNOS_ENOENT;
        char tmp[PROCFS_BUF];
        pidf->gen(t, tmp, sizeof(tmp));
        out->type = VFS_NODE_REG;
        out->mode = 0444;
        if (os_streq(pidf->name, "cmdline")) {
            size_t l = 0;
            while (t->name[l] && l < OSNOS_TASK_NAME_MAX) l++;
            out->size = l + 1;
        } else {
            out->size = os_strlen(tmp);
        }
        return OSNOS_OK;
    }
    default:
        return OSNOS_ENOENT;
    }
}

static osnos_status_t procfs_read(void *priv, const char *path, size_t off,
                                   char *buf, size_t buf_size, size_t *out_size) {
    (void)priv;
    const char *sub = strip_proc(path);
    int pid;
    const proc_top_entry_t *top = 0;
    const proc_pid_entry_t *pidf = 0;
    proc_kind_t k = classify(sub, &pid, &top, &pidf);

    char tmp[PROCFS_BUF];
    size_t len = 0;

    switch (k) {
    case PROC_TOP:
        top->gen(tmp, sizeof(tmp));
        len = os_strlen(tmp);
        break;
    case PROC_PID_FILE: {
        task_t *t = resolve_pid(pid);
        if (!t || t->state == TASK_UNUSED) return OSNOS_ENOENT;
        pidf->gen(t, tmp, sizeof(tmp));
        if (os_streq(pidf->name, "cmdline")) {
            size_t l = 0;
            while (t->name[l] && l < OSNOS_TASK_NAME_MAX) l++;
            len = l + 1;
        } else {
            len = os_strlen(tmp);
        }
        break;
    }
    case PROC_ROOT:
    case PROC_PID_DIR:
        return OSNOS_EISDIR;
    default:
        return OSNOS_ENOENT;
    }

    if (off >= len) { *out_size = 0; return OSNOS_OK; }
    size_t avail = len - off;
    size_t n = (avail > buf_size) ? buf_size : avail;
    for (size_t i = 0; i < n; i++) buf[i] = tmp[off + i];
    *out_size = n;
    return OSNOS_OK;
}

static osnos_status_t procfs_readdir(void *priv, const char *path, size_t cursor,
                                      vfs_dirent_t *out, size_t *next_cursor) {
    (void)priv;
    const char *sub = strip_proc(path);
    int pid;
    const proc_top_entry_t *top = 0;
    const proc_pid_entry_t *pidf = 0;
    proc_kind_t k = classify(sub, &pid, &top, &pidf);

    if (k == PROC_ROOT) {
        if (cursor < TOP_COUNT) {
            os_strlcpy(out->name, top_entries[cursor].name, OSNOS_NAME_MAX);
            out->type = VFS_NODE_REG;
            *next_cursor = cursor + 1;
            return OSNOS_OK;
        }
        /* Después de los top files, una entry por task vivo. */
        size_t start_idx = cursor - TOP_COUNT;
        for (size_t i = start_idx; i < 16; i++) {
            const task_t *t = task_slot(i);
            if (!t || t->state == TASK_UNUSED) continue;
            os_format_u64(t->pid, out->name, OSNOS_NAME_MAX);
            out->type = VFS_NODE_DIR;
            *next_cursor = TOP_COUNT + i + 1;
            return OSNOS_OK;
        }
        return OSNOS_ENOENT;
    }

    if (k == PROC_PID_DIR) {
        if (cursor >= PID_COUNT) return OSNOS_ENOENT;
        os_strlcpy(out->name, pid_entries[cursor].name, OSNOS_NAME_MAX);
        out->type = VFS_NODE_REG;
        *next_cursor = cursor + 1;
        return OSNOS_OK;
    }

    return OSNOS_ENOENT;
}

static osnos_status_t procfs_rofs_path(void *priv, const char *path) {
    (void)priv; (void)path;
    return OSNOS_EROFS;
}

static osnos_status_t procfs_write(void *priv, const char *path,
                                    const char *buf, size_t buf_size) {
    (void)priv; (void)path; (void)buf; (void)buf_size;
    return OSNOS_EROFS;
}

static osnos_status_t procfs_rename(void *priv, const char *s, const char *d) {
    (void)priv; (void)s; (void)d;
    return OSNOS_EROFS;
}

const vfs_ops_t procfs_vfs_ops = {
    .stat    = procfs_stat,
    .readdir = procfs_readdir,
    .read    = procfs_read,
    .write   = procfs_write,
    .append  = procfs_write,
    .mkdir   = procfs_rofs_path,
    .rmdir   = procfs_rofs_path,
    .unlink  = procfs_rofs_path,
    .rename  = procfs_rename,
};
