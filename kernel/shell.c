#include "include/kernel/shell.h"
#include "include/kernel/tty.h"
#include "include/kernel/keyboard.h"
#include "include/kernel/rtc.h"
#include "include/kernel/pit.h"
#include <string.h>
#include <stdio.h>

#define SHELL_PROMPT "DeanOS $ "
#define MAX_COMMAND_LENGTH 256
#define SHELL_HISTORY_SIZE 32

static char command_buffer[MAX_COMMAND_LENGTH];
static size_t command_length = 0;

static char history[SHELL_HISTORY_SIZE][MAX_COMMAND_LENGTH];
static size_t history_len = 0;     // number of stored entries
static size_t history_pos = 0;     // browsing index: 0..history_len (history_len = current line)
static char edit_backup[MAX_COMMAND_LENGTH]; // current line backup when entering history

typedef enum { ESC_IDLE=0, ESC_GOT_ESC, ESC_GOT_BRACKET } esc_state_t;
static esc_state_t esc_state = ESC_IDLE;

// Forward decls for history helpers
static void history_add(const char* cmd);
static void shell_set_line(const char* s);
static void shell_execute_command(const char* command);

// Command handler prototypes
static void cmd_help(const char* args);
static void cmd_echo(const char* args);
static void cmd_color(const char* args);
static void cmd_cls(const char* args);
static void cmd_about(const char* args);
static void cmd_dean(const char* args);
static void cmd_time(const char* args);
static void cmd_uptime(const char* args);
static void cmd_ticks(const char* args);

// Command descriptor
struct shell_command {
    const char* name;
    void (*handler)(const char* args);
    const char* description;
};

// Command table
static const struct shell_command commands[] = {
    {"help",   cmd_help,   "List available commands"},
    {"echo",   cmd_echo,   "Echo text"},
    {"color",  cmd_color,  "Change text color"},
    {"cls",    cmd_cls,    "Clear screen"},
    {"about",  cmd_about,  "About DeanOS"},
    {"dean",   cmd_dean,   "Show DeanOS banner"},
    {"time",   cmd_time,   "Show current time/date"},
    {"uptime", cmd_uptime, "Show system uptime"},
    {"ticks",  cmd_ticks,  "Show PIT tick count"},
    {NULL, NULL, NULL}
};

// Remove the old non-static forward line:
// - void cmd_cls(const char* args);
// ...existing code...

/**
 * Initialize the shell
 */
void shell_initialize(void) {
    command_length = 0;
    history_len = 0;
    history_pos = 0;
    esc_state = ESC_IDLE;
    terminal_writestring(SHELL_PROMPT);
    terminal_enable_cursor();  // Enable cursor now that we're ready for input
}

/**
 * Process a character input to the shell
 */
void shell_process_char(char c) {
    // Handle escape sequences for arrows
    if (esc_state == ESC_IDLE) {
        if ((unsigned char)c == 0x1B) { esc_state = ESC_GOT_ESC; return; }
    } else if (esc_state == ESC_GOT_ESC) {
        if (c == '[') { esc_state = ESC_GOT_BRACKET; return; }
        esc_state = ESC_IDLE; // unknown, fall through
    } else if (esc_state == ESC_GOT_BRACKET) {
        // Arrow dispatch
        if (c == 'A') { // Up
            if (history_len > 0) {
                if (history_pos == history_len) {
                    strncpy(edit_backup, command_buffer, MAX_COMMAND_LENGTH-1);
                    edit_backup[MAX_COMMAND_LENGTH-1] = '\0';
                }
                if (history_pos > 0) history_pos--;
                shell_set_line(history[history_pos]);
            }
        } else if (c == 'B') { // Down
            if (history_len > 0) {
                if (history_pos < history_len) history_pos++;
                if (history_pos == history_len) {
                    shell_set_line(edit_backup);
                } else {
                    shell_set_line(history[history_pos]);
                }
            }
        }
        esc_state = ESC_IDLE;
        return;
    }

    if (c == '\n' || c == '\r') {
        terminal_putchar('\n');
        command_buffer[command_length] = '\0';
        if (command_length > 0) history_add(command_buffer);
        history_pos = history_len; // reset browse point
        shell_execute_command(command_buffer);
        command_length = 0;
        command_buffer[0] = '\0';
        terminal_writestring(SHELL_PROMPT);
    } else if (c == '\b') {
        if (command_length > 0) {
            command_length--;
            terminal_putchar('\b');
        }
    } else if (c >= ' ' && command_length < MAX_COMMAND_LENGTH - 1) {
        command_buffer[command_length++] = c;
        command_buffer[command_length] = '\0';
        terminal_putchar(c);
    }
}

/**
 * Execute a shell command
 */
static void shell_execute_command(const char* command) {
    // Skip leading whitespace
    while (*command == ' ') {
        command++;
    }
    
    // Ignore empty commands
    if (*command == '\0') {
        return;
    }
    
    // Extract command name (first word)
    char cmd_name[32];
    size_t i = 0;
    while (command[i] != ' ' && command[i] != '\0' && i < 31) {
        cmd_name[i] = command[i];
        i++;
    }
    cmd_name[i] = '\0';
    
    // Find arguments (everything after the first word)
    const char* args = "";
    if (command[i] != '\0') {
        args = &command[i + 1];
        // Skip leading whitespace in args
        while (*args == ' ') {
            args++;
        }
    }
    
    // Search for the command in the command table
    for (i = 0; commands[i].name != NULL; i++) {
        if (strcmp(cmd_name, commands[i].name) == 0) {
            commands[i].handler(args);
            return;
        }
    }
    
    // Command not found
    terminal_writestring("Command not found: ");
    terminal_writestring(cmd_name);
    terminal_writestring("\nType 'help' for a list of commands.\n");
}

/**
 * Update shell state - check for and process input
 */
void shell_update(void) {
    // This is called from the main loop when keyboard data is available
    char c = keyboard_getchar();
    if (c != 0) {
        shell_process_char(c);
    }
}

// Command Implementations

/**
 * Help command - display available commands
 */
static void cmd_help(const char* args) {
    (void)args; // Unused
    
    terminal_writestring("Available commands:\n");
    for (size_t i = 0; commands[i].name != NULL; i++) {
        terminal_writestring("  ");
        terminal_writestring(commands[i].name);
        terminal_writestring(" - ");
        terminal_writestring(commands[i].description);
        terminal_writestring("\n");
    }
}

/**
 * Echo command - echo arguments back to the console
 */
static void cmd_echo(const char* args) {
    terminal_writestring(args);
    terminal_writestring("\n");
}

/**
 * Color command - change text color
 */
static void cmd_color(const char* args) {
    if (strcmp(args, "red") == 0) {
        terminal_setcolor(0xFF0000);
        terminal_writestring("Color changed to red\n");
    } else if (strcmp(args, "green") == 0) {
        terminal_setcolor(0x00FF00);
        terminal_writestring("Color changed to green\n");
    } else if (strcmp(args, "blue") == 0) {
        terminal_setcolor(0x0000FF);
        terminal_writestring("Color changed to blue\n");
    } else if (strcmp(args, "white") == 0) {
        terminal_setcolor(0xFFFFFF);
        terminal_writestring("Color changed to white\n");
    } else {
        terminal_writestring("Usage: color [red|green|blue|white]\n");
    }
}

/**
 * Clear screen command
 */
static void cmd_cls(const char* args) {
    (void)args;
    uint32_t cursor_color = terminal_get_color();
    terminal_initialize();      // reset buffers and clear screen
    terminal_enable_cursor();   // show cursor again
    terminal_setcolor(cursor_color); // restore text color
}

/**
 * About command - show information about the OS
 */
static void cmd_about(const char* args) {
    (void)args; // Unused
    
    terminal_writestring("DeanOS - A minimal operating system\n");
    terminal_writestring("Created as a learning project\n");
    terminal_writestring("Version 0.1\n");
}

static void cmd_dean(const char* args) {
    (void)args; // Unused

    terminal_setscale(2);
    terminal_writestring(" _____                    ____   _____ \n");
    terminal_writestring("|  __ \\                  / __ \\ / ____|\n");
    terminal_writestring("| |  | | ___  __ _ _ __ | |  | | (___  \n");
    terminal_writestring("| |  | |/ _ \\/ _` | '_ \\| |  | |\\___ \\\n");
    terminal_writestring("| |__| |  __/ (_| | | | | |__| |____) |\n");
    terminal_writestring("|_____/ \\___|\\__,_|_| |_|\\____/|_____/\n");
    terminal_setscale(1);
    
}

/**
 * Time command - display current time and date
 */
static void cmd_time(const char* args) {
    (void)args; // Unused
    
    rtc_time_t time;
    rtc_read_time(&time);
    
    // Display date
    terminal_writestring("Date: ");
    char buffer[16];
    
    // Day
    itoa(time.day, buffer, 10);
    if (time.day < 10) terminal_writestring("0");
    terminal_writestring(buffer);
    terminal_writestring("/");
    
    // Month
    itoa(time.month, buffer, 10);
    if (time.month < 10) terminal_writestring("0");
    terminal_writestring(buffer);
    terminal_writestring("/");
    
    // Year
    itoa(time.year, buffer, 10);
    terminal_writestring(buffer);
    terminal_writestring("\n");
    
    // Display time
    terminal_writestring("Time: ");
    
    // Hour
    itoa(time.hour, buffer, 10);
    if (time.hour < 10) terminal_writestring("0");
    terminal_writestring(buffer);
    terminal_writestring(":");
    
    // Minute
    itoa(time.minute, buffer, 10);
    if (time.minute < 10) terminal_writestring("0");
    terminal_writestring(buffer);
    terminal_writestring(":");
    
    // Second
    itoa(time.second, buffer, 10);
    if (time.second < 10) terminal_writestring("0");
    terminal_writestring(buffer);
    terminal_writestring("\n");
}

/**
 * Uptime command - display system uptime
 */
static void cmd_uptime(const char* args) {
    (void)args; // Unused
    
    uptime_t uptime;
    get_uptime(&uptime);
    
    terminal_writestring("System uptime: ");
    char buffer[16];
    
    if (uptime.days > 0) {
        itoa(uptime.days, buffer, 10);
        terminal_writestring(buffer);
        terminal_writestring(" day");
        if (uptime.days != 1) terminal_writestring("s");
        terminal_writestring(", ");
    }
    
    // Hours
    itoa(uptime.hours, buffer, 10);
    if (uptime.hours < 10) terminal_writestring("0");
    terminal_writestring(buffer);
    terminal_writestring(":");
    
    // Minutes
    itoa(uptime.minutes, buffer, 10);
    if (uptime.minutes < 10) terminal_writestring("0");
    terminal_writestring(buffer);
    terminal_writestring(":");
    
    // Seconds
    itoa(uptime.seconds, buffer, 10);
    if (uptime.seconds < 10) terminal_writestring("0");
    terminal_writestring(buffer);
    terminal_writestring("\n");
}

static void cmd_ticks(const char* args) {
    (void)args;
    
    uint64_t ticks = pit_get_ticks();
    uint64_t uptime_ms = pit_get_uptime_ms();
    
    char buffer[32];
    
    terminal_writestring("Ticks since boot: ");
    itoa((int)ticks, buffer, 10);
    terminal_writestring(buffer);
    terminal_writestring("\n");
    
    terminal_writestring("Uptime: ");
    itoa((int)uptime_ms, buffer, 10);
    terminal_writestring(buffer);
    terminal_writestring(" ms\n");
}

static void history_add(const char* cmd) {
    if (!cmd || !*cmd) { history_pos = history_len; return; }
    if (history_len > 0 && strcmp(history[history_len-1], cmd) == 0) {
        history_pos = history_len;
        return;
    }
    if (history_len < SHELL_HISTORY_SIZE) {
        strncpy(history[history_len], cmd, MAX_COMMAND_LENGTH-1);
        history[history_len][MAX_COMMAND_LENGTH-1] = '\0';
        history_len++;
    } else {
        for (size_t i = 1; i < SHELL_HISTORY_SIZE; i++) {
            strncpy(history[i-1], history[i], MAX_COMMAND_LENGTH);
        }
        strncpy(history[SHELL_HISTORY_SIZE-1], cmd, MAX_COMMAND_LENGTH-1);
        history[SHELL_HISTORY_SIZE-1][MAX_COMMAND_LENGTH-1] = '\0';
    }
    history_pos = history_len;
}

static void shell_set_line(const char* s) {
    // erase current line (only the editable part, not prompt)
    while (command_length > 0) {
        terminal_putchar('\b'); // your terminal erases on backspace
        command_length--;
    }
    // write new content
    if (s && *s) {
        size_t i = 0;
        while (s[i] && i < MAX_COMMAND_LENGTH-1) {
            command_buffer[i] = s[i];
            terminal_putchar(s[i]);
            i++;
        }
        command_buffer[i] = '\0';
        command_length = i;
    } else {
        command_buffer[0] = '\0';
        command_length = 0;
    }
}