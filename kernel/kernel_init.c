/**
 * @file kernel_init.c
 * @brief Kernel initialization and hardware setup
 *
 * This file contains constructors that run before kernel_main to set up 
 * the system hardware, initialize the framebuffer, and prepare the terminal.
 */

#include <stddef.h>
#include <stdint.h>
#include "include/kernel/tty.h"
#include "include/kernel/multiboot.h"
#include "include/kernel/pmm.h"
#include "include/kernel/paging.h"    // <- add
#include "include/kernel/kheap.h"     // <- add

/**
 * Global variables holding multiboot information passed by bootloader
 * These are initialized by the assembly bootstrap code in boot.s
 */
extern uintptr_t multiboot_info_addr;  // Physical address of multiboot info structure
extern uint32_t multiboot_magic;       // Magic number from bootloader for verification

/**
 * External function to set up the framebuffer with bootloader info
 * Implemented in framebuffer.c
 * 
 * @param fb_info Pointer to the multiboot framebuffer information structure
 */
void framebuffer_initialize(struct multiboot_tag_framebuffer *fb_info);

/**
 * Early hardware initialization constructor
 */
__attribute__((constructor))
void hardware_init() {
    // Disable interrupts during early initialization
    __asm__ __volatile__("cli");
}

/**
 * Main kernel initialization constructor
 */
__attribute__((constructor))
void kernel_init() {
    if (multiboot_magic != MULTIBOOT2_MAGIC) {
        while (1) { __asm__ __volatile__("hlt"); }
    }

    uintptr_t addr = multiboot_info_addr + 8;
    struct multiboot_tag *tag = (struct multiboot_tag *)addr;
    struct multiboot_tag_framebuffer *fb = NULL;
    struct multiboot_tag_mmap* mmap_tag = NULL;

    while (tag->type != MULTIBOOT2_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT2_TAG_TYPE_FRAMEBUFFER) {
            fb = (struct multiboot_tag_framebuffer *)tag;
            framebuffer_initialize(fb);
        } else if (tag->type == MULTIBOOT2_TAG_TYPE_MMAP) {
            mmap_tag = (struct multiboot_tag_mmap*)tag;
        }
        addr += (tag->size + 7) & ~7;
        tag = (struct multiboot_tag *)addr;
    }

    terminal_initialize();

    terminal_setscale(2);
    terminal_writestring("Welcome to: \n");
    terminal_writestring(" _____                    ____   _____ \n");
    terminal_writestring("|  __ \\                  / __ \\ / ____|\n");
    terminal_writestring("| |  | | ___  __ _ _ __ | |  | | (___  \n");
    terminal_writestring("| |  | |/ _ \\/ _` | '_ \\| |  | |\\___ \\\n");
    terminal_writestring("| |__| |  __/ (_| | | | | |__| |____) |\n");
    terminal_writestring("|_____/ \\___|\\__,_|_| |_|\\____/|_____/\n\n");
    terminal_setscale(1);

    // Initialize PMM now that we have the memory map
    pmm_initialize(mmap_tag);

    // Enable paging (identity map kernel + framebuffer, map heap)
    paging_initialize(fb);

    // Initialize kernel heap on the mapped heap region
    kheap_initialize();
}