#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "include/kernel/tty.h"
#include "include/kernel/font.h"
#include "include/kernel/multiboot.h"

/**
 * Terminal dimensions based on character size (8x16 pixels)
 * These define the logical grid size for text display
 */
#define TERM_COLS 128  //128-column text mode width
#define TERM_ROWS 48 // Number of text rows in the terminal

/**
 * External framebuffer function declarations
 */
void framebuffer_clear(uint32_t color);
void framebuffer_drawchar(uint32_t x, uint32_t y, char c, uint32_t color, uint32_t bg_color, int scale);
uint32_t framebuffer_width(void);
uint32_t framebuffer_height(void);

/**
 * Terminal state variables
 */
static char term_buffer[TERM_ROWS][TERM_COLS];
static uint8_t term_scale[TERM_ROWS][TERM_COLS];
static uint16_t line_positions[TERM_ROWS];
static int term_cursor_x, term_cursor_y;
static uint32_t term_color;
static uint32_t term_bg;
static uint32_t current_scale;
static int cursor_enabled = 0;
static int cursor_visible = 1;
static uint32_t cursor_blink_counter = 0;
static int terminal_ready = 0;
#define CURSOR_BLINK_INTERVAL_TICKS 50   // 100Hz PIT → 500ms

/**
 * Recalculates the vertical position of each line based on character scales
 */
static void calculate_line_positions(void) {
    uint16_t y_pos = 0;
    line_positions[0] = 0;
    
    for (uint32_t y = 0; y < TERM_ROWS-1; y++) {
        uint8_t line_scale = 1;
        for (uint32_t x = 0; x < TERM_COLS; x++) {
            if (term_scale[y][x] > line_scale)
                line_scale = term_scale[y][x];
        }
        
        y_pos += 16 * line_scale;
        line_positions[y+1] = y_pos;
    }
}

/**
 * Draw the cursor at current position
 */
static void draw_cursor(void) {
    if (!terminal_ready || !cursor_enabled) return;
    
    if (cursor_visible && term_cursor_y < TERM_ROWS && term_cursor_x < TERM_COLS) {
        uint32_t y_pos = line_positions[term_cursor_y];
        uint8_t scale = term_scale[term_cursor_y][term_cursor_x];
        
        // Calculate x position by accumulating widths of all characters before cursor
        uint32_t x_pos = 0;
        for (uint32_t i = 0; i < term_cursor_x; i++) {
            x_pos += 8 * term_scale[term_cursor_y][i];
        }
        
        // Draw cursor as a full block character (█ - 0xDB in CP437)
        framebuffer_drawchar(x_pos, y_pos, 0xDB, term_color, term_bg, scale);
    }
}

/**
 * Clear the cursor at current position
 */
static void clear_cursor(void) {
    if (!terminal_ready || !cursor_enabled) return;
    
    if (term_cursor_y < TERM_ROWS && term_cursor_x < TERM_COLS) {
        uint32_t y_pos = line_positions[term_cursor_y];
        uint8_t scale = term_scale[term_cursor_y][term_cursor_x];
        char c = term_buffer[term_cursor_y][term_cursor_x];
        
        // Calculate x position by accumulating widths of all characters before cursor
        uint32_t x_pos = 0;
        for (uint32_t i = 0; i < term_cursor_x; i++) {
            x_pos += 8 * term_scale[term_cursor_y][i];
        }
        
        // Redraw the character at cursor position
        framebuffer_drawchar(x_pos, y_pos, c, term_color, term_bg, scale);
    }
}

/**
 * Blink tick (called from PIT IRQ)
 */
void terminal_cursor_blink_tick(void) {
    if (!terminal_ready || !cursor_enabled) return;
    cursor_blink_counter++;
    if (cursor_blink_counter >= CURSOR_BLINK_INTERVAL_TICKS) {
        cursor_blink_counter = 0;
        if (cursor_visible) {
            clear_cursor();
            cursor_visible = 0;
        } else {
            cursor_visible = 1;
            draw_cursor();
        }
    }
}

/**
 * Enables cursor display (called by shell when ready for input)
 */
void terminal_enable_cursor(void) {
    cursor_enabled = 1;
    cursor_visible = 1;
    cursor_blink_counter = 0;  // Reset counter only here
    draw_cursor();
}

/**
 * Redraws the entire terminal contents to the framebuffer
 */
static void terminal_redraw(void) {
    framebuffer_clear(term_bg);
    
    for (uint32_t y = 0; y < TERM_ROWS; y++) {
        uint32_t y_pos = line_positions[y];
        
        for (uint32_t x = 0; x < TERM_COLS; x++) {
            char c = term_buffer[y][x];
            uint8_t scale = term_scale[y][x];
            
            if (c != ' ') {
                framebuffer_drawchar(x * 8 * scale, y_pos, c, term_color, term_bg, scale);
            }
        }
    }
    
    // Draw cursor after redraw if enabled
    if (cursor_enabled && cursor_visible) {
        draw_cursor();
    }
}

/**
 * Scrolls the terminal contents up by one line
 */
void terminal_scroll(void) {
    for (uint32_t y = 1; y < TERM_ROWS; y++) {
        for (uint32_t x = 0; x < TERM_COLS; x++) {
            term_buffer[y-1][x] = term_buffer[y][x];
            term_scale[y-1][x] = term_scale[y][x];
        }
    }
    
    for (uint32_t x = 0; x < TERM_COLS; x++) {
        term_buffer[TERM_ROWS-1][x] = ' ';
        term_scale[TERM_ROWS-1][x] = 1;
    }
    
    calculate_line_positions();
    terminal_redraw();
}

/**
 * Initializes the terminal with default settings
 */
void terminal_initialize(void) {
    term_cursor_x = 1;
    term_cursor_y = 0;
    term_color = 0xFF0000;
    term_bg = 0xFFB7E5;
    current_scale = 1;
    cursor_visible = 1;
    cursor_enabled = 0;
    terminal_ready = 0;
    
    for (uint32_t y = 0; y < TERM_ROWS; y++) {
        for (uint32_t x = 0; x < TERM_COLS; x++) {
            term_buffer[y][x] = ' ';
            term_scale[y][x] = 1;
        }
    }
    
    calculate_line_positions();
    framebuffer_clear(term_bg);
    
    terminal_ready = 1;
}

/**
 * Outputs a single character to the terminal at the current cursor position
 */
void terminal_putchar(char c) {
    if (!terminal_ready) return;
    
    // Clear cursor before moving/writing (only if cursor is enabled)
    if (cursor_enabled && cursor_visible) {
        clear_cursor();
    }
    
    if (c == '\n') {
        term_cursor_x = 1; 
        term_cursor_y += 1;
        calculate_line_positions();
    } else if (c == '\r') {
        term_cursor_x = 0;
    } else if (c == '\b') {
        if (term_cursor_x > 0) {
            term_cursor_x--;
            term_buffer[term_cursor_y][term_cursor_x] = ' ';
            uint32_t y_pos = line_positions[term_cursor_y];
            framebuffer_drawchar(term_cursor_x * 8 * current_scale, y_pos, 
                             ' ', term_color, term_bg, current_scale);
        } else if (term_cursor_y > 0) {
            term_cursor_y--;
            term_cursor_x = TERM_COLS - 1;
            while (term_cursor_x > 0 && term_buffer[term_cursor_y][term_cursor_x] == ' ') {
                term_cursor_x--;
            }
            if (term_buffer[term_cursor_y][term_cursor_x] != ' ') {
                term_cursor_x++;
            }
        }
    } else {
        if (term_cursor_x < TERM_COLS && term_cursor_y < TERM_ROWS) {
            term_buffer[term_cursor_y][term_cursor_x] = c;
            term_scale[term_cursor_y][term_cursor_x] = current_scale;
            
            uint32_t y_pos = line_positions[term_cursor_y];
            
            // Calculate x position by accumulating widths of all characters before cursor
            uint32_t x_pos = 0;
            for (uint32_t i = 0; i < term_cursor_x; i++) {
                x_pos += 8 * term_scale[term_cursor_y][i];
            }
            
            framebuffer_drawchar(x_pos, y_pos, c, term_color, term_bg, current_scale);
        }
        term_cursor_x++;
        if (term_cursor_x >= TERM_COLS) {
            term_cursor_x = 0;
            term_cursor_y += 1;
            calculate_line_positions();
        }
    }
    
    if (term_cursor_y >= TERM_ROWS) {
        terminal_scroll();
        term_cursor_y = TERM_ROWS - 1;
    }
    
    // ONLY redraw cursor if it's visible - don't force it visible or reset counter
    if (cursor_enabled && cursor_visible) {
        draw_cursor();
    }
}

/**
 * Writes a sequence of characters to the terminal
 */
void terminal_write(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

/**
 * Writes a null-terminated string to the terminal
 */
void terminal_writestring(const char* data) {
    terminal_write(data, strlen(data));
}

/**
 * Sets the current text color for the terminal
 */
void terminal_setcolor(uint32_t color) {
    term_color = color;
}

/**
 * Sets the current background color for the terminal
 */
void terminal_setbackground(uint32_t color) {
    term_bg = color;
}

/**
 * Sets the current text scale factor for the terminal
 */
void terminal_setscale(uint32_t scale) {
    current_scale = scale;
}

uint32_t terminal_get_color(void) {
    	return term_color;
}