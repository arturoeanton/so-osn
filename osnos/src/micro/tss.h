#pragma once

#include <stdint.h>

/*
 * Long-mode Task State Segment.
 *
 * In x86_64 the TSS is no longer used for hardware task switching — its
 * surviving role is to provide:
 *   - RSP0 / RSP1 / RSP2: the kernel-mode stack pointer for the CPU to
 *     switch to on a privilege transition (ring 3 -> ring 0 via
 *     interrupt or exception).
 *   - IST stacks: dedicated stacks for nasty exceptions (#DF, NMI, ...)
 *     so a corrupted RSP can't compound the disaster.
 *   - I/O permission bitmap pointer (we leave it past the TSS limit so
 *     all IO from ring 3 traps).
 *
 * One TSS for the whole CPU. Per-task kernel stack swapping is done by
 * rewriting tss.rsp0 when the scheduler dispatches a ring-3 task.
 */

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss_t;

void     tss_init(void);
tss_t   *tss_get(void);
void     tss_set_rsp0(uint64_t rsp0);
uint64_t tss_get_rsp0(void);

/*
 * Mirror of tss.rsp0 kept in sync by tss_set_rsp0. The SYSCALL entry
 * stub reads this directly via RIP-relative addressing so it doesn't
 * have to dereference into the TSS structure (which lives behind a
 * static).
 */
extern uint64_t tss_kernel_rsp0;

#define GDT_TSS  0x28   /* selector for the TSS descriptor (slot 5) */
