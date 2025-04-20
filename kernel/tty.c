#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <kernel/tty.h>

/**
 * Terminal dimensions based on character size (8x16 pixels)
 * These define the logical grid size for text display
 */
#define TERM_COLS 128  //128-column text mode width
#define TERM_ROWS 48 // Number of text rows in the terminal

/**
 * External framebuffer function declarations
 * These functions are implemented in framebuffer.c and used here
 * for rendering characters to the screen
 */
void framebuffer_clear(uint32_t color);  // Clears screen with specified color
void framebuffer_drawchar(uint32_t x, uint32_t y, char c, uint32_t color, uint32_t bg_color, int scale);  // Draws a character
uint32_t framebuffer_width(void);  // Returns framebuffer width in pixels
uint32_t framebuffer_height(void); // Returns framebuffer height in pixels

/**
 * Terminal state variables
 * These variables maintain the current state of the terminal display
 * 
 * term_buffer: Character data for each position in the grid
 * term_scale: Scale factor for each character (allows mixed sizes)
 * line_positions: Y-coordinate of each line in the terminal (for variable height lines)
 * term_cursor_x/y: Current cursor position within the grid
 * term_color: Current text color (32-bit RGBA)
 * term_bg: Current background color (32-bit RGBA)
 * current_scale: Current character size multiplier for new text
 */
static char term_buffer[TERM_ROWS][TERM_COLS];     // 2D grid of characters
static uint8_t term_scale[TERM_ROWS][TERM_COLS];   // 2D grid of character scales
static uint16_t line_positions[TERM_ROWS];         // Y position of each row in pixels
static int term_cursor_x, term_cursor_y;           // Current cursor position
static uint32_t term_color;                        // Current foreground color
static uint32_t term_bg;                           // Current background color
static uint32_t current_scale;                     // Current character scale factor

/**
 * Recalculates the vertical position of each line based on character scales
 * This ensures proper vertical spacing when different text scales are used
 * Lines with larger scale characters take up more vertical space
 */
static void calculate_line_positions(void) {
    uint16_t y_pos = 0;
    line_positions[0] = 0;  // First line always starts at y=0
    
    // Calculate position for each subsequent line
    for (uint32_t y = 0; y < TERM_ROWS-1; y++) {
        // Find the largest scale factor used in this line
        uint8_t line_scale = 1;
        for (uint32_t x = 0; x < TERM_COLS; x++) {
            if (term_scale[y][x] > line_scale)
                line_scale = term_scale[y][x];
        }
        
        // Advance y position by the height of this line (16 pixels * scale)
        y_pos += 16 * line_scale;
        // Store the starting position of the next line
        line_positions[y+1] = y_pos;
    }
}

/**
 * Redraws the entire terminal contents to the framebuffer
 * Uses the line positions to properly render with variable line heights
 * This function is called when the terminal contents change significantly
 */
static void terminal_redraw(void) {
    // Clear the entire screen with the background color
    framebuffer_clear(term_bg);
    
    // Draw each character in the terminal buffer
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
}

/**
 * Scrolls the terminal contents up by one line
 * Moves all content up and clears the bottom line
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
 * Sets up the character buffer, scales, and cursor position
 */
void terminal_initialize(void) {
    term_cursor_x = 1;
    term_cursor_y = 0;
    term_color = 0xFF0000;
    term_bg = 0xFFB7E5;
    current_scale = 1;
    
    for (uint32_t y = 0; y < TERM_ROWS; y++) {
        for (uint32_t x = 0; x < TERM_COLS; x++) {
            term_buffer[y][x] = ' ';
            term_scale[y][x] = 1;
        }
    }
    
    calculate_line_positions();
    framebuffer_clear(term_bg);
}

/**
 * Outputs a single character to the terminal at the current cursor position
 * Handles special characters like newline and carriage return
 * Automatically advances cursor and scrolls if needed
 *
 * @param c Character to display
 */
void terminal_putchar(char c) {
    if (c == '\n') {
        term_cursor_x = 1; 
        term_cursor_y += 1;
        calculate_line_positions();
    } else if (c == '\r') {
        term_cursor_x = 0;
    } else if (c == '\b') {
        // Handle backspace character
        if (term_cursor_x > 0) {
            term_cursor_x--;
            // Clear the character at the current position by replacing it with a space
            term_buffer[term_cursor_y][term_cursor_x] = ' ';
            uint32_t y_pos = line_positions[term_cursor_y];
            // Redraw the space character to erase the previous character
            framebuffer_drawchar(term_cursor_x * 8 * current_scale, y_pos, 
                             ' ', term_color, term_bg, current_scale);
        } else if (term_cursor_y > 0) {
            // If at beginning of line and not first line, move up to end of previous line
            term_cursor_y--;
            term_cursor_x = TERM_COLS - 1;
            // Find the last non-space character on the previous line
            while (term_cursor_x > 0 && term_buffer[term_cursor_y][term_cursor_x] == ' ') {
                term_cursor_x--;
            }
            if (term_buffer[term_cursor_y][term_cursor_x] != ' ') {
                term_cursor_x++; // Position after the last character
            }
        }
    } else {
        if (term_cursor_x < TERM_COLS && term_cursor_y < TERM_ROWS) {
            term_buffer[term_cursor_y][term_cursor_x] = c;
            term_scale[term_cursor_y][term_cursor_x] = current_scale;
            
            uint32_t y_pos = line_positions[term_cursor_y];
            
            framebuffer_drawchar(term_cursor_x * 8 * current_scale, y_pos, 
                             c, term_color, term_bg, current_scale);
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
}

/**
 * Writes a sequence of characters to the terminal
 *
 * @param data Pointer to character data
 * @param size Number of characters to write
 */
void terminal_write(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

/**
 * Writes a null-terminated string to the terminal
 *
 * @param data Pointer to null-terminated string
 */
void terminal_writestring(const char* data) {
    terminal_write(data, strlen(data));
}

/**
 * Sets the current text color for the terminal
 *
 * @param color 32-bit RGBA color value
 */
void terminal_setcolor(uint32_t color) {
    term_color = color;
}

/**
 * Sets the current background color for the terminal
 *
 * @param color 32-bit RGBA color value
 */
void terminal_setbackground(uint32_t color) {
    term_bg = color;
}

/**
 * Sets the current text scale factor for the terminal
 *
 * @param scale Size multiplier (1 = normal size)
 */
void terminal_setscale(uint32_t scale) {
    current_scale = scale;
}