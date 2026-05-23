#pragma once

/*
 * Kernel-side cooperative task that polls the UART receive register
 * each scheduler tick and feeds incoming bytes into the kernel TTY
 * layer via `tty_input` — the same path used by ring-3 kbdsrv when it
 * sees PS/2 keystrokes. Effect: every byte typed into QEMU's serial
 * (`-serial mon:stdio`, `-nographic`) flows to shellsrv fd 0 just
 * like a physical PS/2 keystroke would.
 *
 * Spawn pattern: identical to keyboard_server (see kmain).
 */

void serial_input_server_tick(void);
