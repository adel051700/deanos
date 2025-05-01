#include <stdint.h>
#include <string.h>
#include <kernel/gdt.h>

// GDT entry structure
struct gdt_entry {
    uint16_t limit_low;           // The lower 16 bits of the limit
    uint16_t base_low;            // The lower 16 bits of the base
    uint8_t  base_middle;         // The next 8 bits of the base
    uint8_t  access;              // Access flags, determine ring level
    uint8_t  granularity;         // Granularity flags, part of limit
    uint8_t  base_high;           // The last 8 bits of the base
} __attribute__((packed));

// GDT pointer structure
struct gdt_ptr {
    uint16_t limit;               // Size of GDT minus one
    uint32_t base;                // Address of the GDT
} __attribute__((packed));

// Define 5 GDT entries for our system
struct gdt_entry gdt[5];
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
    // Setup the GDT pointer
    gp.limit = (sizeof(struct gdt_entry) * 5) - 1;
    gp.base = (uint32_t)&gdt;
    
    // NULL descriptor
    gdt_set_gate(0, 0, 0, 0, 0);
    
    // Kernel code segment
    // base=0, limit=4GB, gran=4KB blocks, 32-bit, code segment, executable, ring 0
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    
    // Kernel data segment
    // base=0, limit=4GB, gran=4KB blocks, 32-bit, data segment, writable, ring 0
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    
    // User code segment
    // base=0, limit=4GB, gran=4KB blocks, 32-bit, code segment, executable, ring 3
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    
    // User data segment
    // base=0, limit=4GB, gran=4KB blocks, 32-bit, data segment, writable, ring 3
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);
    
    // Flush the GDT by calling assembly function
    gdt_flush((uint32_t)&gp);
}