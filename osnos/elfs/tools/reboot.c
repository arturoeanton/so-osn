/*
 * /bin/reboot — restart the machine via the 8042 keyboard
 * controller reset line (port 0x64 ← 0xFE). Universal on PC
 * platforms; QEMU honors it; real HW too.
 */

#include <stdio.h>
#include <sys/reboot.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("reboot: restarting osnos...\n");
    reboot(RB_AUTOBOOT);
    printf("reboot: 8042 reset ignored, halting.\n");
    reboot(RB_HALT_SYSTEM);
    return 1;
}
