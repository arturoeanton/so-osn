#include "devfs.h"

#include "../drivers/framebuffer.h"
#include "../drivers/keyboard.h"
#include "../drivers/serial.h"
#include "../include/osnos_status.h"
#include "../lib/string.h"
#include "../micro/tty.h"
#include "vfs.h"

/* ------------------------------------------------------------------ */
/* /dev/input0 event ring                                              */
/* ------------------------------------------------------------------ */

/*
 * Today the kernel keyboard_server already consumes raw events from
 * the PS/2 driver each tick (feeds the TTY + IPC_KEY_EVENT). The
 * /dev/input0 readers (e.g. /bin/inputtest, future ring-3 kbdsrv)
 * must NOT call keyboard_poll() directly — that would race with the
 * kernel server and lose events.
 *
 * Instead, keyboard_server pushes every event into this small ring
 * buffer via devfs_input_push(), and input0_read drains it. Once
 * FASE 10.2 moves the kbdsrv to ring 3, the kernel server goes away
 * and the ring becomes the SOLE consumer of keyboard_poll.
 */
#define DEVFS_INPUT_RING 32
static struct {
    keyboard_event_t buf[DEVFS_INPUT_RING];
    size_t head;
    size_t tail;
    size_t level;
} input_ring;

void devfs_input_push(keyboard_event_t ev) {
    if (input_ring.level >= DEVFS_INPUT_RING) {
        /* Drop oldest — match the TTY's overflow policy. */
        input_ring.tail = (input_ring.tail + 1) % DEVFS_INPUT_RING;
        input_ring.level--;
    }
    input_ring.buf[input_ring.head] = ev;
    input_ring.head = (input_ring.head + 1) % DEVFS_INPUT_RING;
    input_ring.level++;
}

static bool input_ring_pop(keyboard_event_t *out) {
    if (input_ring.level == 0) return false;
    *out = input_ring.buf[input_ring.tail];
    input_ring.tail = (input_ring.tail + 1) % DEVFS_INPUT_RING;
    input_ring.level--;
    return true;
}

/*
 * Each character device has its own read/write semantics. Stat / readdir /
 * the dir-level ops live in this file once; per-device behavior is in
 * the function pointers below.
 */
typedef osnos_status_t (*dev_read_fn)(char *buf, size_t buf_size, size_t *out);
typedef osnos_status_t (*dev_write_fn)(const char *buf, size_t buf_size);

typedef struct {
    const char  *name;
    dev_read_fn  read;
    dev_write_fn write;
} devfs_dev_t;

static osnos_status_t null_read(char *buf, size_t buf_size, size_t *out) {
    (void)buf; (void)buf_size;
    *out = 0;
    return OSNOS_OK;
}

static osnos_status_t null_write(const char *buf, size_t buf_size) {
    (void)buf; (void)buf_size;
    return OSNOS_OK;
}

static osnos_status_t zero_read(char *buf, size_t buf_size, size_t *out) {
    for (size_t i = 0; i < buf_size; i++) buf[i] = 0;
    *out = buf_size;
    return OSNOS_OK;
}

static osnos_status_t zero_write(const char *buf, size_t buf_size) {
    (void)buf; (void)buf_size;
    return OSNOS_OK;
}

/*
 * /dev/fb0 — write-only character device backed by the framebuffer.
 * Bytes pass through framebuffer_write_bytes which honours the same
 * CSI escape vocabulary as the kernel console. Reads return EOF.
 *
 * Used by the future ring-3 console_server (FASE 10.1) which reads
 * IPC_CONSOLE_WRITE messages and forwards them here via write(fb_fd).
 */
static osnos_status_t fb0_read(char *buf, size_t buf_size, size_t *out) {
    (void)buf; (void)buf_size;
    *out = 0;          /* EOF — framebuffer isn't a source. */
    return OSNOS_OK;
}

static osnos_status_t fb0_write(const char *buf, size_t buf_size) {
    framebuffer_write_bytes(buf, buf_size, 0xffffff);
    return OSNOS_OK;
}

/*
 * /dev/input0 — read-only character device exposing raw keyboard
 * events as `keyboard_event_t` structs (3 bytes: ascii + keycode16).
 *
 * Non-blocking: returns EAGAIN when the keyboard buffer is empty so
 * the libc loop yields via nanosleep. Used by the future ring-3
 * kbdsrv (FASE 10.2) which reads from here and forwards into the TTY
 * + the shell IPC.
 */
static osnos_status_t input0_read(char *buf, size_t buf_size, size_t *out) {
    keyboard_event_t ev;
    if (!input_ring_pop(&ev)) {
        *out = 0;
        return OSNOS_EAGAIN;
    }
    size_t n = sizeof(ev);
    if (n > buf_size) n = buf_size;
    const char *src = (const char *)&ev;
    for (size_t i = 0; i < n; i++) buf[i] = src[i];
    *out = n;
    return OSNOS_OK;
}

static osnos_status_t input0_write(const char *buf, size_t buf_size) {
    (void)buf; (void)buf_size;
    return OSNOS_EROFS;
}

/*
 * /dev/ttyS0 — raw access to the UART. write goes straight to
 * serial_puts (one byte at a time, no termios cooking). read polls
 * serial_try_getc once per byte requested and returns EAGAIN if the
 * UART has nothing ready. NOTE: if `serial_input_server` is running
 * (the default since FASE 10.7), the input feeder is racing for the
 * same RX bytes — explicit /dev/ttyS0 reads are best-effort.
 */
static osnos_status_t ttyS0_read(char *buf, size_t buf_size, size_t *out) {
    if (!buf || !out) return OSNOS_EFAULT;
    size_t n = 0;
    while (n < buf_size) {
        uint8_t b;
        if (!serial_try_getc(&b)) break;
        buf[n++] = (char)b;
    }
    *out = n;
    if (n == 0) return OSNOS_EAGAIN;
    return OSNOS_OK;
}

static osnos_status_t ttyS0_write(const char *buf, size_t buf_size) {
    if (!buf) return OSNOS_EFAULT;
    serial_puts(buf, buf_size);
    return OSNOS_OK;
}

/*
 * /dev/tty — the controlling terminal. open() on this path is
 * special-cased in sys_open (src/micro/syscall.c) to return an
 * is_special OFD that routes through the existing fd 0/1/2 paths
 * (stdin_pop / write_to_console / ioctl TCGETS-etc). These read/
 * write fallbacks here are for the rare case where someone reaches
 * /dev/tty via vfs_read/vfs_write (e.g. stat + read in tools).
 */
static osnos_status_t tty_dev_read(char *buf, size_t buf_size, size_t *out) {
    if (!buf || !out) return OSNOS_EFAULT;
    size_t n = tty_read(buf, buf_size);
    *out = n;
    return (n == 0) ? OSNOS_EAGAIN : OSNOS_OK;
}

static osnos_status_t tty_dev_write(const char *buf, size_t buf_size) {
    if (!buf) return OSNOS_EFAULT;
    /* Same dual-console tee as fd 1 default: framebuffer + serial
     * (framebuffer_write_bytes already mirrors to serial_puts since
     * FASE 10.7 Bloque A). */
    framebuffer_write_bytes(buf, buf_size, 0xffffff);
    return OSNOS_OK;
}

static const devfs_dev_t devices[] = {
    { "null",   null_read,    null_write    },
    { "zero",   zero_read,    zero_write    },
    { "fb0",    fb0_read,     fb0_write     },
    { "input0", input0_read,  input0_write  },
    { "ttyS0",  ttyS0_read,   ttyS0_write   },
    { "tty",    tty_dev_read, tty_dev_write }
};

#define DEVFS_DEV_COUNT (sizeof(devices) / sizeof(devices[0]))

static const char *entry_name(const char *path) {
    if (!os_strstarts(path, "/dev")) return 0;
    if (path[4] == 0) return "";
    if (path[4] != '/') return 0;
    return path + 5;
}

static const devfs_dev_t *lookup_dev(const char *name) {
    for (size_t i = 0; i < DEVFS_DEV_COUNT; i++) {
        if (os_streq(devices[i].name, name)) return &devices[i];
    }
    return 0;
}

static osnos_status_t devfs_stat(
    void *priv, const char *path, vfs_stat_t *out
) {
    (void)priv;

    const char *name = entry_name(path);
    if (!name) return OSNOS_ENOENT;

    if (name[0] == 0) {
        out->type  = VFS_NODE_DIR;
        out->size  = 0;
        out->inode = 0;
        out->mode  = 0555;
        return OSNOS_OK;
    }

    for (size_t i = 0; i < DEVFS_DEV_COUNT; i++) {
        if (!os_streq(devices[i].name, name)) continue;
        out->type  = VFS_NODE_CHR;
        out->size  = 0;
        out->inode = i + 1;
        out->mode  = 0666;
        return OSNOS_OK;
    }

    return OSNOS_ENOENT;
}

static osnos_status_t devfs_readdir(
    void *priv, const char *path, size_t cursor,
    vfs_dirent_t *out, size_t *next_cursor
) {
    (void)priv;

    const char *name = entry_name(path);
    if (!name || name[0] != 0) return OSNOS_ENOENT;

    if (cursor >= DEVFS_DEV_COUNT) return OSNOS_ENOENT;

    os_strlcpy(out->name, devices[cursor].name, OSNOS_NAME_MAX);
    out->type = VFS_NODE_CHR;
    *next_cursor = cursor + 1;
    return OSNOS_OK;
}

static osnos_status_t devfs_read(
    void *priv, const char *path,
    size_t off,
    char *buf, size_t buf_size, size_t *out_size
) {
    (void)priv;
    /* Char devices are streams — `off` is meaningless; we ignore
     * it. sys_read flags the fd with is_chr=true so it doesn't
     * advance an offset for these either. */
    (void)off;

    const char *name = entry_name(path);
    if (!name) return OSNOS_ENOENT;
    if (name[0] == 0) return OSNOS_EISDIR;

    const devfs_dev_t *d = lookup_dev(name);
    if (!d) return OSNOS_ENOENT;

    return d->read(buf, buf_size, out_size);
}

static osnos_status_t devfs_write(
    void *priv, const char *path, const char *buf, size_t buf_size
) {
    (void)priv;

    const char *name = entry_name(path);
    if (!name) return OSNOS_ENOENT;
    if (name[0] == 0) return OSNOS_EISDIR;

    const devfs_dev_t *d = lookup_dev(name);
    if (!d) return OSNOS_ENOENT;

    return d->write(buf, buf_size);
}

static osnos_status_t devfs_rofs(void *priv, const char *path) {
    (void)priv; (void)path;
    return OSNOS_EROFS;
}

static osnos_status_t devfs_rename(
    void *priv, const char *src, const char *dst
) {
    (void)priv; (void)src; (void)dst;
    return OSNOS_EROFS;
}

const vfs_ops_t devfs_vfs_ops = {
    .stat    = devfs_stat,
    .readdir = devfs_readdir,
    .read    = devfs_read,
    .write   = devfs_write,
    .append  = devfs_write,
    .mkdir   = devfs_rofs,
    .rmdir   = devfs_rofs,
    .unlink  = devfs_rofs,
    .rename  = devfs_rename
};
