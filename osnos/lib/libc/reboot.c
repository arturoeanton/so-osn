#include <sys/reboot.h>

#include "syscall.h"

int reboot(unsigned int cmd) {
    long r = osnos_syscall1(SYS_REBOOT, (long)cmd);
    /* Successful power-off / restart never returns. Reaching this
     * point means the kernel rejected the command (EINVAL) or the
     * I/O writes didn't take effect (real HW without ACPI). */
    return (int)r;
}
