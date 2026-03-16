#include <stdint.h>
#include "include/kernel/font.h"
#include "include/kernel/multiboot.h"

/**
 * Multiboot2 framebuffer tag type identifier
 * Used to identify framebuffer information in Multiboot2 tags
 */
#define MULTIBOOT2_TAG_TYPE_FRAMEBUFFER 8

/**
 * Global framebuffer state variables
 * g_fb: Pointer to the framebuffer memory
 * g_pitch: Number of 32-bit words per row
 * g_width: Screen width in pixels
 * g_height: Screen height in pixels
 */
static uint32_t *g_fb;    // Direct pointer to framebuffer memory
static uint32_t g_pitch;  // Row width in 32-bit words
static uint32_t g_width;  // Screen width in pixels
static uint32_t g_height; // Screen height in pixels

/**
 * Initializes the framebuffer with information from the bootloader
 * Sets up global variables to access the framebuffer properly
 *
 * @param fb_info Pointer to the framebuffer information structure
 */
void framebuffer_initialize(struct multiboot_tag_framebuffer *fb_info) {
    // Cast 64-bit address to pointer, making framebuffer accessible
    g_fb = (uint32_t *)(uintptr_t)fb_info->framebuffer_addr;
    // Convert pitch from bytes to 32-bit words for easier pixel addressing
    g_pitch = fb_info->framebuffer_pitch / 4;
    // Store dimensions for bounds checking and screen operations
    g_width = fb_info->framebuffer_width;
    g_height = fb_info->framebuffer_height;
}

/**
 * Clears the entire framebuffer with the specified color
 * Fills every pixel on screen with the same color value
 *
 * @param color 32-bit RGBA color value to fill the screen with
 */
void framebuffer_clear(uint32_t color) {
    // Iterate through each row
    for (uint32_t y = 0; y < g_height; y++) {
        // Iterate through each column
        for (uint32_t x = 0; x < g_width; x++) {
            g_fb[y * g_pitch + x] = color;
        }
    }
}

/**
 * Draws a character at the specified position with the given colors and scale
 *
 * @param x X-coordinate of the top-left corner of the character
 * @param y Y-coordinate of the top-left corner of the character
 * @param c Character to draw
 * @param color Foreground color for the character (32-bit RGBA)
 * @param bg_color Background color for the character (32-bit RGBA)
 * @param scale Size multiplier for the character (1 = normal size)
 */
void framebuffer_drawchar(uint32_t x, uint32_t y, char c, uint32_t color, uint32_t bg_color, int scale) {
    int idx = (unsigned char)c;
    
    // Adjust index to account for CP437 character set mapping discrepancies
    // This aligns control characters with their positions in the font array
    if (idx > 8){
        idx += 1;
        if(idx > 18){
            idx += 4;
        }
    }
    
    // Draw character pixel by pixel using font bitmap data
    for (int row = 0; row < 16; row++) {
        uint8_t bits = font8x16[idx][row];
        for (int col = 0; col < 8; col++) {
            // Check if this pixel should be foreground or background
            if (bits & (1 << (7-col))) {
                // Draw foreground pixel (scaled)
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        g_fb[(y + row * scale + dy) * g_pitch + (x + col * scale + dx)] = color;
                    }
                }
            } else {
                // Draw background pixel (scaled)
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        g_fb[(y + row * scale + dy) * g_pitch + (x + col * scale + dx)] = bg_color;
                    }
                }
            }
        }
    }
}

/**
 * Returns the width of the framebuffer in pixels
 *
 * @return Width of the screen in pixels
 */
uint32_t framebuffer_width(void) {
    return g_width;
}

/**
 * Returns the height of the framebuffer in pixels
 *
 * @return Height of the screen in pixels
 */
uint32_t framebuffer_height(void) {
    return g_height;
}

uint32_t framebuffer_getpixel(uint32_t x, uint32_t y) {
    if (!g_fb || x >= g_width || y >= g_height) return 0;
    return g_fb[y * g_pitch + x];
}

void framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!g_fb || x >= g_width || y >= g_height) return;
    g_fb[y * g_pitch + x] = color;
}
