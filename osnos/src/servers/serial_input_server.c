#include "serial_input_server.h"

#include <stdint.h>

#include "../drivers/serial.h"
#include "../micro/task.h"
#include "../micro/timer.h"
#include "../micro/tty.h"

/*
 * Drain the UART RX FIFO and translate raw bytes into the TTY layer's
 * input stream. Mirrors what kbdsrv does for PS/2 (it reads
 * /dev/input0 and calls sys_tty_input(b) per byte). Since this runs
 * in ring 0, we call tty_input() directly — no syscall hop.
 *
 * Paced at ~10 ms via the wakeup_at_ms pattern (FASE 15.0). The old
 * always-READY version kept the scheduler from ever going idle —
 * with PS/2 input now IRQ-driven this was the last busy poller, and
 * blocking it lets scheduler_tick HLT between events. 10 ms matches
 * the PIT period, so worst-case serial latency is one timer tick —
 * imperceptible interactively (the 16550 FIFO buffers 16 bytes, and
 * QEMU's host-side pty buffers far more, so nothing is lost).
 *
 * Host terminals send '\r' on Enter, not '\n'. Translate inline so
 * the line discipline (ICANON's "newline commits the line") fires
 * at the right moment. This mirrors POSIX termios ICRNL on the
 * kernel TTY (which is set by default; see tty.c termios_default).
 */
void serial_input_server_tick(void) {
    uint8_t b;
    int drained = 0;
    while (serial_try_getc(&b) && drained < 64) {
        char c = (char)b;
        if (c == '\r') c = '\n';
        tty_input(c);
        drained++;
    }
    /* Sleep until the next PIT tick instead of being re-dispatched
     * every scheduler round. */
    task_t *self = task_current();
    if (self) {
        self->wakeup_at_ms = timer_ms() + 10;
        self->state        = TASK_BLOCKED;
    }
}
