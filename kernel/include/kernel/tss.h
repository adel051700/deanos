#ifndef _KERNEL_TSS_H
#define _KERNEL_TSS_H

#include <stdint.h>

/* i386 Task State Segment */
struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0;      /* kernel stack pointer (ring 0) */
    uint32_t ss0;       /* kernel stack segment  (ring 0) */
    uint32_t esp1, ss1;
    uint32_t esp2, ss2;
    uint32_t cr3;
    uint32_t eip, eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

/* Install TSS into GDT slot 5, load TR. */
void tss_initialize(uint32_t kernel_ss, uint32_t kernel_esp);

/* Update the ring-0 stack pointer (call on every context switch). */
void tss_set_kernel_stack(uint32_t esp0);

#endif

