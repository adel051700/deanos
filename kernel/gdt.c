#include <stdint.h>
#include "include/kernel/gdt.h"


// Define 6 GDT entries: null, kcode, kdata, ucode, udata, TSS
struct gdt_entry gdt[6];
struct gdt_ptr   gp;

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
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    
    // Index 4: User data segment (selector 0x20, RPL 3 → 0x23)
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);
    
    // Index 5: TSS — zeroed now, filled by tss_initialize()
    gdt_set_gate(5, 0, 0, 0, 0);

    // Flush the GDT by calling assembly function
    gdt_flush((uint32_t)&gp);
}