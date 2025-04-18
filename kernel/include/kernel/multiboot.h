#ifndef _KERNEL_MULTIBOOT_H
#define _KERNEL_MULTIBOOT_H

#include <stdint.h>

/**
 * Multiboot2 constants for bootloader communication
 * These define standard values used to interact with multiboot2-compliant bootloaders
 */
#define MULTIBOOT2_TAG_TYPE_FRAMEBUFFER 8  // Tag identifier for framebuffer information
#define MULTIBOOT2_MAGIC 0x36d76289        // Magic value passed by bootloader to kernel

/**
 * Structure containing framebuffer details provided by the bootloader
 * Includes memory location, dimensions and pixel format information
 * This allows the kernel to access the graphics hardware properly
 */
struct multiboot_tag_framebuffer {
    uint32_t type;                // Tag type identifier (should be 8)
    uint32_t size;                // Size of this structure in bytes
    uint64_t framebuffer_addr;    // Physical memory address of the framebuffer
    uint32_t framebuffer_pitch;   // Number of bytes per row
    uint32_t framebuffer_width;   // Width of the framebuffer in pixels
    uint32_t framebuffer_height;  // Height of the framebuffer in pixels
    uint8_t  framebuffer_bpp;     // Bits per pixel (color depth)
    uint8_t  framebuffer_type;    // Type of framebuffer (1=RGB, 2=EGA)
    uint16_t reserved;            // Reserved field for alignment
};

/**
 * Base structure for all multiboot tag types
 * Used to iterate through multiboot information structures
 */
struct multiboot_tag {
    uint32_t type;    // Type identifier for this tag
    uint32_t size;    // Size of this tag in bytes
};

#endif /* _KERNEL_MULTIBOOT_H */