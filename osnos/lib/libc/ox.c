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
#include <ox.h>
#include <osnos_ipc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long g_server_pid = 0;

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

/* Send a message, then drain the IPC queue until a RESPONSE arrives;
 * any events seen along the way are pushed to the local ring. */
static long send_and_wait(ipc_msg_t *req) {
    req->to = SERVER_OX;
    if (ipc_send(req) < 0) return -1;
    for (;;) {
        ipc_msg_t r;
        if (ipc_recv(&r) == 0) {
            if (r.type == IPC_OX_RESPONSE) {
                if ((int)r.arg0 != 0) { errno = (int)r.arg0; return -1; }
                return (long)r.arg1;
            }
            if (is_event_type(r.type)) {
                ev_push(&r);
                continue;
            }
            /* Unknown opcode — drop and keep waiting. */
            continue;
        }
        if (errno != EAGAIN) return -1;
        struct timespec ts = { 0, 2 * 1000000 };  /* 2 ms */
        nanosleep(&ts, 0);
    }
}

/* Fire-and-forget sender (no response expected). */
static int send_one(ipc_msg_t *req) {
    req->to = SERVER_OX;
    for (;;) {
        if (ipc_send(req) == 0) return 0;
        if (errno != EAGAIN) return -1;
        /* Queue full — yield briefly and retry. */
        struct timespec ts = { 0, 1 * 1000000 };
        nanosleep(&ts, 0);
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
    long id = send_and_wait(&m);
    return (ox_win_t)id;
}

void ox_window_destroy(ox_win_t win) {
    if (g_server_pid == 0) return;
    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = IPC_OX_WINDOW_DESTROY;
    m.arg0 = (uint64_t)win;
    send_one(&m);
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

static void put_u32(char *p, uint32_t v) {
    p[0] = (char)(v & 0xff);
    p[1] = (char)((v >> 8) & 0xff);
    p[2] = (char)((v >> 16) & 0xff);
    p[3] = (char)((v >> 24) & 0xff);
}

void ox_draw_rect(ox_win_t win, int x, int y, int w, int h,
                   uint32_t color) {
    if (g_server_pid == 0) return;
    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = IPC_OX_DRAW_RECT;
    m.arg0 = (uint64_t)win;
    m.arg1 = color;
    put_u32(m.data + 0,  (uint32_t)x);
    put_u32(m.data + 4,  (uint32_t)y);
    put_u32(m.data + 8,  (uint32_t)w);
    put_u32(m.data + 12, (uint32_t)h);
    send_one(&m);
}

void ox_draw_text(ox_win_t win, int x, int y, const char *s,
                   uint32_t color) {
    if (g_server_pid == 0 || !s) return;
    size_t n = strlen(s);
    if (n > sizeof(((ipc_msg_t *)0)->data) - 9) n = sizeof(((ipc_msg_t *)0)->data) - 9;
    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = IPC_OX_DRAW_TEXT;
    m.arg0 = (uint64_t)win;
    m.arg1 = color;
    put_u32(m.data + 0, (uint32_t)x);
    put_u32(m.data + 4, (uint32_t)y);
    memcpy(m.data + 8, s, n);
    m.data[8 + n] = 0;
    send_one(&m);
}

/*
 * Chunk a BGRA rectangle into tile messages. Each IPC msg carries:
 *   header  : uint32 x, uint32 y, uint32 w, uint32 h  (16 bytes)
 *   payload : up to (1024-16) bytes of BGRA pixels
 *
 * Max payload = 1008 / 4 = 252 pixels per message. We chunk by
 * splitting the rectangle into 1-row stripes when the row is short
 * enough, or into 8×16 tiles otherwise. Simplest pattern: one row
 * at a time, clipped to 240 px wide.
 */
#define OX_TILE_W 240
void ox_draw_image(ox_win_t win, int x, int y, int w, int h,
                    const uint32_t *bgra, int src_pitch_px) {
    if (g_server_pid == 0 || !bgra) return;
    for (int row = 0; row < h; row++) {
        const uint32_t *src = bgra + (size_t)row * src_pitch_px;
        for (int col = 0; col < w; col += OX_TILE_W) {
            int chunk_w = w - col;
            if (chunk_w > OX_TILE_W) chunk_w = OX_TILE_W;
            ipc_msg_t m;
            memset(&m, 0, sizeof(m));
            m.type = IPC_OX_DRAW_IMAGE;
            m.arg0 = (uint64_t)win;
            put_u32(m.data + 0,  (uint32_t)(x + col));
            put_u32(m.data + 4,  (uint32_t)(y + row));
            put_u32(m.data + 8,  (uint32_t)chunk_w);
            put_u32(m.data + 12, 1);
            memcpy(m.data + 16, src + col,
                   (size_t)chunk_w * sizeof(uint32_t));
            send_one(&m);
        }
    }
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
        out->mouse_kind = (unsigned char)m->data[1];
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
    default:
        return 0;
    }
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
        struct timespec ts = { 0, 5 * 1000000 };  /* 5 ms */
        nanosleep(&ts, 0);
    }
}
