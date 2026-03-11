/*
 * tss.c — Task State Segment setup
 *
 * The TSS tells the CPU which stack to use when transitioning from
 * ring 3 → ring 0 (interrupts, syscalls, exceptions).
 * We only use a single TSS; on every context switch we update esp0
 * to point to the top of the current task's kernel stack.
 */

#include "include/kernel/tss.h"
#include "include/kernel/gdt.h"
#include "../libc/include/string.h"
#include <stdint.h>

#define GDT_TSS_SEL  0x28   /* index 5, RPL 0 */


/* The single system-wide TSS. */
static struct tss_entry g_tss;

/* Low-level: load TR register. */
static inline void ltr(uint16_t sel) {
    __asm__ __volatile__("ltr %0" : : "r"(sel));
}

/*
 * Write a TSS descriptor into the GDT.
 * A TSS descriptor is a "system segment" with type 0x89 (32-bit TSS, available).
 */
static void gdt_write_tss(int num, uint32_t base, uint32_t limit) {
    gdt[num].base_low    = base & 0xFFFF;
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;

    gdt[num].limit_low   = limit & 0xFFFF;
    gdt[num].granularity  = (limit >> 16) & 0x0F;
    gdt[num].granularity |= 0x00;  /* byte granularity, 16-bit (for TSS) */

    gdt[num].access = 0x89;        /* present, DPL 0, type = 32-bit TSS */
}

void tss_initialize(uint32_t kernel_ss, uint32_t kernel_esp) {
    memset(&g_tss, 0, sizeof(g_tss));

    g_tss.ss0  = kernel_ss;
    g_tss.esp0 = kernel_esp;

    /* IOPL = 0, no I/O permission bitmap */
    g_tss.iomap_base = sizeof(g_tss);

    /* Write TSS descriptor into GDT slot 5 and update GDT size. */
    gdt_write_tss(5, (uint32_t)&g_tss, sizeof(g_tss) - 1);

    /* Update GDT limit to cover 6 entries (0–5). */
    gp.limit = (sizeof(struct gdt_entry) * 6) - 1;

    /* Reload GDT then load the Task Register. */
    __asm__ __volatile__("lgdt %0" : : "m"(gp));
    ltr(GDT_TSS_SEL);
}

void tss_set_kernel_stack(uint32_t esp0) {
    g_tss.esp0 = esp0;
}

