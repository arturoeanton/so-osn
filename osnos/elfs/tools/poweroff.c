/*
 * /bin/poweroff — gracefully shut down the machine.
 *
 *   shellsrv:/$ poweroff
 *
 * On QEMU this fires the ACPI S5 vector (port 0xB004 = 0x2000) via
 * the kernel's sys_reboot — QEMU closes and the host gets control
 * back. On real hardware ACPI proper would be needed; we just fall
 * through to `cli; hlt`.
 *
 * Should never return; if it does we exit with a hint that the host
 * platform didn't honour the shutdown vectors.
 */

#include <stdio.h>
#include <sys/reboot.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("poweroff: shutting down osnos...\n");
    reboot(RB_POWER_OFF);
    /* Only reached if every ACPI vector + isa-debug-exit were
     * ignored — e.g. bare metal without ACPI tables wired up. */
    printf("poweroff: platform ignored shutdown, halting CPU.\n");
    reboot(RB_HALT_SYSTEM);
    return 1;
}
