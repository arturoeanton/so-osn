/*
 * elfs/osn-server/consrv.c — Console server, ring 3 (FASE 10.1).
 *
 * Replaces the in-kernel src/servers/console_server.c. The kernel
 * keeps the framebuffer driver; this server is the IPC ↔ /dev/fb0
 * bridge:
 *
 *   IPC_CONSOLE_WRITE(arg1 = byte count, data[] = payload)
 *     → write(fb0, data, count)
 *   IPC_CONSOLE_CLEAR
 *     → write(fb0, ESC[2J ESC[H)  (VT100 clear + home)
 *
 * Self-registers as SERVER_CONSOLE on startup. Any task that wants
 * to draw on the framebuffer addresses ipc_msg_t.to = SERVER_CONSOLE
 * and the kernel routes the message to us.
 *
 * Loop uses ipc_recv_block (libc loops on EAGAIN with a short
 * nanosleep) so we yield the CPU cleanly when idle.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "osnos_ipc.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int fb = open("/dev/fb0", O_WRONLY);
    if (fb < 0) {
        /* No way to surface the error visually since we ARE the
         * console — fall back to /dev/null so the panic doesn't
         * loop forever. */
        return 1;
    }

    if (ipc_service_register(SERVER_CONSOLE) != 0) {
        return 1;
    }

    /* Suspended while a GUI compositor (oxsrv) owns the framebuffer.
     * Writes are dropped (we still drain the IPC queue so senders
     * don't pile up). RESUME via IPC_CONSOLE_RESUME re-enables. */
    int suspended = 0;

    for (;;) {
        ipc_msg_t msg;
        if (ipc_recv_block(&msg) != 0) {
            /* Anything but EAGAIN propagated → unexpected. Restart
             * the loop; the kernel respawn (FASE 10.3+) will catch
             * a real crash. */
            continue;
        }

        switch (msg.type) {
        case IPC_CONSOLE_SUSPEND:
            suspended = 1;
            break;
        case IPC_CONSOLE_RESUME:
            suspended = 0;
            /* When the GUI exits, the user typically wants the shell
             * back where it was. Clear the FB so prompt redraws on
             * a clean canvas. */
            write(fb, "\x1b[2J\x1b[H", 7);
            break;
        case IPC_CONSOLE_WRITE: {
            if (suspended) break;
            /* arg1 holds the byte count when the sender wants a
             * specific length (e.g. shell colored writes); fall
             * back to strlen so legacy plain-C-string senders
             * still work. */
            size_t n = msg.arg1;
            if (n == 0 || n > IPC_DATA_SIZE) {
                n = strnlen(msg.data, IPC_DATA_SIZE);
            }
            /* arg0 carries the foreground color (24-bit RGB). The
             * old kernel console_server passed it straight to
             * framebuffer_draw_string; with the ring-3 split we
             * encode it as an inline ANSI 38;2;R;G;B SGR escape
             * + reset. The framebuffer's CSI parser honours both. */
            uint32_t color = (uint32_t)msg.arg0;
            char prefix[32];
            int  plen = 0;
            if (color != 0 && color != 0xffffff) {
                int r = (int)((color >> 16) & 0xff);
                int g = (int)((color >>  8) & 0xff);
                int b = (int)( color        & 0xff);
                plen = snprintf(prefix, sizeof(prefix),
                                "\x1b[38;2;%d;%d;%dm", r, g, b);
                if (plen > 0) write(fb, prefix, (size_t)plen);
            }
            if (n > 0) write(fb, msg.data, n);
            if (plen > 0) write(fb, "\x1b[39m", 5);
            break;
        }
        case IPC_CONSOLE_CLEAR:
            if (suspended) break;
            /* VT100 clear + home is what the kernel console used
             * to emit directly. The framebuffer CSI parser handles
             * both bytes in a single write. */
            write(fb, "\x1b[2J\x1b[H", 7);
            break;
        default:
            /* Stray opcode — drop it silently. The kernel never
             * sends mismatched ones, so this is a malformed
             * sender's problem, not ours. */
            break;
        }
    }
}
