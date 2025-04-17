#include <stdint.h>
#include <stddef.h>
#include "font8x16.h"

// Add this line to declare the constructor function
extern void call_constructors(void);

#define MULTIBOOT2_TAG_TYPE_FRAMEBUFFER 8
#define MULTIBOOT2_MAGIC 0x36d76289

#define TERM_COLS 128
#define TERM_ROWS 48

// Forward declarations to fix implicit declaration warnings
void setScale(uint32_t scale);
void terminal_puts(const char *s);
void terminal_putc(char c);
void calculate_line_positions(void);
void terminal_redraw(void);
void terminal_scroll(void);
void drawChar(uint32_t *fb, uint32_t pitch, uint32_t x, uint32_t y, char c, uint32_t color, int scale);

// Corrected struct definition (fixed typo)
struct multiboot_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint16_t reserved;
};

struct multiboot_tag {
    uint32_t type;
    uint32_t size;
};

// Global variables to store multiboot info
extern uintptr_t multiboot_info_addr;
extern uint32_t multiboot_magic;

static char term_buffer[TERM_ROWS][TERM_COLS];
static uint8_t term_scale[TERM_ROWS][TERM_COLS];
static uint16_t line_positions[TERM_ROWS];
static int term_cursor_x, term_cursor_y;
static uint32_t *g_fb;
static uint32_t g_pitch;
static uint32_t g_scale;
static uint32_t g_color;
static uint32_t g_bg;
static struct multiboot_tag_framebuffer *fb;

// Replace your current var_constructor function
__attribute__((constructor))
void var_constructor() {
    term_cursor_x = 0;
    term_cursor_y = 0;
    g_bg = 0xffb7e5;  // Light pink background
    g_pitch = 0;
    g_scale = 1;
    g_color = 0xff0000;  // Red text color
    fb = NULL;
}

__attribute__((constructor))
void hardware_init() {
    // Disable interrupts during initialization
    __asm__ __volatile__("cli");
    
    // Initialize any CPU features needed
    // For example: Enable SSE/SSE2 if you're planning to use it
}

__attribute__((constructor))
void memory_init() {
    // Initialize memory regions or allocators
    // that don't depend on multiboot info
}

__attribute__((constructor))
void debug_init() {
    // Initialize debug flags or systems
    #ifdef DEBUG_BUILD
    debug_mode = 1;
    #endif
}

__attribute__((constructor)) // Run after other initializations
void terminal_init() {
    // Initialize terminal buffer to spaces
    for (int y = 0; y < TERM_ROWS; y++) {
        for (int x = 0; x < TERM_COLS; x++) {
            term_buffer[y][x] = ' ';
            term_scale[y][x] = 1;
        }
    }
    
    // Check for correct multiboot2 magic number
    if (multiboot_magic != MULTIBOOT2_MAGIC) {
        // Handle error: Not loaded by multiboot2-compliant bootloader
        // Simple message and halt
        for (int i = 0; i < 10; i++) {
            g_fb[i] = 0xFF0000; // Red pixels to indicate error
        }
        while (1) { __asm__ __volatile__("hlt"); }
    }
    
    // Process multiboot information using the global variable
    uintptr_t addr = multiboot_info_addr + 8; // Skip total_size field
    struct multiboot_tag *tag = (struct multiboot_tag *)addr;
    
    // Find framebuffer information
    while (tag->type != 0) {
        if (tag->type == MULTIBOOT2_TAG_TYPE_FRAMEBUFFER) {
            fb = (struct multiboot_tag_framebuffer *)tag;
            g_fb = (uint32_t *)(uintptr_t)fb->framebuffer_addr;
            g_pitch = fb->framebuffer_pitch / 4;

            // Clear screen to background color
            for (uint32_t y = 0; y < fb->framebuffer_height; y++) {
                for (uint32_t x = 0; x < fb->framebuffer_width; x++) {
                    g_fb[y * g_pitch + x] = g_bg;
                }
            }

            term_cursor_x = 1;
            term_cursor_y = 0;
            uint32_t old_scale = g_scale;
            setScale(2); // Set scale to 2 for the welcome message
            
            terminal_puts("Welcome to: \n");
            terminal_puts(" _____                    ____   _____ \n");
            terminal_puts("|  __ \\                  / __ \\ / ____|\n");
            terminal_puts("| |  | | ___  __ _ _ __ | |  | | (___  \n");
            terminal_puts("| |  | |/ _ \\/ _` | '_ \\| |  | |\\___ \\\n");
            terminal_puts("| |__| |  __/ (_| | | | | |__| |____) |\n");
            terminal_puts("|_____/ \\___|\\__,_|_| |_|\\____/|_____/\n");
            terminal_puts("\n");
            setScale(old_scale); // Restore original scale
            //terminal_puts("Dean Moore!\n");
            
            while (1) { __asm__ __volatile__("hlt"); }
        }
        addr += (tag->size + 7) & ~7; // align to 8 bytes
        tag = (struct multiboot_tag *)addr;
    }
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = s1, *p2 = s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++; p2++;
    }
    return 0;
}

void setScale(uint32_t scale) {
    g_scale = scale;
}

void setColor(uint32_t color) {
    g_color = color;
}

void setBackground(uint32_t color) {
    g_bg = color;
}

// Keep only this implementation of calculate_line_positions()
void calculate_line_positions() {
    uint16_t y_pos = 0;
    line_positions[0] = 0;
    
    for (uint32_t y = 0; y < TERM_ROWS-1; y++) {
        // Find the maximum scale in this line
        uint8_t line_scale = 1;
        for (uint32_t x = 0; x < TERM_COLS; x++) {
            if (term_scale[y][x] > line_scale)
                line_scale = term_scale[y][x];
        }
        
        // Update y position by the height of this line
        y_pos += 16 * line_scale;
        
        // Set the position for the next line
        line_positions[y+1] = y_pos;
    }
}

void drawChar(uint32_t *fb, uint32_t pitch, uint32_t x, uint32_t y, char c, uint32_t color, int scale) {
    int idx = (unsigned char)c;  // Keep this cast but remove the range check
    for (int row = 0; row < 16; row++) {
        uint32_t bits = font8x16[idx][row];
        for (int col = 0; col < 8; col++) {
            // Fix: Use (7-col) to flip the bit ordering horizontally
            if (bits & (1 << (7-col))) {
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        fb[(y + row * scale + dy) * pitch + (x + col * scale + dx)] = color;
                    }
                }
            } else {
                // Draw background for each pixel
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        fb[(y + row * scale + dy) * pitch + (x + col * scale + dx)] = g_bg;
                    }
                }
            }
        }
    }
}

void terminal_redraw() {
    // Clear the entire framebuffer first
    for (uint32_t y = 0; y < fb->framebuffer_height; y++) {
        for (uint32_t x = 0; x < fb->framebuffer_width; x++) {
            g_fb[y * g_pitch + x] = g_bg;
        }
    }
    
    // Now draw each line at its correct position
    for (uint32_t y = 0; y < TERM_ROWS; y++) {
        uint32_t y_pos = line_positions[y];
        
        for (uint32_t x = 0; x < TERM_COLS; x++) {
            char c = term_buffer[y][x];
            uint8_t scale = term_scale[y][x];
            
            if (c != ' ') { // Skip spaces for efficiency
                drawChar(g_fb, g_pitch, x * 8 * scale, y_pos, c, g_color, scale);
            }
        }
    }
}

void terminal_scroll() {
    // Shift terminal buffer and scale up
    for (uint32_t y = 1; y < TERM_ROWS; y++) {
        for (uint32_t x = 0; x < TERM_COLS; x++) {
            term_buffer[y-1][x] = term_buffer[y][x];
            term_scale[y-1][x] = term_scale[y][x];
        }
    }
    
    // Clear last line in buffer and scale
    for (uint32_t x = 0; x < TERM_COLS; x++) {
        term_buffer[TERM_ROWS-1][x] = ' ';
        term_scale[TERM_ROWS-1][x] = 1;
    }
    
    // Clear the entire framebuffer
    for (uint32_t y = 0; y < fb->framebuffer_height; y++) {
        for (uint32_t x = 0; x < fb->framebuffer_width; x++) {
            g_fb[y * g_pitch + x] = g_bg;
        }
    }
    
    // Recalculate line positions after scrolling
    calculate_line_positions();
    
    // Redraw with proper line spacing
    terminal_redraw();
}

void terminal_putc(char c) {
    if (c == '\n') {
        term_cursor_x = 1;
        
        // Find the maximum scale in the current line
        uint8_t max_scale = 1;
        for (int x = 0; x < TERM_COLS; x++) {
            if (term_scale[term_cursor_y][x] > max_scale)
                max_scale = term_scale[term_cursor_y][x];
        }
        
        // Move down by the maximum scale of the current line
        term_cursor_y += 1;
        
        // Recalculate line positions as we may have changed scale
        calculate_line_positions();
    } else if (c == '\r') {
        term_cursor_x = 1;
    } else {
        if (term_cursor_x < TERM_COLS && term_cursor_y < TERM_ROWS) {
            term_buffer[term_cursor_y][term_cursor_x] = c;
            term_scale[term_cursor_y][term_cursor_x] = g_scale;
            
            // Get the correct y position from our cache
            uint32_t y_pos = line_positions[term_cursor_y];
            uint8_t scale = g_scale;
            
            drawChar(g_fb, g_pitch, term_cursor_x * 8 * scale, y_pos, c, g_color, scale);
        }
        term_cursor_x++;
        if (term_cursor_x >= TERM_COLS) {
            term_cursor_x = 1;
            term_cursor_y += 1;
            
            // Recalculate line positions for the new row
            calculate_line_positions();
        }
    }
    
    if (term_cursor_y >= TERM_ROWS) {
        terminal_scroll();
        term_cursor_y = TERM_ROWS - 1;
    }
}

void terminal_puts(const char *s) {
    while (*s) {
        terminal_putc(*s++);
    }
}

void kernel_main(void) {

    // Infinite loop
    while (1) { __asm__ __volatile__("hlt"); }
}