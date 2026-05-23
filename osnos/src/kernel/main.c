#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <limine.h>

#include "../drivers/block_ata.h"
#include "../drivers/framebuffer.h"
#include "../drivers/lapic.h"
#include "../drivers/pic.h"
#include "../drivers/rtl8139.h"
#include "../drivers/serial.h"
#include "../proc/exec.h"
#include "../servers/keyboard_server.h"
#include "../servers/serial_input_server.h"
#include "../micro/fpu.h"
#include "../micro/gdt.h"
#include "../micro/idt.h"
#include "../micro/ipc.h"
#include "../micro/kmalloc.h"
#include "../micro/pipe.h"
#include "../micro/pmm.h"
#include "../micro/pty.h"
#include "../micro/reaper.h"
#include "../micro/task.h"
#include "../micro/syscall.h"
#include "../micro/syscall_msr.h"
#include "../micro/scheduler.h"
#include "../micro/service.h"
#include "../micro/timer.h"
#include "../micro/tss.h"
#include "../micro/uaccess.h"
#include "../micro/vmm.h"
#include "../fs/bootstrap.h"
#include "../fs/ramfs.h"
#include "../net/eth.h"

/* Forward decl for the respawn watchdog tick. Defined below. */
static void server_respawn_tick(void);

// ======================================================
// Limine requests
// ======================================================

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] =
    LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] =
    LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] =
    LIMINE_REQUESTS_END_MARKER;

// ======================================================
// Halt
// ======================================================

static void hcf(void) {
    for (;;) {
#if defined(__x86_64__)
        __asm__ volatile ("hlt");
#endif
    }
}

// ======================================================
// Kernel entry
// ======================================================

void kmain(void) {
    /* UART first — gives panic handlers a serial sink even if the
     * framebuffer request fails or the FB driver mis-inits. */
    serial_init(SERIAL_COM1);

    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        hcf();
    }

    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1) {
        hcf();
    }

    struct limine_framebuffer *fb =
        framebuffer_request.response->framebuffers[0];

    framebuffer_init(
        fb->address,
        fb->width,
        fb->height,
        fb->pitch
    );

    if (memmap_request.response == NULL || hhdm_request.response == NULL) {
        hcf();
    }
    pmm_init(memmap_request.response, hhdm_request.response->offset);
    vmm_init();
    kheap_init();

    /*
     * GDT + IDT after the memory layer so handlers can use kmalloc /
     * panic-print safely. Exceptions before this point still triple-
     * fault under Limine's IDT — acceptable since the boot path is
     * tightly controlled.
     */
    gdt_init();
    tss_init();
    idt_init();
    uaccess_init();        /* registers copy_*_user span in the extable */
    syscall_msr_init();    /* enables EFER.SCE + STAR/LSTAR/FMASK */
    fpu_init();            /* CR0/CR4 + FNINIT — lets ring-3 use double/SSE */
    pic_init();            /* remap 8259 to 0x20-0x2F, mask all lines */
    lapic_init();          /* enable LAPIC + LINT0=ExtINT (q35 needs this) */
    timer_init();          /* PIT @ 100 Hz, installs IDT[0x20], unmask IRQ0 */
    block_ata_init();      /* IDENTIFY primary master; FS layer mounts it */
    rtl8139_init();        /* PCI scan + driver; silent if no NIC */
    net_init();            /* register RX dispatch + ARP cache */

    ipc_init();
    pipe_init();           /* shell-pipeline kernel object */
    pty_init();            /* /dev/ptmx + /dev/pts/N pool */
    task_init();
    reaper_init();
    scheduler_init();
    syscall_init();
    ramfs_init();
    bootstrap_fs();

    /*
     * Kernel-side "keyboard" task is now a hardware-poll feeder only
     * (FASE 10.2): every tick it drains keyboard_poll into the
     * /dev/input0 ring. The user-visible policy layer (TTY feed +
     * IPC_KEY_EVENT) moved to the ring-3 kbdsrv ELF spawned below.
     * The kernel task does NOT register against SERVER_KEYBOARD;
     * that ID belongs to kbdsrv (sys_tty_input enforces it).
     */
    int keyboard_pid = task_create("keyboard", keyboard_server_tick);
    (void)keyboard_pid;

    /* Serial input feeder (FASE 10.7): pulls bytes off COM1's RX
     * register each tick and pushes them through tty_input(), the
     * same way kbdsrv does for PS/2. Means host-side serial input
     * (qemu -nographic + stdio) reaches shellsrv fd 0 verbatim. */
    int serial_in_pid = task_create("serial-in", serial_input_server_tick);
    (void)serial_in_pid;

    /* SERVER_FS / fs_server.c removed in FASE 10.3 — the shell speaks
     * directly to the VFS via syscalls. */

    /*
     * Console + keyboard + shell now all run as ring-3 ELFs:
     *   consrv   — FASE 10.1 (IPC_CONSOLE_* → /dev/fb0)
     *   kbdsrv   — FASE 10.2 (/dev/input0 → tty_input + IPC_KEY_EVENT)
     *   shellsrv — FASE 10.4 (line editor + dispatch + spawn)
     * kmain pre-registers each service ID so any early IPC traffic
     * lands on the right queue before the scheduler dispatches the
     * ELF for the first time. The ELFs also self-register on entry
     * (idempotent — same pid).
     */
    int64_t consrv_pid = proc_execve("/bin/consrv", "", 0);
    if (consrv_pid > 0) {
        service_register(SERVER_CONSOLE, (uint64_t)consrv_pid);
    }
    int64_t kbdsrv_pid = proc_execve("/bin/kbdsrv", "", 0);
    if (kbdsrv_pid > 0) {
        service_register(SERVER_KEYBOARD, (uint64_t)kbdsrv_pid);
    }
    int64_t shellsrv_pid = proc_execve("/bin/shellsrv", "", 0);
    if (shellsrv_pid > 0) {
        service_register(SERVER_SHELL, (uint64_t)shellsrv_pid);
    }

    /* Watchdog: cada ~100ms checkea si shellsrv sigue vivo. Si murió
     * (e.g. por `exec /bin/foo`, sys_exit, o fault), lo respawnea +
     * re-registra SERVER_SHELL. Sin esto, un `exec` interactivo deja
     * el sistema sin shell y aparece como cuelgue. Mismo para
     * consrv/kbdsrv por simetría — si cualquiera muere, init lo
     * trae de vuelta. */
    task_create("init-respawn", server_respawn_tick);

    keyboard_server_init();

    /*
     * Everything is set up — enable hardware interrupts. From this
     * point IRQ 0 (PIT) fires every 10 ms; the timer handler bumps
     * the tick counter and EOIs. No preemption yet — the cooperative
     * scheduler keeps running, but `timer_ms()` is now real wall time.
     */
    __asm__ volatile ("sti");

    /*
     * scheduler_loop is the eternal home of the kernel. It saves a
     * resume point at the top so sched_resume_jump() (called from
     * user-task sys_exit / fault handlers) can long-jump back without
     * losing the kernel's runtime.
     */
    scheduler_loop();
}

/* ----------------------------------------------------------------- */
/* init-respawn watchdog (kernel-side cooperative task)               */
/* ----------------------------------------------------------------- */
/*
 * Periodically checks the 3 ring-3 servers (consrv / kbdsrv /
 * shellsrv). If any one of them has died (state == TASK_UNUSED or
 * pid mismatch in their service slot), respawns it and re-registers
 * the service pid.
 *
 * Triggered: user runs `exec /bin/echo "..."` from interactive
 * shellsrv, echo finishes, shellsrv slot becomes UNUSED → no shell
 * to type into → system appears hung. With this watchdog, shellsrv
 * comes right back.
 *
 * Cooperative: returns after one check. Scheduler dispatches us
 * again next round-robin. With ~5 tasks total and 50ms preempt
 * quantum, that's a check every ~50-200ms — fast enough to feel
 * instant after `exec`.
 */
static void respawn_if_dead(int sid, const char *path) {
    uint64_t pid = service_get_pid((uint64_t)sid);
    int alive = 0;
    if (pid != 0) {
        for (size_t i = 0; i < 16; i++) {
            const task_t *t = task_slot(i);
            if (!t) continue;
            if (t->pid != pid) continue;
            if (t->state == TASK_UNUSED || t->state == TASK_DEAD) break;
            alive = 1;
            break;
        }
    }
    if (alive) return;

    /* Re-spawn. proc_execve allocates a new pid. */
    int64_t new_pid = proc_execve(path, "", 0);
    if (new_pid > 0) {
        service_register((uint64_t)sid, (uint64_t)new_pid);
    }
}

static void server_respawn_tick(void) {
    respawn_if_dead(SERVER_CONSOLE,  "/bin/consrv");
    respawn_if_dead(SERVER_KEYBOARD, "/bin/kbdsrv");
    respawn_if_dead(SERVER_SHELL,    "/bin/shellsrv");

    /* Sleep ~100 ms between checks. Without this the scheduler
     * re-dispatches us every round-robin cycle (~50us with idle
     * tasks), accumulating millions of pointless wake-ups. By
     * setting state=BLOCKED with a wakeup_at_ms timer,
     * task_check_wakeups flips us back to READY at the right time. */
    task_t *self = task_current();
    if (self) {
        self->wakeup_at_ms = timer_ms() + 100;
        self->state        = TASK_BLOCKED;
    }
}
