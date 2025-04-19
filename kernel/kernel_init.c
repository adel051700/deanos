/**
 * @file kernel_init.c
 * @brief Kernel initialization and hardware setup
 *
 * This file contains constructors that run before kernel_main to set up 
 * the system hardware, initialize the framebuffer, and prepare the terminal.
 */

#include <stddef.h>
#include <stdint.h>
#include <kernel/tty.h>
#include <kernel/multiboot.h>

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
    // Verify we were booted by a multiboot2-compliant bootloader
    if (multiboot_magic != MULTIBOOT2_MAGIC) {
        // If magic value doesn't match, halt the CPU indefinitely
        while (1) { __asm__ __volatile__("hlt"); }
    }
    
    // Multiboot2 info starts with 8-byte header
    uintptr_t addr = multiboot_info_addr + 8;
    struct multiboot_tag *tag = (struct multiboot_tag *)addr;
    struct multiboot_tag_framebuffer *fb = NULL;
    
    // Iterate through all multiboot tags to find the framebuffer information
    while (tag->type != 0) {  // Type 0 marks the end of the tags
        if (tag->type == MULTIBOOT2_TAG_TYPE_FRAMEBUFFER) {
            // Found the framebuffer tag, cast it to the appropriate structure type
            fb = (struct multiboot_tag_framebuffer *)tag;
            // Initialize the framebuffer subsystem with the provided info
            framebuffer_initialize(fb);
            break;
        }
        // Move to the next tag (align to 8-byte boundary)
        addr += (tag->size + 7) & ~7;  // Round up to multiple of 8
        tag = (struct multiboot_tag *)addr;
    }
    
    // Initialize the terminal subsystem which depends on the framebuffer
    terminal_initialize();
    
    // Set larger text scale for the welcome banner
    terminal_setscale(2);
    
    // Display the DeanOS ASCII art welcome banner
    terminal_writestring("Welcome to: \n");
    terminal_writestring(" _____                    ____   _____ \n");
    terminal_writestring("|  __ \\                  / __ \\ / ____|\n");
    terminal_writestring("| |  | | ___  __ _ _ __ | |  | | (___  \n");
    terminal_writestring("| |  | |/ _ \\/ _` | '_ \\| |  | |\\___ \\\n");
    terminal_writestring("| |__| |  __/ (_| | | | | |__| |____) |\n");
    terminal_writestring("|_____/ \\___|\\__,_|_| |_|\\____/|_____/\n");
    terminal_writestring("\n");
    
    // Restore original terminal scale for normal operation
    terminal_setscale(1);
}