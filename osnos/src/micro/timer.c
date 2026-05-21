#include "timer.h"

#include "../drivers/pic.h"
#include "../proc/exec.h"
#include "idt.h"
#include "scheduler.h"
#include "syscall.h"
#include "task.h"

#include <stdint.h>

/* I/O port helpers. */
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(port));
}

/* PIT registers and constants. */
#define PIT_CH0       0x40
#define PIT_CMD       0x43
#define PIT_BASE_HZ   1193182u   /* 14.318MHz / 12 */

/* Command byte (port 0x43):
 *   bits 7-6  channel  (00 = ch0)
 *   bits 5-4  access   (11 = lobyte/hibyte)
 *   bits 3-1  mode     (011 = mode 3 — square wave generator)
 *   bit  0    BCD      (0 = binary)
 */
#define PIT_CMD_CH0_LH_MODE3  0x36

static volatile uint64_t ticks_counter;
static volatile uint64_t irq_counter;

/*
 * Preemption quantum, in timer ticks. With TIMER_HZ = 100 (10ms/tick),
 * a quantum of 5 gives a 50ms slice to each user task. Kernel tasks
 * are never preempted — when the timer fires in CPL=0 we just tick
 * and EOI.
 */
#define PREEMPT_QUANTUM 5
static int preempt_countdown = PREEMPT_QUANTUM;

uint64_t preempt_counter;   /* number of times we preempted a user task */

/*
 * Two-stage timer IRQ:
 *
 *   timer_entry (asm) — pushes the full 15-GPR syscall_frame_t and
 *                       calls timer_handle(frame). On return, pops
 *                       and iretq's. Same layout as int80_entry, so
 *                       timer_handle can stash the iret frame + GPRs
 *                       into task.saved_* with the same offsets that
 *                       user_task_resume reads later.
 *
 *   timer_handle (C)  — increments tick, sends EOI, and decides
 *                       whether to preempt the current user task.
 *                       If yes: snapshot state, mark READY (the task
 *                       remains runnable, just no longer the
 *                       currently-dispatched one), sched_resume_jump
 *                       back to scheduler_loop.
 *
 * Kernel-mode interrupts (CS=GDT_KCODE) are never preempted — the
 * kernel is cooperative. The handler ticks + EOIs and returns,
 * letting the original kernel code resume via iretq.
 */
__asm__ (
    ".global timer_entry\n"
    "timer_entry:\n"
    "    pushq %r15\n"
    "    pushq %r14\n"
    "    pushq %r13\n"
    "    pushq %r12\n"
    "    pushq %r11\n"
    "    pushq %r10\n"
    "    pushq %r9\n"
    "    pushq %r8\n"
    "    pushq %rbp\n"
    "    pushq %rdi\n"
    "    pushq %rsi\n"
    "    pushq %rdx\n"
    "    pushq %rcx\n"
    "    pushq %rbx\n"
    "    pushq %rax\n"

    "    movq %rsp, %rdi\n"             /* arg1 = frame* */
    "    movq %rsp, %r12\n"
    "    andq $-16, %rsp\n"

    "    call timer_handle\n"

    "    movq %r12, %rsp\n"

    "    popq %rax\n"
    "    popq %rbx\n"
    "    popq %rcx\n"
    "    popq %rdx\n"
    "    popq %rsi\n"
    "    popq %rdi\n"
    "    popq %rbp\n"
    "    popq %r8\n"
    "    popq %r9\n"
    "    popq %r10\n"
    "    popq %r11\n"
    "    popq %r12\n"
    "    popq %r13\n"
    "    popq %r14\n"
    "    popq %r15\n"
    "    iretq\n"
);

extern void timer_entry(void);

/* Forward decl — keeping the timer subsystem ignorant of the network
 * stack header here so the network layer can stay optional. */
extern void sock_tick(uint64_t now_ms);

void timer_handle(syscall_frame_t *frame) {
    ticks_counter++;
    irq_counter++;
    pic_send_eoi(IRQ_TIMER);

    /* TCP retransmission timer: walk sockets, retransmit any segment
     * that's been outstanding longer than TCP_RTO_MS. */
    sock_tick(ticks_counter * (uint64_t)TIMER_TICK_MS);

    /*
     * Iret frame sits just past the syscall_frame_t we pushed:
     *   frame (15 GPRs)  →  iret (5 words: rip, cs, rflags, rsp, ss)
     */
    uint64_t *iret = (uint64_t *)((char *)frame + sizeof(*frame));
    uint64_t cs = iret[1];

    /* Kernel mode: cooperative, never preempted. */
    if ((cs & 3) != 3) {
        preempt_countdown = PREEMPT_QUANTUM;
        return;
    }

    task_t *t = task_current();
    if (!t || !t->pml4 || !t->kernel_stack_top) return;
    if (t->state != TASK_RUNNING)                 return;

    /*
     * Honour a pending Ctrl+C kill. Runs at every timer tick from
     * user mode — even if the user task is in a tight loop and never
     * makes a syscall, this catches it within 10ms of the ^C event.
     */
    if (t->kill_pending) {
        proc_exit_current_user(130);   /* never returns */
    }

    if (--preempt_countdown > 0) return;
    preempt_countdown = PREEMPT_QUANTUM;

    /* Snapshot the full user CPU state. */
    t->saved_iret_rip    = iret[0];
    t->saved_iret_cs     = cs;
    t->saved_iret_rflags = iret[2];
    t->saved_iret_rsp    = iret[3];
    t->saved_iret_ss     = iret[4];

    t->saved_rax = frame->rax;
    t->saved_rbx = frame->rbx;
    t->saved_rcx = frame->rcx;
    t->saved_rdx = frame->rdx;
    t->saved_rsi = frame->rsi;
    t->saved_rdi = frame->rdi;
    t->saved_rbp = frame->rbp;
    t->saved_r8  = frame->r8;
    t->saved_r9  = frame->r9;
    t->saved_r10 = frame->r10;
    t->saved_r11 = frame->r11;
    t->saved_r12 = frame->r12;
    t->saved_r13 = frame->r13;
    t->saved_r14 = frame->r14;
    t->saved_r15 = frame->r15;

    t->saved_valid = 1;
    t->state       = TASK_READY;       /* preempted = runnable, not blocked */

    preempt_counter++;
    sched_resume_jump();                /* never returns */
}

static void pit_set_hz(uint32_t hz) {
    /*
     * The PIT counts down from the divisor at 1.193 MHz. Output
     * rate = base / divisor. Clamp the divisor to the 16-bit range
     * — at 1.193 MHz the smallest reachable rate is ~18.2 Hz.
     */
    uint32_t divisor = PIT_BASE_HZ / hz;
    if (divisor == 0)       divisor = 1;
    if (divisor > 0xFFFF)   divisor = 0xFFFF;

    outb(PIT_CMD, PIT_CMD_CH0_LH_MODE3);
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));
}

void timer_init(void) {
    ticks_counter = 0;
    irq_counter   = 0;
    preempt_counter = 0;
    preempt_countdown = PREEMPT_QUANTUM;

    pit_set_hz(TIMER_HZ);
    idt_set_handler(0x20, (void *)timer_entry, /*dpl=*/0);
    pic_unmask(IRQ_TIMER);
}

uint64_t timer_ticks(void)    { return ticks_counter;   }
uint64_t timer_ms   (void)    { return ticks_counter * (uint64_t)TIMER_TICK_MS; }
uint64_t timer_irqs (void)    { return irq_counter;     }
uint64_t timer_preempts(void) { return preempt_counter; }
