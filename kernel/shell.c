#include "include/kernel/shell.h"
#include "include/kernel/tty.h"
#include "include/kernel/keyboard.h"
#include "include/kernel/mouse.h"
#include "include/kernel/rtc.h"
#include "include/kernel/pit.h"
#include "include/kernel/task.h"
#include "include/kernel/usermode.h"
#include "include/kernel/syscall.h"   // SYS_write, SYS_time, SYS_exit
#include "include/kernel/vfs.h"
#include "include/kernel/elf.h"
#include "include/kernel/log.h"
#include "include/kernel/blockdev.h"
#include "include/kernel/mbr.h"
#include "include/kernel/minfs.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_COMMAND_LENGTH 256
#define SHELL_HISTORY_SIZE 32

static char cwd[VFS_PATH_MAX] = "/";   /* current working directory */

static char command_buffer[MAX_COMMAND_LENGTH];
static size_t command_length = 0;
static size_t cursor_pos = 0;   /* position of terminal cursor within command_buffer */

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
static const char* resolve_shell_path(const char* arg, char* buf, size_t bufsz);
static void shell_autocomplete(void);

// New forward decls (syscall test commands)
static void cmd_sys_write(const char* args);
static void cmd_sys_time(const char* args);
static void cmd_sys81_time(const char* args);
static void cmd_sys_exit(const char* args);

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
static void cmd_tasks(const char* args);
static void cmd_libctest(const char* args);
static void cmd_mouse(const char* args);
static void cmd_dmesg(const char* args);
static void cmd_blk(const char* args);
static void cmd_disk(const char* args);
static void cmd_fsfill(const char* args);
static void cmd_fsverify(const char* args);

// Filesystem commands
static void cmd_ls(const char* args);
static void cmd_cat(const char* args);
static void cmd_touch(const char* args);
static void cmd_writef(const char* args);
static void cmd_mkdir(const char* args);
static void cmd_rm(const char* args);
static void cmd_stat(const char* args);
static void cmd_cd(const char* args);
static void cmd_pwd(const char* args);
static void cmd_exec(const char* args);
static void cmd_anim(const char* args);

/* Print the shell prompt: "DeanOS /path $ " */
static void shell_print_prompt(void);

/* Command descriptor */
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
    {"tasks",  cmd_tasks,  "List all tasks and their state"},
    {"mouse",  cmd_mouse,  "Show PS/2 mouse state (mouse clear resets totals)"},
    {"blk",    cmd_blk,    "Block devices: blk list | blk read <dev> <lba> | blk write <dev> <lba> <seed>"},
    {"disk",   cmd_disk,   "Disk tools: disk parts | init | mkfs | mount | setup"},
    {"fsfill", cmd_fsfill, "Write a large patterned file: fsfill <path> <bytes> <seed>"},
    {"fsverify", cmd_fsverify, "Verify a patterned file: fsverify <path> <bytes> <seed>"},
    {"dmesg",  cmd_dmesg,  "Show kernel log buffer (use 'dmesg clear' to clear)"},
    {"libctest", cmd_libctest, "Run libc smoke tests (printf/malloc/io)"},

    // Filesystem commands
    {"ls",      cmd_ls,      "List directory contents: ls [path]"},
    {"cat",     cmd_cat,     "Display file contents: cat <path>"},
    {"touch",   cmd_touch,   "Create an empty file: touch <path>"},
    {"write",   cmd_writef,  "Write text to file: write <path> <text>"},
    {"mkdir",   cmd_mkdir,   "Create a directory: mkdir <path>"},
    {"rm",      cmd_rm,      "Remove a file or empty directory: rm <path>"},
    {"stat",    cmd_stat,    "Show file/directory info: stat <path>"},
    {"cd",      cmd_cd,      "Change directory: cd <path>"},
    {"pwd",     cmd_pwd,     "Print working directory"},
    {"exec",    cmd_exec,    "Run an ELF program: exec <path>"},
    {"anim",    cmd_anim,    "Run large animated demo"},

    // New syscall test commands
    {"sys.write",   cmd_sys_write,   "Syscall write via int 0x80: sys.write <text>"},
    {"sys.time",    cmd_sys_time,    "Syscall time via int 0x80"},
    {"sys81.time",  cmd_sys81_time,  "Syscall time via int 0x81"},
    {"sys.exit",    cmd_sys_exit,    "Syscall exit (halts) via int 0x80: sys.exit <code>"},

    {NULL, NULL, NULL}
};

// Remove the old non-static forward line:
// - void cmd_cls(const char* args);
// ...existing code...

/**
 * Print the shell prompt: "DeanOS /path $ "
 */
static void shell_print_prompt(void) {
    terminal_writestring("DeanOS ");
    terminal_writestring(cwd);
    terminal_writestring(" $ ");
}

/**
 * Initialize the shell
 */
void shell_initialize(void) {
    command_length = 0;
    cursor_pos = 0;
    history_len = 0;
    history_pos = 0;
    esc_state = ESC_IDLE;
    cwd[0] = '/';
    cwd[1] = '\0';
    shell_print_prompt();
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
        } else if (c == 'D') { // Left
            if (cursor_pos > 0) {
                cursor_pos--;
                terminal_move_cursor_left();
            }
        } else if (c == 'C') { // Right
            if (cursor_pos < command_length) {
                cursor_pos++;
                terminal_move_cursor_right();
            }
        }
        esc_state = ESC_IDLE;
        return;
    }

    if (c == '\n' || c == '\r') {
        /* Move terminal cursor to end of input before printing newline */
        while (cursor_pos < command_length) {
            terminal_move_cursor_right();
            cursor_pos++;
        }
        terminal_putchar('\n');
        command_buffer[command_length] = '\0';
        if (command_length > 0) history_add(command_buffer);
        history_pos = history_len; // reset browse point
        shell_execute_command(command_buffer);
        command_length = 0;
        cursor_pos = 0;
        command_buffer[0] = '\0';
        shell_print_prompt();
    } else if (c == '\b') {
        if (cursor_pos > 0) {
            size_t chars_after = command_length - cursor_pos;
            terminal_move_cursor_left();
            memmove(&command_buffer[cursor_pos - 1], &command_buffer[cursor_pos], chars_after);
            cursor_pos--;
            command_length--;
            command_buffer[command_length] = '\0';
            /* Redraw tail and overwrite old last char with space */
            for (size_t i = cursor_pos; i < command_length; i++)
                terminal_putchar(command_buffer[i]);
            terminal_putchar(' ');
            /* Move cursor back to cursor_pos */
            for (size_t i = 0; i <= chars_after; i++)
                terminal_move_cursor_left();
        }
    } else if (c == '\t') {
        shell_autocomplete();
        cursor_pos = command_length; // autocomplete always appends to end
    } else {
        unsigned char uc = (unsigned char)c;
        if (uc >= ' ' && uc != 0x7F && command_length < MAX_COMMAND_LENGTH - 1) {
            /* Insert at cursor_pos */
            memmove(&command_buffer[cursor_pos + 1], &command_buffer[cursor_pos],
                    command_length - cursor_pos);
            command_buffer[cursor_pos] = c;
            command_length++;
            command_buffer[command_length] = '\0';
            /* Redraw from cursor_pos to end */
            for (size_t i = cursor_pos; i < command_length; i++)
                terminal_putchar(command_buffer[i]);
            /* Move terminal cursor back to cursor_pos + 1 */
            size_t move_back = command_length - cursor_pos - 1;
            for (size_t i = 0; i < move_back; i++)
                terminal_move_cursor_left();
            cursor_pos++;
        }
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
    terminal_writestring("Version 0.4\n");
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

/**
 * Tab-autocomplete: complete the current token against command names (first
 * word) or VFS paths (subsequent words).  Completes to the first match found.
 */
static void shell_autocomplete(void) {
    command_buffer[command_length] = '\0';

    /* Find the start of the current (last) word */
    const char* word_start = command_buffer;
    for (size_t i = 0; i < command_length; i++) {
        if (command_buffer[i] == ' ') {
            word_start = &command_buffer[i + 1];
        }
    }
    size_t word_len = (size_t)(command_buffer + command_length - word_start);

    if (word_start == command_buffer) {
        /* ---- Command name completion ---- */
        for (size_t i = 0; commands[i].name != NULL; i++) {
            if (strncmp(commands[i].name, word_start, word_len) == 0) {
                const char* rest = commands[i].name + word_len;
                while (*rest && command_length < MAX_COMMAND_LENGTH - 1) {
                    command_buffer[command_length++] = *rest;
                    terminal_putchar(*rest);
                    rest++;
                }
                command_buffer[command_length] = '\0';
                return;
            }
        }
    } else {
        /* ---- Path completion ---- */
        char partial[VFS_PATH_MAX];
        if (word_len >= VFS_PATH_MAX) return;
        strncpy(partial, word_start, word_len);
        partial[word_len] = '\0';

        /* Split partial path into directory part and name prefix */
        char dir_part[VFS_PATH_MAX];
        char name_prefix[VFS_NAME_MAX];

        const char* last_slash = NULL;
        for (size_t i = 0; i < word_len; i++) {
            if (partial[i] == '/') last_slash = &partial[i];
        }

        vfs_node_t* dir = NULL;
        size_t prefix_len;

        if (last_slash) {
            size_t dir_len = (size_t)(last_slash - partial) + 1; /* include slash */
            strncpy(dir_part, partial, dir_len);
            dir_part[dir_len] = '\0';
            strncpy(name_prefix, last_slash + 1, VFS_NAME_MAX - 1);
            name_prefix[VFS_NAME_MAX - 1] = '\0';
            prefix_len = strlen(name_prefix);

            char resolved[VFS_PATH_MAX];
            const char* rpath = resolve_shell_path(dir_part, resolved, sizeof(resolved));
            dir = vfs_namei(rpath);
        } else {
            strncpy(name_prefix, partial, VFS_NAME_MAX - 1);
            name_prefix[VFS_NAME_MAX - 1] = '\0';
            prefix_len = word_len;
            dir = vfs_namei(cwd);
        }

        if (!dir || !(dir->type & VFS_DIRECTORY)) return;

        /* Scan directory entries for the first match */
        vfs_dirent_t entry;
        uint32_t idx = 0;
        while (vfs_readdir(dir, idx, &entry) == 0) {
            if (strncmp(entry.name, name_prefix, prefix_len) == 0) {
                /* Append the rest of the matching name */
                const char* rest = entry.name + prefix_len;
                while (*rest && command_length < MAX_COMMAND_LENGTH - 1) {
                    command_buffer[command_length++] = *rest;
                    terminal_putchar(*rest);
                    rest++;
                }
                /* Append a trailing '/' for directories */
                if ((entry.type & VFS_DIRECTORY) && command_length < MAX_COMMAND_LENGTH - 1) {
                    command_buffer[command_length++] = '/';
                    terminal_putchar('/');
                }
                command_buffer[command_length] = '\0';
                return;
            }
            idx++;
        }
    }
}

static void shell_set_line(const char* s) {
    // Move terminal cursor to end of current input first
    while (cursor_pos < command_length) {
        terminal_move_cursor_right();
        cursor_pos++;
    }
    // Erase current line (only the editable part, not prompt)
    while (command_length > 0) {
        terminal_putchar('\b');
        command_length--;
    }
    cursor_pos = 0;
    // Write new content
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
    cursor_pos = command_length; // cursor at end
}

// Small helpers for syscalls and parsing
static long ksyscall3_vec(uint8_t vec, uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3) {
    register uint32_t eax asm("eax") = num;
    register uint32_t ebx asm("ebx") = a1;
    register uint32_t ecx asm("ecx") = a2;
    register uint32_t edx asm("edx") = a3;
    if (vec == 0x80) {
        __asm__ __volatile__("int $0x80" : "+a"(eax) : "b"(ebx), "c"(ecx), "d"(edx) : "memory", "cc");
    } else {
        __asm__ __volatile__("int $0x81" : "+a"(eax) : "b"(ebx), "c"(ecx), "d"(edx) : "memory", "cc");
    }
    return (long)eax;
}

static uint32_t parse_uint(const char* s) {
    while (*s == ' ') s++;
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10u + (uint32_t)(*s - '0'); s++; }
    return v;
}

static int is_decimal_token(const char* s) {
    if (!s || *s == '\0') return 0;
    while (*s && *s != ' ') {
        if (*s < '0' || *s > '9') return 0;
        s++;
    }
    return 1;
}

static size_t copy_token(const char* src, char* dst, size_t dstsz) {
    size_t i = 0;
    if (!dst || dstsz == 0) return 0;
    while (src && src[i] && src[i] != ' ' && i < dstsz - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return i;
}


static const block_device_t* resolve_blockdev_token(const char* s, char* token_buf, size_t token_buf_sz) {
    if (!s || !*s) return NULL;
    copy_token(s, token_buf, token_buf_sz);
    if (token_buf[0] == '\0') return NULL;
    if (is_decimal_token(token_buf)) {
        return blockdev_get(parse_uint(token_buf));
    }
    return blockdev_find_by_name(token_buf);
}

static const char* next_token(const char* s) {
    while (s && *s && *s != ' ') s++;
    while (s && *s == ' ') s++;
    return s;
}

static int is_leap_year_u32(uint32_t year) {
    return ((year % 4u) == 0u && (year % 100u) != 0u) || ((year % 400u) == 0u);
}

static uint32_t days_in_month_u32(uint32_t year, uint32_t month) {
    static const uint8_t mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2u && is_leap_year_u32(year)) return 29u;
    if (month >= 1u && month <= 12u) return mdays[month - 1u];
    return 30u;
}

static void term_write_u32(uint32_t v) {
    char buf[16];
    itoa((int)v, buf, 10);
    terminal_writestring(buf);
}

static void term_write_2d(uint32_t v) {
    if (v < 10u) terminal_writestring("0");
    term_write_u32(v);
}

static void term_write_hex8(uint8_t v) {
    char out[3];
    static const char* hex = "0123456789ABCDEF";
    out[0] = hex[(v >> 4) & 0xF];
    out[1] = hex[v & 0xF];
    out[2] = '\0';
    terminal_writestring(out);
}

static void term_write_wallclock_from_epoch(uint32_t epoch_seconds) {
    uint32_t days = epoch_seconds / 86400u;
    uint32_t rem = epoch_seconds % 86400u;
    uint32_t hour = rem / 3600u;
    rem %= 3600u;
    uint32_t minute = rem / 60u;
    uint32_t second = rem % 60u;

    uint32_t year = 1970u;
    while (1) {
        uint32_t ydays = is_leap_year_u32(year) ? 366u : 365u;
        if (days < ydays) break;
        days -= ydays;
        year++;
    }

    uint32_t month = 1u;
    while (1) {
        uint32_t mdays = days_in_month_u32(year, month);
        if (days < mdays) break;
        days -= mdays;
        month++;
    }
    uint32_t day = days + 1u;

    term_write_u32(year);
    terminal_writestring("-");
    term_write_2d(month);
    terminal_writestring("-");
    term_write_2d(day);
    terminal_writestring(" ");
    term_write_2d(hour);
    terminal_writestring(":");
    term_write_2d(minute);
    terminal_writestring(":");
    term_write_2d(second);
}

// Implementations

static void cmd_sys_write(const char* args) {
    if (!args || *args == '\0') {
        terminal_writestring("usage: sys.write <text>\n");
        return;
    }
    size_t len = strlen(args);
    long ret = ksyscall3_vec(0x80, SYS_write, 1, (uint32_t)args, (uint32_t)len);
    terminal_writestring("\n[sys.write returned ");
    char buf[16]; itoa((int)ret, buf, 10); terminal_writestring(buf); terminal_writestring("]\n");
}

static void cmd_sys_time(const char* args) {
    (void)args;
    uint32_t sec = (uint32_t)ksyscall3_vec(0x80, SYS_time, 0, 0, 0);
    terminal_writestring("time (int 0x80): ");
    term_write_wallclock_from_epoch(sec);
    terminal_writestring("\n");
}

static void cmd_sys81_time(const char* args) {
    (void)args;
    uint32_t sec = (uint32_t)ksyscall3_vec(0x81, SYS_time, 0, 0, 0);
    terminal_writestring("time (int 0x81): ");
    term_write_wallclock_from_epoch(sec);
    terminal_writestring("\n");
}

static void cmd_sys_exit(const char* args) {
    uint32_t code = parse_uint(args ? args : "");
    terminal_writestring("calling exit(\n");
    char buf[16]; itoa((int)code, buf, 10); terminal_writestring(buf); terminal_writestring(")...\n");
    (void)ksyscall3_vec(0x80, SYS_exit, code, 0, 0);
    // Not reached: sys_exit halts.
}

static void cmd_tasks(const char* args) {
    (void)args;
    static const char* state_names[] = {"READY","RUN  ","BLOCK","DEAD "};
    char buf[16];

    terminal_writestring("ID  STATE  QUANTUM  NAME\n");
    terminal_writestring("--  -----  -------  ----\n");

    uint32_t count = task_count();
    int cur = task_current_id();

    for (uint32_t i = 0; i < count; ++i) {
        const task_t* t = task_get(i);
        if (!t) continue;

        /* ID (with * for current) */
        itoa(t->id, buf, 10);
        if ((int)t->id == cur) {
            terminal_writestring("*");
        } else {
            terminal_writestring(" ");
        }
        terminal_writestring(buf);
        terminal_writestring("  ");

        /* State */
        if (t->state <= TASK_DEAD)
            terminal_writestring(state_names[t->state]);
        else
            terminal_writestring("???? ");
        terminal_writestring("  ");

        /* Quantum */
        itoa(t->quantum, buf, 10);
        terminal_writestring(buf);
        terminal_writestring("        ");

        /* Name */
        terminal_writestring(t->name);
        terminal_writestring("\n");
    }
}

static void cmd_dmesg(const char* args) {
    if (args && (strcmp(args, "clear") == 0 || strcmp(args, "-c") == 0)) {
        klog_clear();
        terminal_writestring("dmesg: log buffer cleared\n");
        return;
    }

    if (args && *args) {
        terminal_writestring("usage: dmesg [clear|-c]\n");
        return;
    }

    terminal_writestring("--- kernel log (latest) ---\n");
    klog_dump();
    terminal_writestring("\n--- end kernel log ---\n");
}

static void cmd_mouse(const char* args) {
    if (args && (strcmp(args, "clear") == 0 || strcmp(args, "reset") == 0)) {
        mouse_reset_counters();
        terminal_writestring("mouse: counters cleared\n");
        return;
    }

    if (args && *args) {
        terminal_writestring("usage: mouse [clear]\n");
        return;
    }

    mouse_state_t st;
    mouse_get_state(&st);

    terminal_writestring("mouse ready: ");
    terminal_writestring(mouse_is_ready() ? "yes\n" : "no\n");

    char buf[24];
    terminal_writestring("packets: ");
    itoa((int)st.packet_count, buf, 10);
    terminal_writestring(buf);
    terminal_writestring("\nbuttons: ");
    itoa((int)st.buttons, buf, 10);
    terminal_writestring(buf);
    terminal_writestring(" (L=");
    terminal_writestring((st.buttons & 0x1) ? "1" : "0");
    terminal_writestring(" M=");
    terminal_writestring((st.buttons & 0x4) ? "1" : "0");
    terminal_writestring(" R=");
    terminal_writestring((st.buttons & 0x2) ? "1" : "0");
    terminal_writestring(")\n");

    terminal_writestring("pos: x=");
    itoa(st.x, buf, 10);
    terminal_writestring(buf);
    terminal_writestring(" y=");
    itoa(st.y, buf, 10);
    terminal_writestring(buf);
    terminal_writestring("\n");

    terminal_writestring("motion total: dx=");
    itoa(st.dx_total, buf, 10);
    terminal_writestring(buf);
    terminal_writestring(" dy=");
    itoa(st.dy_total, buf, 10);
    terminal_writestring(buf);
    terminal_writestring("\n");

    terminal_writestring("overflow: x=");
    terminal_writestring(st.x_overflow ? "1" : "0");
    terminal_writestring(" y=");
    terminal_writestring(st.y_overflow ? "1" : "0");
    terminal_writestring("\n");

    terminal_writestring("clicks: L=");
    itoa((int)st.left_clicks, buf, 10);
    terminal_writestring(buf);
    terminal_writestring(" M=");
    itoa((int)st.middle_clicks, buf, 10);
    terminal_writestring(buf);
    terminal_writestring(" R=");
    itoa((int)st.right_clicks, buf, 10);
    terminal_writestring(buf);
    terminal_writestring("\n");
}

static void cmd_blk(const char* args) {
    if (!args || *args == '\0' || strcmp(args, "list") == 0) {
        uint32_t n = blockdev_count();
        char buf[24];

        terminal_writestring("ID  NAME  BSIZE  BLOCKS  FLAGS\n");
        terminal_writestring("--  ----  -----  ------  -----\n");
        for (uint32_t i = 0; i < n; ++i) {
            const block_device_t* d = blockdev_get(i);
            if (!d) continue;

            itoa((int)d->id, buf, 10);
            terminal_writestring(buf);
            terminal_writestring("   ");
            terminal_writestring(d->name);
            terminal_writestring("    ");

            itoa((int)d->block_size, buf, 10);
            terminal_writestring(buf);
            terminal_writestring("    ");

            itoa((int)d->block_count, buf, 10);
            terminal_writestring(buf);
            terminal_writestring("    ");

            if (d->flags & BLOCKDEV_FLAG_READONLY) terminal_writestring("RO ");
            if (d->flags & BLOCKDEV_FLAG_ATAPI) terminal_writestring("ATAPI ");
            if (d->flags & BLOCKDEV_FLAG_PARTITION) terminal_writestring("PART ");
            if (d->flags == 0) terminal_writestring("-");
            terminal_writestring("\n");
        }
        if (n == 0) terminal_writestring("(no block devices)\n");
        return;
    }

    if (strncmp(args, "read ", 5) == 0) {
        const char* p = args + 5;
        uint32_t dev = parse_uint(p);
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        if (!*p) {
            terminal_writestring("usage: blk read <dev> <lba>\n");
            return;
        }
        uint32_t lba = parse_uint(p);

        const block_device_t* d = blockdev_get(dev);
        if (!d) {
            terminal_writestring("blk: invalid device id\n");
            return;
        }
        if (d->block_size > 2048u) {
            terminal_writestring("blk: block size too large for shell dump\n");
            return;
        }

        uint8_t sector[2048];
        int rc = blockdev_read(dev, lba, 1, sector);
        if (rc < 0) {
            terminal_writestring("blk: read failed\n");
            return;
        }

        terminal_writestring("blk: first 64 bytes\n");
        for (uint32_t i = 0; i < 64; ++i) {
            term_write_hex8(sector[i]);
            if ((i & 0x0Fu) == 0x0Fu) terminal_writestring("\n");
            else terminal_writestring(" ");
        }

        if (d->block_size >= 512u) {
            terminal_writestring("sig[510..511]=");
            term_write_hex8(sector[510]);
            terminal_writestring(" ");
            term_write_hex8(sector[511]);
            terminal_writestring("\n");
        }
        return;
    }

    if (strncmp(args, "write ", 6) == 0) {
        const char* p = args + 6;
        uint32_t dev = parse_uint(p);
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        if (!*p) {
            terminal_writestring("usage: blk write <dev> <lba> <seed-byte>\n");
            return;
        }

        uint32_t lba = parse_uint(p);
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        if (!*p) {
            terminal_writestring("usage: blk write <dev> <lba> <seed-byte>\n");
            return;
        }

        uint32_t seed = parse_uint(p);
        if (seed > 255u) {
            terminal_writestring("blk: seed-byte must be 0..255\n");
            return;
        }

        const block_device_t* d = blockdev_get(dev);
        if (!d) {
            terminal_writestring("blk: invalid device id\n");
            return;
        }
        if (d->block_size > 2048u) {
            terminal_writestring("blk: block size too large for shell write test\n");
            return;
        }
        if (d->flags & BLOCKDEV_FLAG_ATAPI) {
            terminal_writestring("blk: refusing writes to ATAPI device\n");
            return;
        }

        uint8_t tx[2048];
        uint8_t rx[2048];
        for (uint32_t i = 0; i < d->block_size; ++i) {
            tx[i] = (uint8_t)((seed + i) & 0xFFu);
            rx[i] = 0;
        }

        int wrc = blockdev_write(dev, lba, 1, tx);
        if (wrc < 0) {
            terminal_writestring("blk: write failed\n");
            return;
        }

        int rrc = blockdev_read(dev, lba, 1, rx);
        if (rrc < 0) {
            terminal_writestring("blk: readback failed\n");
            return;
        }

        int mismatch = -1;
        for (uint32_t i = 0; i < d->block_size; ++i) {
            if (rx[i] != tx[i]) {
                mismatch = (int)i;
                break;
            }
        }

        if (mismatch >= 0) {
            terminal_writestring("blk: verify mismatch at byte ");
            char nbuf[24];
            itoa(mismatch, nbuf, 10);
            terminal_writestring(nbuf);
            terminal_writestring(" (expected ");
            term_write_hex8(tx[(uint32_t)mismatch]);
            terminal_writestring(", got ");
            term_write_hex8(rx[(uint32_t)mismatch]);
            terminal_writestring(")\n");
            return;
        }

        terminal_writestring("blk: write+readback verify ok\n");
        return;
    }

    terminal_writestring("usage: blk list | blk read <dev> <lba> | blk write <dev> <lba> <seed-byte>\n");
}

static void cmd_disk(const char* args) {
    if (!args || *args == '\0' || strcmp(args, "help") == 0) {
        terminal_writestring("usage:\n");
        terminal_writestring("  disk parts\n");
        terminal_writestring("  disk init <disk>\n");
        terminal_writestring("  disk mkfs <partition>\n");
        terminal_writestring("  disk mount <partition>\n");
        terminal_writestring("  disk setup <disk>\n");
        return;
    }

    if (strcmp(args, "parts") == 0) {
        mbr_scan_all();
        uint32_t n = mbr_partition_count();
        if (n == 0) {
            terminal_writestring("disk: no MBR partitions detected\n");
            return;
        }

        terminal_writestring("DEV     PARENT  TYPE  START   BLOCKS\n");
        terminal_writestring("------  ------  ----  ------  ------\n");
        for (uint32_t i = 0; i < n; ++i) {
            const mbr_partition_info_t* p = mbr_partition_get(i);
            if (!p) continue;
            terminal_writestring(p->name);
            terminal_writestring("   ");

            const block_device_t* parent = blockdev_get(p->parent_index);
            terminal_writestring(parent ? parent->name : "?");
            terminal_writestring("    ");

            term_write_hex8(p->partition_type);
            terminal_writestring("    ");

            char buf[24];
            itoa((int)p->start_lba, buf, 10);
            terminal_writestring(buf);
            terminal_writestring("    ");

            itoa((int)p->block_count, buf, 10);
            terminal_writestring(buf);
            terminal_writestring("\n");
        }
        return;
    }

    if (strncmp(args, "init ", 5) == 0 || strncmp(args, "mkfs ", 5) == 0 ||
        strncmp(args, "mount ", 6) == 0 || strncmp(args, "setup ", 6) == 0) {
        int is_init = strncmp(args, "init ", 5) == 0;
        int is_mkfs = strncmp(args, "mkfs ", 5) == 0;
        int is_mount = strncmp(args, "mount ", 6) == 0;
        int is_setup = strncmp(args, "setup ", 6) == 0;
        const char* p = args + (is_init || is_mkfs ? 5 : 6);
        char devtok[16];
        const block_device_t* dev = resolve_blockdev_token(p, devtok, sizeof(devtok));
        if (!dev) {
            terminal_writestring("disk: unknown device\n");
            return;
        }

        if (is_init) {
            int rc = mbr_create_single_partition(dev->id, 0x83);
            if (rc < 0) {
                terminal_writestring("disk: failed to write MBR\n");
                return;
            }
            mbr_scan_all();
            terminal_writestring("disk: created single partition table on ");
            terminal_writestring(dev->name);
            terminal_writestring("\n");
            return;
        }

        if (is_mkfs) {
            int rc = minfs_format(dev->id);
            if (rc < 0) {
                terminal_writestring("disk: mkfs failed\n");
                return;
            }
            terminal_writestring("disk: minfs formatted on ");
            terminal_writestring(dev->name);
            terminal_writestring("\n");
            return;
        }

        if (is_mount) {
            int rc = minfs_mount(dev->id, NULL);
            if (rc < 0) {
                terminal_writestring("disk: mount failed\n");
                return;
            }
            terminal_writestring("disk: mounted /mnt/");
            terminal_writestring(dev->name);
            terminal_writestring("\n");
            return;
        }

        if (is_setup) {
            int rc = mbr_create_single_partition(dev->id, 0x83);
            if (rc < 0) {
                terminal_writestring("disk: setup failed while creating MBR\n");
                return;
            }
            mbr_scan_all();

            char pname[16];
            strncpy(pname, dev->name, sizeof(pname) - 3);
            pname[sizeof(pname) - 3] = '\0';
            strcat(pname, "p1");

            const block_device_t* part = blockdev_find_by_name(pname);
            if (!part) {
                terminal_writestring("disk: setup could not find new partition\n");
                return;
            }
            if (minfs_format(part->id) < 0) {
                terminal_writestring("disk: setup format failed\n");
                return;
            }
            if (minfs_mount(part->id, NULL) < 0) {
                terminal_writestring("disk: setup mount failed\n");
                return;
            }
            terminal_writestring("disk: ready at /mnt/");
            terminal_writestring(part->name);
            terminal_writestring("\n");
            return;
        }
    }

    terminal_writestring("usage: disk parts | disk init <disk> | disk mkfs <partition> | disk mount <partition> | disk setup <disk>\n");
}

static void cmd_fsfill(const char* args) {
    if (!args || *args == '\0') {
        terminal_writestring("usage: fsfill <path> <bytes> <seed>\n");
        return;
    }

    char path_arg[VFS_PATH_MAX];
    copy_token(args, path_arg, sizeof(path_arg));
    const char* p = next_token(args);
    if (!*path_arg || !*p) {
        terminal_writestring("usage: fsfill <path> <bytes> <seed>\n");
        return;
    }

    uint32_t total = parse_uint(p);
    p = next_token(p);
    if (!*p) {
        terminal_writestring("usage: fsfill <path> <bytes> <seed>\n");
        return;
    }
    uint32_t seed = parse_uint(p) & 0xFFu;

    char pathbuf[VFS_PATH_MAX];
    const char* path = resolve_shell_path(path_arg, pathbuf, sizeof(pathbuf));
    int fd = vfs_fd_open(path, VFS_O_RDWR | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) {
        terminal_writestring("fsfill: cannot open: ");
        terminal_writestring(path);
        terminal_writestring("\n");
        return;
    }

    uint8_t chunk[256];
    uint32_t written_total = 0;
    while (written_total < total) {
        uint32_t chunk_size = total - written_total;
        if (chunk_size > sizeof(chunk)) chunk_size = sizeof(chunk);
        for (uint32_t i = 0; i < chunk_size; ++i) {
            chunk[i] = (uint8_t)((seed + written_total + i) & 0xFFu);
        }

        int32_t n = vfs_fd_write(fd, chunk, chunk_size);
        if (n <= 0) {
            terminal_writestring("fsfill: write failed\n");
            vfs_fd_close(fd);
            return;
        }
        written_total += (uint32_t)n;
    }

    vfs_fd_close(fd);
    terminal_writestring("fsfill: wrote ");
    char buf[24];
    itoa((int)written_total, buf, 10);
    terminal_writestring(buf);
    terminal_writestring(" bytes to ");
    terminal_writestring(path);
    terminal_writestring("\n");
}

static void cmd_fsverify(const char* args) {
    if (!args || *args == '\0') {
        terminal_writestring("usage: fsverify <path> <bytes> <seed>\n");
        return;
    }

    char path_arg[VFS_PATH_MAX];
    copy_token(args, path_arg, sizeof(path_arg));
    const char* p = next_token(args);
    if (!*path_arg || !*p) {
        terminal_writestring("usage: fsverify <path> <bytes> <seed>\n");
        return;
    }

    uint32_t total = parse_uint(p);
    p = next_token(p);
    if (!*p) {
        terminal_writestring("usage: fsverify <path> <bytes> <seed>\n");
        return;
    }
    uint32_t seed = parse_uint(p) & 0xFFu;

    char pathbuf[VFS_PATH_MAX];
    const char* path = resolve_shell_path(path_arg, pathbuf, sizeof(pathbuf));
    vfs_node_t* node = vfs_namei(path);
    if (!node || !(node->type & VFS_FILE)) {
        terminal_writestring("fsverify: no such file: ");
        terminal_writestring(path);
        terminal_writestring("\n");
        return;
    }
    if (node->size != total) {
        terminal_writestring("fsverify: size mismatch\n");
        return;
    }

    int fd = vfs_fd_open(path, VFS_O_RDONLY);
    if (fd < 0) {
        terminal_writestring("fsverify: cannot open: ");
        terminal_writestring(path);
        terminal_writestring("\n");
        return;
    }

    uint8_t chunk[256];
    uint32_t checked = 0;
    while (checked < total) {
        uint32_t chunk_size = total - checked;
        if (chunk_size > sizeof(chunk)) chunk_size = sizeof(chunk);

        int32_t n = vfs_fd_read(fd, chunk, chunk_size);
        if (n != (int32_t)chunk_size) {
            terminal_writestring("fsverify: short read\n");
            vfs_fd_close(fd);
            return;
        }

        for (uint32_t i = 0; i < chunk_size; ++i) {
            uint8_t expected = (uint8_t)((seed + checked + i) & 0xFFu);
            if (chunk[i] != expected) {
                terminal_writestring("fsverify: mismatch at byte ");
                char buf[24];
                itoa((int)(checked + i), buf, 10);
                terminal_writestring(buf);
                terminal_writestring(" (expected ");
                term_write_hex8(expected);
                terminal_writestring(", got ");
                term_write_hex8(chunk[i]);
                terminal_writestring(")\n");
                vfs_fd_close(fd);
                return;
            }
        }

        checked += chunk_size;
    }

    vfs_fd_close(fd);
    terminal_writestring("fsverify: ok\n");
}

static void cmd_libctest(const char* args) {
    (void)args;

    int failures = 0;
    printf("[libctest] begin\n");

    /* printf smoke */
    int printed = printf("[libctest] printf: %s %d 0x%x %u %c %p %%\n",
                         "ok", -42, 0x2a, 42u, 'Z', (void*)cwd);
    if (printed < 0) {
        terminal_writestring("[libctest] FAIL printf\n");
        failures++;
    } else {
        terminal_writestring("[libctest] PASS printf\n");
    }

    /* allocator smoke */
    char* a = (char*)malloc(16);
    char* z = (char*)calloc(8, 1);
    char* a2 = NULL;
    if (!a || !z) {
        terminal_writestring("[libctest] FAIL alloc: malloc/calloc returned NULL\n");
        failures++;
    } else {
        strcpy(a, "hello");
        a2 = (char*)realloc(a, 32);
        if (!a2 || strcmp(a2, "hello") != 0) {
            terminal_writestring("[libctest] FAIL alloc: realloc/contents\n");
            failures++;
        } else {
            int zeros_ok = 1;
            for (size_t i = 0; i < 8; i++) {
                if (z[i] != 0) {
                    zeros_ok = 0;
                    break;
                }
            }
            if (!zeros_ok) {
                terminal_writestring("[libctest] FAIL alloc: calloc not zeroed\n");
                failures++;
            } else {
                terminal_writestring("[libctest] PASS alloc\n");
            }
        }
        if (a2) {
            free(a2);
        } else if (a) {
            free(a);
        }
        free(z);
    }

    /* file I/O wrapper smoke */
    const char* path = "/libctest.tmp";
    const char* text = "libc-io-ok";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC);
    if (fd < 0) {
        terminal_writestring("[libctest] FAIL io: open(write)\n");
        failures++;
    } else {
        ssize_t nwr = write(fd, text, strlen(text));
        struct stat st;
        int sret = fstat(fd, &st);
        int cret = close(fd);
        if (nwr != (ssize_t)strlen(text) || sret < 0 || cret < 0 || st.size != (uint32_t)strlen(text)) {
            terminal_writestring("[libctest] FAIL io: write/fstat/close\n");
            failures++;
        } else {
            char buf[32];
            int fd2 = open(path, O_RDONLY);
            if (fd2 < 0) {
                terminal_writestring("[libctest] FAIL io: open(read)\n");
                failures++;
            } else {
                ssize_t nrd = read(fd2, buf, sizeof(buf) - 1);
                int cret2 = close(fd2);
                if (nrd < 0 || cret2 < 0) {
                    terminal_writestring("[libctest] FAIL io: read/close\n");
                    failures++;
                } else {
                    buf[nrd] = '\0';
                    if (strcmp(buf, text) != 0) {
                        terminal_writestring("[libctest] FAIL io: content mismatch\n");
                        failures++;
                    } else {
                        terminal_writestring("[libctest] PASS io\n");
                    }
                }
            }
        }
    }

    if (failures == 0) {
        terminal_writestring("[libctest] ALL PASS\n");
    } else {
        terminal_writestring("[libctest] FAILURES: ");
        char num[16];
        itoa(failures, num, 10);
        terminal_writestring(num);
        terminal_writestring("\n");
    }
}


/* ---- Filesystem commands ----------------------------------------------- */

/*
 * Resolve a path for shell commands.
 * Absolute paths ("/foo/bar") are used as-is.
 * Relative paths are joined with the current working directory.
 */
static const char* resolve_shell_path(const char* arg, char* buf, size_t bufsz) {
    if (!arg || *arg == '\0') return cwd;
    if (arg[0] == '/') return arg;           /* absolute – use as-is */

    /* Build cwd + "/" + arg into buf */
    size_t cwdlen = strlen(cwd);
    if (cwdlen + 1 + strlen(arg) + 1 > bufsz) return cwd; /* too long */

    strcpy(buf, cwd);
    /* append '/' separator unless cwd already ends with '/' */
    if (cwdlen > 0 && cwd[cwdlen - 1] != '/') {
        buf[cwdlen] = '/';
        buf[cwdlen + 1] = '\0';
    }
    strncat(buf, arg, bufsz - strlen(buf) - 1);
    buf[bufsz - 1] = '\0';
    return buf;
}

/* Helper: count children of a directory node */
static uint32_t count_children(vfs_node_t* dir) {
    vfs_dirent_t tmp;
    uint32_t n = 0;
    while (vfs_readdir(dir, n, &tmp) == 0) n++;
    return n;
}

/* Helper: print a single file entry with size */
static void ls_print_file_info(vfs_node_t* dir, const char* name) {
    terminal_writestring("[FILE] ");
    terminal_writestring(name);
    vfs_node_t* child = vfs_finddir(dir, name);
    if (child) {
        char buf[16];
        terminal_writestring("  (");
        itoa((int)child->size, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" bytes)");
    }
    terminal_writestring("\n");
}

#define LS_MAX_DEPTH 3

/* Print the tree prefix for a given depth level */
static void ls_print_prefix(int depth, int is_last) {
    for (int i = 0; i < depth; i++) {
        if (i == 0) {
            terminal_writestring("  ");
        }
        else{
            terminal_writestring("     ");
        }
    }
    if (depth > 0) {
        if (is_last) {
            terminal_writestring("|_ ");
        } else {
            terminal_writestring("|  ");
        }
    }
}

/* Recursively list directory contents up to LS_MAX_DEPTH levels */
static void ls_recursive(vfs_node_t* dir, int depth) {
    if (depth > LS_MAX_DEPTH) return;

    uint32_t total = count_children(dir);
    if (total == 0) {
        if (depth == 0) terminal_writestring("  (empty)\n");
        return;
    }

    vfs_dirent_t ent;
    uint32_t idx = 0;
    while (vfs_readdir(dir, idx, &ent) == 0) {
        int is_last = (idx == total - 1);
        int is_dir  = (ent.type & VFS_DIRECTORY);

        ls_print_prefix(depth, is_last);

        if (is_dir) {
            terminal_writestring("[DIR] ");
            terminal_writestring(ent.name);
            terminal_writestring("\n");

            vfs_node_t* sub = vfs_finddir(dir, ent.name);
            if (sub && (sub->type & VFS_DIRECTORY)) {
                ls_recursive(sub, depth + 1);
            }
        } else {
            ls_print_file_info(dir, ent.name);
        }
        idx++;
    }
}

static void cmd_ls(const char* args) {
    char pathbuf[VFS_PATH_MAX];
    const char* path = resolve_shell_path(args, pathbuf, sizeof(pathbuf));

    vfs_node_t* dir = vfs_namei(path);
    if (!dir) {
        terminal_writestring("ls: no such directory: ");
        terminal_writestring(path);
        terminal_writestring("\n");
        return;
    }
    if (!(dir->type & VFS_DIRECTORY)) {
        terminal_writestring("ls: not a directory: ");
        terminal_writestring(path);
        terminal_writestring("\n");
        return;
    }

    ls_recursive(dir, 0);
}

static void cmd_cat(const char* args) {
    if (!args || *args == '\0') {
        terminal_writestring("usage: cat <path>\n");
        return;
    }

    char pathbuf[VFS_PATH_MAX];
    const char* path = resolve_shell_path(args, pathbuf, sizeof(pathbuf));

    vfs_node_t* node = vfs_namei(path);
    if (!node) {
        terminal_writestring("cat: no such file: ");
        terminal_writestring(path);
        terminal_writestring("\n");
        return;
    }
    if (!(node->type & VFS_FILE)) {
        terminal_writestring("cat: not a file: ");
        terminal_writestring(path);
        terminal_writestring("\n");
        return;
    }

    if (node->size == 0) {
        terminal_writestring("(empty file)\n");
        return;
    }

    /* Read in chunks */
    uint8_t buf[257];
    uint32_t offset = 0;
    while (offset < node->size) {
        uint32_t chunk = node->size - offset;
        if (chunk > 256) chunk = 256;
        int32_t n = vfs_read(node, offset, chunk, buf);
        if (n <= 0) break;
        buf[n] = '\0';
        terminal_writestring((const char*)buf);
        offset += (uint32_t)n;
    }
    terminal_writestring("\n");
}

static void cmd_touch(const char* args) {
    if (!args || *args == '\0') {
        terminal_writestring("usage: touch <path>\n");
        return;
    }

    char pathbuf[VFS_PATH_MAX];
    const char* path = resolve_shell_path(args, pathbuf, sizeof(pathbuf));

    /* Check if it already exists */
    if (vfs_namei(path)) {
        terminal_writestring("touch: already exists: ");
        terminal_writestring(path);
        terminal_writestring("\n");
        return;
    }

    /* Split into parent + name */
    /* Find last '/' */
    size_t plen = strlen(path);
    int last_slash = -1;
    for (size_t i = 0; i < plen; i++) {
        if (path[i] == '/') last_slash = (int)i;
    }

    char parent_path[VFS_PATH_MAX];
    char fname[VFS_NAME_MAX];
    if (last_slash <= 0) {
        parent_path[0] = '/'; parent_path[1] = '\0';
        strncpy(fname, path + (last_slash < 0 ? 0 : 1), VFS_NAME_MAX - 1);
    } else {
        memcpy(parent_path, path, (size_t)last_slash);
        parent_path[last_slash] = '\0';
        strncpy(fname, path + last_slash + 1, VFS_NAME_MAX - 1);
    }
    fname[VFS_NAME_MAX - 1] = '\0';

    vfs_node_t* parent = vfs_namei(parent_path);
    if (!parent) {
        terminal_writestring("touch: parent not found: ");
        terminal_writestring(parent_path);
        terminal_writestring("\n");
        return;
    }

    if (vfs_create(parent, fname, VFS_FILE) < 0) {
        terminal_writestring("touch: failed to create file\n");
    }
}

static void cmd_writef(const char* args) {
    if (!args || *args == '\0') {
        terminal_writestring("usage: write <path> <text>\n");
        return;
    }

    /* Split: first token is path, rest is text */
    char path_arg[VFS_PATH_MAX];
    size_t i = 0;
    while (args[i] && args[i] != ' ' && i < VFS_PATH_MAX - 1) {
        path_arg[i] = args[i];
        i++;
    }
    path_arg[i] = '\0';

    const char* text = "";
    if (args[i] == ' ') {
        text = args + i + 1;
        while (*text == ' ') text++;
    }

    if (*text == '\0') {
        terminal_writestring("usage: write <path> <text>\n");
        return;
    }

    char pathbuf[VFS_PATH_MAX];
    const char* path = resolve_shell_path(path_arg, pathbuf, sizeof(pathbuf));

    /* Open with create + truncate */
    int fd = vfs_fd_open(path, VFS_O_RDWR | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) {
        terminal_writestring("write: cannot open: ");
        terminal_writestring(path);
        terminal_writestring("\n");
        return;
    }

    size_t tlen = strlen(text);
    int32_t written = vfs_fd_write(fd, (const uint8_t*)text, (uint32_t)tlen);
    vfs_fd_close(fd);

    char buf[16];
    terminal_writestring("Wrote ");
    itoa((int)written, buf, 10);
    terminal_writestring(buf);
    terminal_writestring(" bytes to ");
    terminal_writestring(path);
    terminal_writestring("\n");
}

static void cmd_mkdir(const char* args) {
    if (!args || *args == '\0') {
        terminal_writestring("usage: mkdir <path>\n");
        return;
    }

    char pathbuf[VFS_PATH_MAX];
    const char* path = resolve_shell_path(args, pathbuf, sizeof(pathbuf));

    if (vfs_namei(path)) {
        terminal_writestring("mkdir: already exists: ");
        terminal_writestring(path);
        terminal_writestring("\n");
        return;
    }

    /* Split parent + dirname */
    size_t plen = strlen(path);
    int last_slash = -1;
    for (size_t i = 0; i < plen; i++) {
        if (path[i] == '/') last_slash = (int)i;
    }

    char parent_path[VFS_PATH_MAX];
    char dname[VFS_NAME_MAX];
    if (last_slash <= 0) {
        parent_path[0] = '/'; parent_path[1] = '\0';
        strncpy(dname, path + (last_slash < 0 ? 0 : 1), VFS_NAME_MAX - 1);
    } else {
        memcpy(parent_path, path, (size_t)last_slash);
        parent_path[last_slash] = '\0';
        strncpy(dname, path + last_slash + 1, VFS_NAME_MAX - 1);
    }
    dname[VFS_NAME_MAX - 1] = '\0';

    vfs_node_t* parent = vfs_namei(parent_path);
    if (!parent) {
        terminal_writestring("mkdir: parent not found: ");
        terminal_writestring(parent_path);
        terminal_writestring("\n");
        return;
    }

    if (vfs_create(parent, dname, VFS_DIRECTORY) < 0) {
        terminal_writestring("mkdir: failed to create directory\n");
    }
}

static void cmd_rm(const char* args) {
    if (!args || *args == '\0') {
        terminal_writestring("usage: rm <path>\n");
        return;
    }

    char pathbuf[VFS_PATH_MAX];
    const char* path = resolve_shell_path(args, pathbuf, sizeof(pathbuf));

    vfs_node_t* node = vfs_namei(path);
    if (!node) {
        terminal_writestring("rm: no such file: ");
        terminal_writestring(path);
        terminal_writestring("\n");
        return;
    }

    /* Resolve parent and basename from path, do not trust node->parent. */
    size_t plen = strlen(path);
    int last_slash = -1;
    for (size_t i = 0; i < plen; ++i) {
        if (path[i] == '/') last_slash = (int)i;
    }

    char parent_path[VFS_PATH_MAX];
    char name[VFS_NAME_MAX];
    if (last_slash <= 0) {
        strcpy(parent_path, "/");
        strncpy(name, path + (last_slash < 0 ? 0 : 1), VFS_NAME_MAX - 1);
    } else {
        memcpy(parent_path, path, (size_t)last_slash);
        parent_path[last_slash] = '\0';
        strncpy(name, path + last_slash + 1, VFS_NAME_MAX - 1);
    }
    name[VFS_NAME_MAX - 1] = '\0';

    if (name[0] == '\0') {
        terminal_writestring("rm: cannot remove root\n");
        return;
    }

    vfs_node_t* parent = vfs_namei(parent_path);
    if (!parent) {
        terminal_writestring("rm: parent not found\n");
        return;
    }

    if (vfs_unlink(parent, name) < 0) {
        terminal_writestring("rm: failed (directory not empty?)\n");
    }
}

static void cmd_stat(const char* args) {
    if (!args || *args == '\0') {
        terminal_writestring("usage: stat <path>\n");
        return;
    }

    char pathbuf[VFS_PATH_MAX];
    const char* path = resolve_shell_path(args, pathbuf, sizeof(pathbuf));

    vfs_node_t* node = vfs_namei(path);
    if (!node) {
        terminal_writestring("stat: not found: ");
        terminal_writestring(path);
        terminal_writestring("\n");
        return;
    }

    vfs_stat_t st;
    vfs_stat(node, &st);

    char buf[16];
    terminal_writestring("  path:  ");
    terminal_writestring(path);
    terminal_writestring("\n  inode: ");
    itoa((int)st.inode, buf, 10);
    terminal_writestring(buf);
    terminal_writestring("\n  type:  ");
    terminal_writestring((st.type & VFS_DIRECTORY) ? "directory" : "file");
    terminal_writestring("\n  size:  ");
    itoa((int)st.size, buf, 10);
    terminal_writestring(buf);
    terminal_writestring(" bytes\n");
}

/*
 * Canonicalize a path in-place: collapse "//", resolve "." and "..".
 * The result always starts with "/" and never has a trailing slash
 * (except for the root directory "/").
 */
static void canonicalize_path(char* path) {
    /* We build the result in a local buffer, then copy back. */
    char tmp[VFS_PATH_MAX];
    size_t tp = 0;  /* write position in tmp */

    const char* p = path;
    while (*p) {
        /* skip duplicate slashes */
        if (*p == '/') {
            p++;
            continue;
        }

        /* find the next component */
        const char* start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);

        if (len == 1 && start[0] == '.') {
            /* "." — skip */
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            /* ".." — go up: remove last component from tmp */
            while (tp > 0 && tmp[tp - 1] != '/') tp--;
            if (tp > 0) tp--;   /* remove the slash itself */
            continue;
        }

        /* normal component: append "/component" */
        if (tp + 1 + len >= VFS_PATH_MAX - 1) break;  /* overflow guard */
        tmp[tp++] = '/';
        memcpy(tmp + tp, start, len);
        tp += len;
    }

    if (tp == 0) {
        path[0] = '/';
        path[1] = '\0';
    } else {
        tmp[tp] = '\0';
        strcpy(path, tmp);
    }
}

static void cmd_cd(const char* args) {
    char pathbuf[VFS_PATH_MAX];

    /* "cd" with no args -> go to root */
    if (!args || *args == '\0') {
        cwd[0] = '/';
        cwd[1] = '\0';
        return;
    }

    const char* path = resolve_shell_path(args, pathbuf, sizeof(pathbuf));

    /* Copy into a mutable buffer so we can canonicalize */
    char resolved[VFS_PATH_MAX];
    strncpy(resolved, path, VFS_PATH_MAX - 1);
    resolved[VFS_PATH_MAX - 1] = '\0';
    canonicalize_path(resolved);

    /* Check the path actually exists and is a directory */
    vfs_node_t* node = vfs_namei(resolved);
    if (!node) {
        terminal_writestring("cd: no such directory: ");
        terminal_writestring(resolved);
        terminal_writestring("\n");
        return;
    }
    if (!(node->type & VFS_DIRECTORY)) {
        terminal_writestring("cd: not a directory: ");
        terminal_writestring(resolved);
        terminal_writestring("\n");
        return;
    }

    strcpy(cwd, resolved);
}

static void cmd_pwd(const char* args) {
    (void)args;
    terminal_writestring(cwd);
    terminal_writestring("\n");
}

static void cmd_exec(const char* args) {
    if (!args || *args == '\0') {
        terminal_writestring("usage: exec <path>\n");
        return;
    }

    char pathbuf[VFS_PATH_MAX];
    const char* path = resolve_shell_path(args, pathbuf, sizeof(pathbuf));

    terminal_writestring("Loading ELF: ");
    terminal_writestring(path);
    terminal_writestring("\n");

    int ret = elf_exec(path, 1);   /* 1 = wait for the task to finish */
    if (ret < 0) {
        terminal_writestring("exec: failed (error ");
        char buf[16];
        itoa(ret, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(")\n");
    }
}

static void cmd_anim(const char* args) {
    (void)args;

    int ret = elf_exec("/bin/anim", 1);
    if (ret < 0) {
        terminal_writestring("anim: failed to launch /bin/anim (error ");
        char buf[16];
        itoa(ret, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(")\n");
    }
}

