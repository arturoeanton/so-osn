#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <limine.h>

#include "../drivers/block_ata.h"
#include "../drivers/framebuffer.h"
#include "../drivers/lapic.h"
#include "../drivers/pic.h"
#include "../drivers/rtl8139.h"
#include "../servers/console_server.h"
#include "../servers/keyboard_server.h"
#include "../servers/shell_server.h"
#include "../micro/fpu.h"
#include "../micro/gdt.h"
#include "../micro/idt.h"
#include "../micro/ipc.h"
#include "../micro/kmalloc.h"
#include "../micro/pipe.h"
#include "../micro/pmm.h"
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
#include "../servers/fs_server.h"

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
    task_init();
    reaper_init();
    scheduler_init();
    syscall_init();
    ramfs_init();
    bootstrap_fs();

    int fs_pid = task_create("fs", fs_server_tick);
    int keyboard_pid = task_create("keyboard", keyboard_server_tick);
    int shell_pid = task_create("shell", shell_server_tick);
    int console_pid = task_create("console", console_server_tick);

    service_register(SERVER_FS, fs_pid);
    service_register(SERVER_KEYBOARD, keyboard_pid);
    service_register(SERVER_SHELL, shell_pid);
    service_register(SERVER_CONSOLE, console_pid);

    fs_server_init();
    console_server_init();
    keyboard_server_init();
    shell_server_init();

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
