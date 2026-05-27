/*
 * Ox client library implementation. See lib/libc/include/ox.h.
 *
 * Wire format (all multi-byte values little-endian, payload after
 * any fixed header):
 *   WINDOW_CREATE     arg0 = (w<<16)|h         data = title (NUL-term)
 *   WINDOW_DESTROY    arg0 = win_id            data = empty
 *   DRAW_RECT         arg0 = win_id  arg1=rgb  data = uint32 x,y,w,h
 *   DRAW_TEXT         arg0 = win_id  arg1=rgb  data = uint32 x,y + str
 *   DRAW_IMAGE        arg0 = win_id            data = uint32 x,y,w,h
 *                                                       + BGRA pixels
 *   PRESENT           arg0 = win_id            data = empty
 *   SET_TITLE         arg0 = win_id            data = title
 *   RESPONSE (S→C)    arg0 = status  arg1=value
 *   EVENT_KEY (S→C)   arg0 = win_id  arg1 = (ascii<<24)|(keycode<<8)|mods
 *   EVENT_MOUSE       arg0 = win_id  arg1 = ((uint32)x<<32)|y
 *                                    data[0]=buttons data[1]=kind
 *   EVENT_EXPOSE      arg0 = win_id  arg1 = ((uint32)x<<32)|y
 *                                    data = uint32 w,h
 *   EVENT_CLOSE       arg0 = win_id
 */

#include <errno.h>
#include <fcntl.h>
#include <ox.h>
#include <osnos_ipc.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static long g_server_pid = 0;

/* Per-window backing buffer — shared memory mapped from oxsrv. The
 * client writes directly to it for ox_draw_rect/text/image; ox_present
 * sends a single IPC to tell oxsrv "frame ready". Cuts heavy renders
 * (oxsettings: 10 thumbnails = 1200 IPCs) down to 1 IPC. */
#define OX_MAX_LOCAL_WINS 16
typedef struct {
    int       used;
    int       id;
    int       w, h;
    uint32_t *back;
    size_t    bytes;
} ox_local_win_t;
static ox_local_win_t g_local_wins[OX_MAX_LOCAL_WINS];

static ox_local_win_t *local_lookup(int id) {
    for (int i = 0; i < OX_MAX_LOCAL_WINS; i++) {
        if (g_local_wins[i].used && g_local_wins[i].id == id)
            return &g_local_wins[i];
    }
    return 0;
}
static ox_local_win_t *local_alloc(int id, int w, int h,
                                    uint32_t *back, size_t bytes) {
    for (int i = 0; i < OX_MAX_LOCAL_WINS; i++) {
        if (!g_local_wins[i].used) {
            g_local_wins[i].used  = 1;
            g_local_wins[i].id    = id;
            g_local_wins[i].w     = w;
            g_local_wins[i].h     = h;
            g_local_wins[i].back  = back;
            g_local_wins[i].bytes = bytes;
            return &g_local_wins[i];
        }
    }
    return 0;
}

/* Local drawing primitives — write directly into the shared backing
 * buffer. Same shape as oxsrv's buf_* helpers; clipped to the win's
 * width/height. */
static void local_fill_rect(ox_local_win_t *lw,
                             int x, int y, int w, int h,
                             uint32_t color) {
    if (!lw || !lw->back) return;
    int bw = lw->w, bh = lw->h;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > bw) w = bw - x;
    if (y + h > bh) h = bh - y;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        uint32_t *p = lw->back + (size_t)(y + row) * bw + x;
        for (int col = 0; col < w; col++) p[col] = color;
    }
}

static void local_draw_glyph(ox_local_win_t *lw,
                              int x, int y, int c, uint32_t color) {
    if (!lw || !lw->back) return;
    const uint8_t *g = ox_font_glyph(c);
    int bw = lw->w, bh = lw->h;
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            if (g[row] & (1 << (7 - col))) {
                int px = x + col, py = y + row;
                if (px < 0 || py < 0 || px >= bw || py >= bh) continue;
                lw->back[(size_t)py * bw + px] = color;
            }
        }
    }
}

static void local_draw_text(ox_local_win_t *lw, int x, int y,
                             const char *s, uint32_t color) {
    if (!s) return;
    int cx = x;
    while (*s) {
        if (*s == '\n') { y += 10; cx = x; s++; continue; }
        local_draw_glyph(lw, cx, y, (unsigned char)*s, color);
        cx += 8;
        s++;
    }
}

static void local_blit_image(ox_local_win_t *lw,
                              int dx, int dy, int sw, int sh,
                              const uint32_t *src, int src_pitch_px) {
    if (!lw || !lw->back || !src) return;
    int bw = lw->w, bh = lw->h;
    int x0 = dx, y0 = dy;
    int x1 = dx + sw, y1 = dy + sh;
    int sx0 = 0, sy0 = 0;
    if (x0 < 0) { sx0 = -x0; x0 = 0; }
    if (y0 < 0) { sy0 = -y0; y0 = 0; }
    if (x1 > bw) x1 = bw;
    if (y1 > bh) y1 = bh;
    if (x0 >= x1 || y0 >= y1) return;
    for (int row = y0; row < y1; row++) {
        const uint32_t *sp = src + (size_t)(sy0 + (row - y0)) * src_pitch_px + sx0;
        uint32_t *dp = lw->back + (size_t)row * bw + x0;
        memcpy(dp, sp, (size_t)(x1 - x0) * sizeof(uint32_t));
    }
}

/* Per-client event ring — events arriving while we're waiting for a
 * RESPONSE get queued here so the next ox_poll_event drains them. */
#define OX_EV_RING_CAP 32
static ipc_msg_t g_ev_ring[OX_EV_RING_CAP];
static int       g_ev_head, g_ev_tail, g_ev_count;

static void ev_push(const ipc_msg_t *m) {
    if (g_ev_count == OX_EV_RING_CAP) {
        /* Drop oldest. */
        g_ev_tail = (g_ev_tail + 1) % OX_EV_RING_CAP;
        g_ev_count--;
    }
    g_ev_ring[g_ev_head] = *m;
    g_ev_head = (g_ev_head + 1) % OX_EV_RING_CAP;
    g_ev_count++;
}

static int ev_pop(ipc_msg_t *out) {
    if (g_ev_count == 0) return 0;
    *out = g_ev_ring[g_ev_tail];
    g_ev_tail = (g_ev_tail + 1) % OX_EV_RING_CAP;
    g_ev_count--;
    return 1;
}

static int is_event_type(int t) {
    return t == IPC_OX_EVENT_KEY  ||
           t == IPC_OX_EVENT_MOUSE ||
           t == IPC_OX_EVENT_EXPOSE ||
           t == IPC_OX_EVENT_RESIZE ||
           t == IPC_OX_EVENT_CLOSE;
}

int ox_init(void) {
    if (g_server_pid > 0) return 0;
    long pid = ipc_service_lookup(SERVER_OX);
    if (pid < 0) { errno = ENOENT; return -1; }
    g_server_pid = pid;
    /* Identify ourselves to the server. The server uses msg->from to
     * track which client owns which window — no payload needed.
     * Address via SERVER_OX (service ID), NOT the bare pid — kernel
     * ipc_send tries SID lookup first. */
    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.to   = SERVER_OX;
    m.type = IPC_OX_CONNECT;
    if (ipc_send(&m) < 0) return -1;
    return 0;
}

/* send_and_wait was the IPC-draw path. Replaced by send_and_wait_resp
 * (returns also the response data[] for shm name handoff). Removed
 * to satisfy -Wunused. */

/* Fire-and-forget sender (no response expected). The kernel
 * sys_ipc_send blocks in-kernel on queue-full (since FASE perf),
 * so a single call is enough — no userland retry/sleep loop. */
static int send_one(ipc_msg_t *req) {
    req->to = SERVER_OX;
    return (ipc_send(req) == 0) ? 0 : -1;
}

/* Same as send_and_wait but also returns the response data[] so we
 * can recover the shm name oxsrv assigned to our new window. */
static long send_and_wait_resp(ipc_msg_t *req, char *resp_data, size_t cap) {
    req->to = SERVER_OX;
    if (ipc_send(req) < 0) return -1;
    for (;;) {
        ipc_msg_t r;
        if (ipc_recv(&r) == 0) {
            if (r.type == IPC_OX_RESPONSE) {
                if ((int)r.arg0 != 0) { errno = (int)r.arg0; return -1; }
                if (resp_data && cap > 0) {
                    size_t copy = cap - 1;
                    if (copy > sizeof(r.data)) copy = sizeof(r.data);
                    memcpy(resp_data, r.data, copy);
                    resp_data[copy] = 0;
                }
                return (long)r.arg1;
            }
            if (is_event_type(r.type)) { ev_push(&r); continue; }
            continue;
        }
        if (errno != EAGAIN) return -1;
        struct pollfd pfd = { -1, POLL_IPC_PENDING, 0 };
        /* 100 ms cap — long enough that we sleep, short enough that
         * any missed wake hook from oxsrv shows up as a ~10 Hz hit
         * instead of an absurd 1-second hang. */
        poll(&pfd, 1, 100);
    }
}

ox_win_t ox_window_create(int w, int h, const char *title) {
    if (g_server_pid == 0 && ox_init() < 0) return -1;
    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = IPC_OX_WINDOW_CREATE;
    m.arg0 = ((uint64_t)(uint16_t)w << 16) | (uint16_t)h;
    if (title) {
        size_t n = strlen(title);
        if (n > sizeof(m.data) - 1) n = sizeof(m.data) - 1;
        memcpy(m.data, title, n);
        m.data[n] = 0;
    }
    /* Response data[] holds the shm name oxsrv assigned. shm_open it,
     * mmap MAP_SHARED, store the local backing pointer so subsequent
     * ox_draw_* can write to it without any IPC round-trip. */
    char shm_name[32] = {0};
    long id = send_and_wait_resp(&m, shm_name, sizeof(shm_name));
    if (id < 0) return -1;
    if (shm_name[0] == '/') {
        int fd = shm_open(shm_name, O_RDWR, 0);
        if (fd >= 0) {
            size_t bsz = (size_t)w * h * sizeof(uint32_t);
            size_t mmap_bsz = (bsz + 4095) & ~(size_t)4095;
            void *p = mmap(0, mmap_bsz, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
            close(fd);
            if (p && p != (void *)-1) {
                local_alloc((int)id, w, h, (uint32_t *)p, mmap_bsz);
            }
        }
    }
    return (ox_win_t)id;
}

void ox_window_destroy(ox_win_t win) {
    if (g_server_pid == 0) return;
    /* Tear down local shared mapping first. */
    ox_local_win_t *lw = local_lookup((int)win);
    if (lw) {
        if (lw->back && lw->bytes) munmap(lw->back, lw->bytes);
        lw->used = 0; lw->id = 0; lw->w = 0; lw->h = 0;
        lw->back = 0; lw->bytes = 0;
    }
    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = IPC_OX_WINDOW_DESTROY;
    m.arg0 = (uint64_t)win;
    send_one(&m);
}

int ox_clipboard_set(const char *bytes, int len) {
    if (g_server_pid == 0 && ox_init() < 0) return -1;
    if (!bytes || len <= 0) return 0;
    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = IPC_OX_CLIPBOARD_SET;
    size_t n = (size_t)len;
    if (n > sizeof(m.data)) n = sizeof(m.data);
    memcpy(m.data, bytes, n);
    m.arg1 = (uint64_t)n;
    return send_one(&m);
}

int ox_clipboard_get(char *buf, int cap) {
    if (g_server_pid == 0 && ox_init() < 0) return 0;
    if (!buf || cap <= 0) return 0;
    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = IPC_OX_CLIPBOARD_GET;
    long n = send_and_wait_resp(&m, buf, (size_t)cap);
    if (n < 0) { buf[0] = 0; return 0; }
    /* server returned arg1 = total size; send_and_wait_resp truncated
     * to cap-1 already and NUL-terminated. Report the byte count the
     * caller actually has access to. */
    if (n > cap - 1) n = cap - 1;
    return (int)n;
}

void ox_window_set_title(ox_win_t win, const char *title) {
    if (g_server_pid == 0) return;
    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = IPC_OX_SET_TITLE;
    m.arg0 = (uint64_t)win;
    if (title) {
        size_t n = strlen(title);
        if (n > sizeof(m.data) - 1) n = sizeof(m.data) - 1;
        memcpy(m.data, title, n);
        m.data[n] = 0;
    }
    send_one(&m);
}

/* put_u32 was used by the IPC-draw payload encoders. With drawing
 * now local (shm-backed), the wire encoders are gone. Kept this
 * spot as a comment so anyone reading wire-format docs above can
 * cross-reference. */

/* Draws now go LOCAL: write directly into the shared-memory backing
 * buffer mapped by ox_window_create. No IPC traffic. ox_present (one
 * IPC) tells oxsrv "frame ready". This is the big perf win — what
 * used to be hundreds of IPCs per render is now zero. */
void ox_draw_rect(ox_win_t win, int x, int y, int w, int h,
                   uint32_t color) {
    ox_local_win_t *lw = local_lookup((int)win);
    if (!lw) return;
    local_fill_rect(lw, x, y, w, h, color);
}

void ox_draw_text(ox_win_t win, int x, int y, const char *s,
                   uint32_t color) {
    if (!s) return;
    ox_local_win_t *lw = local_lookup((int)win);
    if (!lw) return;
    local_draw_text(lw, x, y, s, color);
}

void ox_draw_image(ox_win_t win, int x, int y, int w, int h,
                    const uint32_t *bgra, int src_pitch_px) {
    if (!bgra) return;
    ox_local_win_t *lw = local_lookup((int)win);
    if (!lw) return;
    local_blit_image(lw, x, y, w, h, bgra, src_pitch_px);
}

void ox_present(ox_win_t win) {
    if (g_server_pid == 0) return;
    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = IPC_OX_PRESENT;
    m.arg0 = (uint64_t)win;
    send_one(&m);
}

/* Decode a server-sent IPC msg into an ox_event_t. */
static int decode_event(const ipc_msg_t *m, ox_event_t *out) {
    out->type = OX_EV_NONE;
    out->win  = (ox_win_t)m->arg0;
    out->ascii = 0;
    out->keycode = 0;
    out->mods = 0;
    out->x = out->y = 0;
    out->buttons = 0;
    out->mouse_kind = 0;
    out->ex = out->ey = out->ew = out->eh = 0;
    out->new_w = out->new_h = 0;
    switch (m->type) {
    case IPC_OX_EVENT_KEY:
        out->type    = OX_EV_KEY;
        out->ascii   = (int)((m->arg1 >> 24) & 0xff);
        out->keycode = (int)((m->arg1 >>  8) & 0xffff);
        out->mods    = (int)( m->arg1        & 0xff);
        return 1;
    case IPC_OX_EVENT_MOUSE:
        out->type    = OX_EV_MOUSE;
        out->x       = (int)((m->arg1 >> 32) & 0xffffffffu);
        out->y       = (int)( m->arg1        & 0xffffffffu);
        out->buttons = (unsigned char)m->data[0];
        out->mouse_kind  = (unsigned char)m->data[1];
        out->wheel_delta = (int)(signed char)m->data[2];
        return 1;
    case IPC_OX_EVENT_EXPOSE:
        out->type = OX_EV_EXPOSE;
        out->ex   = (int)((m->arg1 >> 32) & 0xffffffffu);
        out->ey   = (int)( m->arg1        & 0xffffffffu);
        out->ew   = (int)(((uint32_t)(uint8_t)m->data[0]      ) |
                          ((uint32_t)(uint8_t)m->data[1] << 8) |
                          ((uint32_t)(uint8_t)m->data[2] << 16)|
                          ((uint32_t)(uint8_t)m->data[3] << 24));
        out->eh   = (int)(((uint32_t)(uint8_t)m->data[4]      ) |
                          ((uint32_t)(uint8_t)m->data[5] << 8) |
                          ((uint32_t)(uint8_t)m->data[6] << 16)|
                          ((uint32_t)(uint8_t)m->data[7] << 24));
        return 1;
    case IPC_OX_EVENT_CLOSE:
        out->type = OX_EV_CLOSE;
        return 1;
    case IPC_OX_EVENT_RESIZE: {
        out->type  = OX_EV_RESIZE;
        out->new_w = (int)((m->arg1 >> 32) & 0xffffffffu);
        out->new_h = (int)( m->arg1        & 0xffffffffu);
        /* data[] is a NUL-terminated SHM name from oxsrv. Swap our
         * backing buffer to that SHM in-place — every subsequent
         * ox_draw_* writes the new buffer, and the next ox_present
         * implicitly ACKs the resize to the server (which then
         * unlinks the old SHM). The app sees an OX_EV_RESIZE event
         * with new_w/new_h and is expected to re-render. */
        ox_local_win_t *lw = local_lookup((int)out->win);
        if (lw && out->new_w > 0 && out->new_h > 0) {
            char shm_name[64];
            int n = (int)sizeof(shm_name) - 1;
            int i = 0;
            while (i < n && m->data[i]) {
                shm_name[i] = m->data[i];
                i++;
            }
            shm_name[i] = 0;
            int fd = shm_open(shm_name, O_RDWR, 0);
            if (fd >= 0) {
                size_t new_bsz = (size_t)out->new_w *
                                 (size_t)out->new_h * 4;
                size_t mmap_bsz = (new_bsz + 4095) & ~(size_t)4095;
                void *p = mmap(0, mmap_bsz,
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, 0);
                close(fd);
                if (p != (void *)-1) {
                    /* Old backing — release before swapping pointers
                     * so we never have two mappings of the same SHM
                     * alive simultaneously. */
                    if (lw->back) munmap(lw->back, lw->bytes);
                    lw->back  = (uint32_t *)p;
                    lw->bytes = mmap_bsz;
                    lw->w     = out->new_w;
                    lw->h     = out->new_h;
                }
            }
        }
        return 1;
    }
    default:
        return 0;
    }
}

int ox_window_dims(ox_win_t win, int *out_w, int *out_h) {
    ox_local_win_t *lw = local_lookup((int)win);
    if (!lw) return -1;
    if (out_w) *out_w = lw->w;
    if (out_h) *out_h = lw->h;
    return 0;
}

int ox_poll_event(ox_event_t *out) {
    if (!out) return 0;
    ipc_msg_t m;
    if (ev_pop(&m) && decode_event(&m, out)) return 1;
    /* Try to drain any unread messages. */
    while (ipc_recv(&m) == 0) {
        if (is_event_type(m.type)) {
            if (decode_event(&m, out)) return 1;
        }
        /* Stray RESPONSE — caller's expected req/resp pairing was
         * broken; drop it. */
    }
    return 0;
}

int ox_wait_event(ox_event_t *out) {
    if (!out) return 0;
    for (;;) {
        if (ox_poll_event(out)) return 1;
        /* True block in kernel: poll() with POLL_IPC_PENDING wakes
         * the task INSTANTLY when ipc_send → task_unblock fires for
         * our pid. Replaces a 200 Hz nanosleep busy-spin that burned
         * CPU across every open GUI app and saturated the scheduler.
         * The 1 s safety timeout is a fallback in case a wake hook
         * gets missed (shouldn't happen with the kernel changes). */
        struct pollfd pfd;
        pfd.fd = -1;
        pfd.events = POLL_IPC_PENDING;
        pfd.revents = 0;
        poll(&pfd, 1, 100);
    }
}
