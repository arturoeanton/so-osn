#include "serial_input_server.h"

#include <stdint.h>

#include "../drivers/serial.h"
#include "../micro/tty.h"

/*
 * Drain the UART RX FIFO each tick and translate raw bytes into the
 * TTY layer's input stream. Mirrors what kbdsrv does for PS/2 (it
 * reads /dev/input0 and calls sys_tty_input(b) per byte). Since this
 * runs in ring 0, we call tty_input() directly — no syscall hop.
 *
 * Tick frequency is the scheduler's task quantum (50ms preempt for
 * CPL=3 / cooperative dispatch otherwise). Net latency for the host
 * sending a key over serial: <50ms — imperceptible interactively.
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
}
