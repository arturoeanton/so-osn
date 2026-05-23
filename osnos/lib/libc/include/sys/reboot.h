#ifndef _SYS_REBOOT_H
#define _SYS_REBOOT_H

/*
 * reboot(2) — power-off / restart / halt. Linux-compat command
 * codes (we don't enforce the magic1/magic2 cookies; osnos calls
 * are trusted from userland).
 *
 * Returns 0 on success in theory; in practice never returns —
 * the kernel will have shut down the platform by then.
 */

#define RB_POWER_OFF    0x4321FEDC
#define RB_AUTOBOOT     0x01234567    /* restart */
#define RB_HALT_SYSTEM  0xCDEF0123

int reboot(unsigned int cmd);

#endif
