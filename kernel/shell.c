#include <kernel/shell.h>
#include <kernel/tty.h>
#include <kernel/keyboard.h>  // Add this include
#include <string.h>
#include <stdio.h>

#define SHELL_PROMPT "DeanOS> "
#define MAX_COMMAND_LENGTH 256

static char command_buffer[MAX_COMMAND_LENGTH];
static size_t command_length = 0;

// Add this declaration near the top of shell.c to fix the cmd_cls function
extern void framebuffer_clear(uint32_t color);

// Command handler function type
typedef void (*command_handler_t)(const char* args);

// Command structure
typedef struct {
    const char* name;
    command_handler_t handler;
    const char* description;
} command_t;

// Forward declarations of command handlers
static void cmd_help(const char* args);
static void cmd_echo(const char* args);
static void cmd_color(const char* args);
static void cmd_cls(const char* args);
static void cmd_about(const char* args);

// Command table
static const command_t commands[] = {
    {"help", cmd_help, "Display this help message"},
    {"echo", cmd_echo, "Echo the arguments back to the console"},
    {"color", cmd_color, "Change text color (usage: color [red|green|blue|white])"},
    {"cls", cmd_cls, "Clear the screen"},
    {"about", cmd_about, "Show information about DeanOS"},
    {NULL, NULL, NULL} // End of table marker
};

/**
 * Initialize the shell
 */
void shell_initialize(void) {
    command_length = 0;
    terminal_writestring(SHELL_PROMPT);
}

/**
 * Process a character input to the shell
 */
void shell_process_char(char c) {
    if (c == '\n' || c == '\r') {
        // Process the command when Enter is pressed
        terminal_putchar('\n');
        command_buffer[command_length] = '\0';
        shell_execute_command(command_buffer);
        command_length = 0;
        terminal_writestring(SHELL_PROMPT);
    } else if (c == '\b') {
        // Handle backspace
        if (command_length > 0) {
            command_length--;
            terminal_putchar('\b');
            terminal_putchar(' ');
            terminal_putchar('\b');
        }
    } else if (c >= ' ' && c <= '~' && command_length < MAX_COMMAND_LENGTH - 1) {
        // Handle printable characters
        command_buffer[command_length++] = c;
        terminal_putchar(c);
    }
}

/**
 * Execute a shell command
 */
void shell_execute_command(const char* command) {
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
    (void)args; // Unused
    
    framebuffer_clear(0x000000);  // Clear screen to black
    terminal_initialize();
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