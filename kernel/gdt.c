#include <stdint.h>
#include "include/kernel/gdt.h"


// Define 6 GDT entries: null, kcode, kdata, ucode, udata, TSS
struct gdt_entry gdt[6];
struct gdt_ptr   gp;

/*
 * W^X for the user stack (non-executable stack) on non-PAE i386.
 *
 * There is no per-page NX bit without PAE, so we make the stack non-executable
 * by capping the ring-3 *code* segment's limit below it. User code and heap
 * live low (ELF at 0x08048000); the user stack sits high (~0xBFFF4000). With
 * 4 KiB granularity a limit field of 0xBEFFF yields a code-fetch ceiling of
 * 0xBEFFFFFF, so any instruction fetch from the stack raises #GP (handled in
 * fault.c by killing just that task). The ring-3 *data* segment keeps the full
 * 4 GiB limit, so the stack stays readable/writable.
 *
 * Trade-off: everything at/above 0xBF000000 is non-executable, not only the
 * stack — acceptable for this layout (nothing else lives up there).
 */
#define USER_CODE_LIMIT_PAGES 0xBEFFFu   /* exec ceiling 0xBEFFFFFF (< 0xBF000000) */

// External assembly function to load the GDT
extern void gdt_flush(uint32_t);

// Set up a GDT entry
static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    // Set base address
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;
    
    // Set limits
    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    
    // Set granularity flags
    gdt[num].granularity |= gran & 0xF0;
    
    // Set access flags
    gdt[num].access = access;
}

// Initialize the GDT
void gdt_initialize() {
    // Setup the GDT pointer (6 entries; slot 5 = TSS, written by tss_initialize)
    gp.limit = (sizeof(struct gdt_entry) * 6) - 1;
    gp.base = (uint32_t)&gdt;
    
    // NULL descriptor
    gdt_set_gate(0, 0, 0, 0, 0);
    
    // Index 1: Kernel code segment (selector 0x08)
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    
    // Index 2: Kernel data segment (selector 0x10)
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    
    // Index 3: User code segment (selector 0x18, RPL 3 → 0x1B)
    // Limit capped below the user stack so the stack is non-executable (W^X).
    gdt_set_gate(3, 0, USER_CODE_LIMIT_PAGES, 0xFA, 0xCF);
    
    // Index 4: User data segment (selector 0x20, RPL 3 → 0x23)
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);
    
    // Index 5: TSS — zeroed now, filled by tss_initialize()
    gdt_set_gate(5, 0, 0, 0, 0);

    // Flush the GDT by calling assembly function
    gdt_flush((uint32_t)&gp);
}