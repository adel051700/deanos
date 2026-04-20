#include "include/kernel/shell.h"
#include "include/kernel/tty.h"
#include "include/kernel/keyboard.h"
#include "include/kernel/mouse.h"
#include "include/kernel/rtc.h"
#include "include/kernel/pit.h"
#include "include/kernel/task.h"
#include "include/kernel/usermode.h"
#include "include/kernel/syscall.h"   // SYS_write, SYS_time, SYS_exit
#include "include/kernel/signal.h"
#include "include/kernel/vfs.h"
#include "include/kernel/elf.h"
#include "include/kernel/log.h"
#include "include/kernel/blockdev.h"
#include "include/kernel/mbr.h"
#include "include/kernel/minfs.h"
#include "include/kernel/fat32.h"
#include "include/kernel/paging.h"
#include "include/kernel/net.h"
#include "include/kernel/net_dhcp.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_COMMAND_LENGTH 256
#define SHELL_HISTORY_SIZE 32
#define SHELL_BG_JOBS_MAX 16

static char cwd[VFS_PATH_MAX] = "/";   /* current working directory */

static char command_buffer[MAX_COMMAND_LENGTH];
static size_t command_length = 0;
static size_t cursor_pos = 0;   /* position of terminal cursor within command_buffer */
static int input_line_dirty = 0; /* async output happened since last shell redraw */
static int shell_jobctl_ready = 0;
static int shell_sid = 0;
static int shell_pgid = 0;

typedef struct {
    int in_use;
    int pid;
    int pgid;
    char cmd[VFS_PATH_MAX];
} shell_bg_job_t;

static shell_bg_job_t shell_bg_jobs[SHELL_BG_JOBS_MAX];

static char history[SHELL_HISTORY_SIZE][MAX_COMMAND_LENGTH];
static size_t history_len = 0;     // number of stored entries
static size_t history_pos = 0;     // browsing index: 0..history_len (history_len = current line)
static char edit_backup[MAX_COMMAND_LENGTH]; // current line backup when entering history

typedef enum { ESC_IDLE=0, ESC_GOT_ESC, ESC_GOT_BRACKET, ESC_GOT_BRACKET_3 } esc_state_t;
static esc_state_t esc_state = ESC_IDLE;

// Forward decls for history helpers
static void history_add(const char* cmd);
static void shell_set_line(const char* s);
static void shell_execute_command(const char* command);
static void shell_execute_pipeline(const char* command);
static const char* resolve_shell_path(const char* arg, char* buf, size_t bufsz);
static void shell_autocomplete(void);
static void shell_insert_char_at_cursor(char c);
static size_t copy_token(const char* src, char* dst, size_t dstsz);

static void shell_jobctl_ensure(void);

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
static void cmd_kill(const char* args);
static void cmd_wait(const char* args);
static void cmd_libctest(const char* args);
static void cmd_mouse(const char* args);
static void cmd_dmesg(const char* args);
static void cmd_blk(const char* args);
static void cmd_disk(const char* args);
static void cmd_fsfill(const char* args);
static void cmd_fsverify(const char* args);
static void cmd_vm(const char* args);
static void cmd_net(const char* args);

// Filesystem commands
static void cmd_ls(const char* args);
static void cmd_cat(const char* args);
static void cmd_touch(const char* args);
static void cmd_writef(const char* args);
static void cmd_mkdir(const char* args);
static void cmd_rm(const char* args);
static void cmd_stat(const char* args);
static void cmd_id(const char* args);
static void cmd_chmod(const char* args);
static void cmd_chown(const char* args);
static void cmd_cd(const char* args);
static void cmd_pwd(const char* args);
static void cmd_exec(const char* args);
static void cmd_anim(const char* args);
static void cmd_jobs(const char* args);
static void cmd_fg(const char* args);
static void cmd_bg(const char* args);
static void cmd_vfstest(const char* args);
static void cmd_fat32test(const char* args);

static void shell_jobs_reap(void);
static void shell_jobs_add(int pid, int pgid, const char* cmd);
static int shell_jobs_find_by_pid(int pid);
static void shell_jobs_remove_index(int idx);

/* Print the shell prompt: "DeanOS /path $ " */
static void shell_print_prompt(void);
static void shell_clear_screen(int redraw_input);
static void shell_redraw_current_input(void);

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
    {"color",  cmd_color,  "Set colors: color <text> | color <background> <text>"},
    {"cls",    cmd_cls,    "Clear screen"},
    {"about",  cmd_about,  "About DeanOS"},
    {"dean",   cmd_dean,   "Show DeanOS banner"},
    {"time",   cmd_time,   "Show current time/date"},
    {"uptime", cmd_uptime, "Show system uptime"},
    {"ticks",  cmd_ticks,  "Show PIT tick count"},
    {"tasks",  cmd_tasks,  "List all tasks and their state (with PPID)"},
    {"kill",   cmd_kill,   "Send signal by parent id: kill [-INT|-TERM|-KILL|-<num>] <ppid>"},
    {"wait",   cmd_wait,   "Wait for child exit: wait [pid|any]"},
    {"mouse",  cmd_mouse,  "Show PS/2 mouse state (mouse clear resets totals)"},
    {"blk",    cmd_blk,    "Block devices: blk list/read/write/cache/flush/async"},
    {"disk",   cmd_disk,   "Disk tools: disk parts | init | mkfs | mount | setup"},
    {"fsfill", cmd_fsfill, "Write a large patterned file: fsfill <path> <bytes> <seed>"},
    {"fsverify", cmd_fsverify, "Verify a patterned file: fsverify <path> <bytes> <seed>"},
    {"vm",     cmd_vm,     "VM hooks: vm stats | vm demand [addr pages] | vm cow | vm refs"},
    {"net",    cmd_net,    "Network: net | net netstat | net rxdefer | net timers | net p2 | net dns cache | net test regress|fuzz|stress [count] [seed] | net dhcp [timeout_ms] | net tx | net regs | net ip | net arp | net arping <ip> | net ping <ip> [-c count] [-W timeout_ms] | net tcp http <host> <port> <path>"},
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
    {"id",      cmd_id,      "Show current uid/gid"},
    {"chmod",   cmd_chmod,   "Change mode bits: chmod <octal-mode> <path>"},
    {"chown",   cmd_chown,   "Change owner/group: chown <uid> <gid> <path>"},
    {"cd",      cmd_cd,      "Change directory: cd <path>"},
    {"pwd",     cmd_pwd,     "Print working directory"},
    {"exec",    cmd_exec,    "Run an ELF program: exec <path> [&]"},
    {"jobs",    cmd_jobs,    "List background jobs"},
    {"fg",      cmd_fg,      "Bring job to foreground: fg <pid>"},
    {"bg",      cmd_bg,      "Keep job in background: bg <pid>"},
    {"anim",    cmd_anim,    "Run large animated demo"},
    {"vfstest", cmd_vfstest, "VFS tests: vfstest norm | vfstest mount | vfstest perm"},
    {"fat32test", cmd_fat32test, "FAT32 write regression test: fat32test <existing-file-path>"},

    // New syscall test commands
    {"sys.write",   cmd_sys_write,   "Syscall write via int 0x80: sys.write <text>"},
    {"sys.time",    cmd_sys_time,    "Syscall time via int 0x80"},
    {"sys81.time",  cmd_sys81_time,  "Syscall time via int 0x81"},
    {"sys.exit",    cmd_sys_exit,    "Syscall exit (halts) via int 0x80: sys.exit <code>"},

    {NULL, NULL, NULL}
};

static int shell_is_hidden_help_command(const char* name) {
    if (!name) return 0;

    if (strcmp(name, "fsfill") == 0) return 1;
    if (strcmp(name, "fsverify") == 0) return 1;
    if (strcmp(name, "vm") == 0) return 1;
    if (strcmp(name, "libctest") == 0) return 1;
    if (strcmp(name, "fat32test") == 0) return 1;
    if (strcmp(name, "sys.write") == 0) return 1;
    if (strcmp(name, "sys.time") == 0) return 1;
    if (strcmp(name, "sys81.time") == 0) return 1;
    if (strcmp(name, "sys.exit") == 0) return 1;
    return 0;
}

static void shell_print_prompt(void) {
    terminal_writestring("DeanOS ");
    terminal_writestring(cwd);
    terminal_writestring(" $ ");
    input_line_dirty = 0;
}

static void shell_clear_screen(int redraw_input) {
    uint32_t cursor_color = terminal_get_color();
    uint32_t bg_color = terminal_get_background();
    int ctl_sid = terminal_get_controlling_sid();
    int fg_pgid = terminal_get_foreground_pgid();

    terminal_initialize();
    terminal_enable_cursor();
    terminal_setbackground(bg_color);
    terminal_setcolor(cursor_color);

    if (ctl_sid > 0) (void)terminal_set_controlling_sid(ctl_sid);
    if (fg_pgid >= 0) (void)terminal_set_foreground_pgid(fg_pgid);

    if (redraw_input) {
        shell_print_prompt();
        shell_redraw_current_input();
    }
}

static void shell_redraw_current_input(void) {
    for (size_t i = 0; i < command_length; ++i) {
        terminal_putchar(command_buffer[i]);
    }
    size_t tail = command_length - cursor_pos;
    for (size_t i = 0; i < tail; ++i) {
        terminal_move_cursor_left();
    }
}

static void shell_jobctl_ensure(void) {
    if (shell_jobctl_ready) return;

    int self = task_current_id();
    if (self <= 0) return;

    int sid = task_setsid();
    if (sid < 0) sid = task_current_sid();
    int pgid = task_current_pgid();
    if (sid <= 0 || pgid <= 0) return;

    shell_sid = sid;
    shell_pgid = pgid;

    if (terminal_set_controlling_sid(shell_sid) == 0) {
        (void)terminal_set_foreground_pgid(shell_pgid);
        (void)task_set_signal_ignored(KSIGINT, 1);
        shell_jobctl_ready = 1;
    }
}

static void shell_jobs_remove_index(int idx) {
    if (idx < 0 || idx >= SHELL_BG_JOBS_MAX) return;
    shell_bg_jobs[idx].in_use = 0;
    shell_bg_jobs[idx].pid = 0;
    shell_bg_jobs[idx].pgid = 0;
    shell_bg_jobs[idx].cmd[0] = '\0';
}

static int shell_jobs_find_by_pid(int pid) {
    if (pid <= 0) return -1;
    for (int i = 0; i < SHELL_BG_JOBS_MAX; ++i) {
        if (!shell_bg_jobs[i].in_use) continue;
        if (shell_bg_jobs[i].pid == pid) return i;
    }
    return -1;
}

static void shell_jobs_add(int pid, int pgid, const char* cmd) {
    if (pid <= 0 || pgid <= 0) return;
    int slot = -1;
    for (int i = 0; i < SHELL_BG_JOBS_MAX; ++i) {
        if (!shell_bg_jobs[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) return;

    shell_bg_jobs[slot].in_use = 1;
    shell_bg_jobs[slot].pid = pid;
    shell_bg_jobs[slot].pgid = pgid;
    if (cmd) {
        strncpy(shell_bg_jobs[slot].cmd, cmd, sizeof(shell_bg_jobs[slot].cmd) - 1);
        shell_bg_jobs[slot].cmd[sizeof(shell_bg_jobs[slot].cmd) - 1] = '\0';
    } else {
        shell_bg_jobs[slot].cmd[0] = '\0';
    }
}

static void shell_jobs_reap(void) {
    for (int i = 0; i < SHELL_BG_JOBS_MAX; ++i) {
        if (!shell_bg_jobs[i].in_use) continue;

        int status = 0;
        int rc = task_waitpid(shell_bg_jobs[i].pid, &status, TASK_WAIT_NOHANG);
        if (rc == shell_bg_jobs[i].pid || rc == -3) {
            shell_jobs_remove_index(i);
        }
    }
}

void shell_mark_tty_async_output(void) {
    input_line_dirty = 1;
}

void shell_write_async_output(const char* data, size_t len) {
    if (!data || len == 0) return;

    /* Start async output on a fresh line once, then keep streaming there. */
    if (!input_line_dirty) {
        terminal_putchar('\n');
    }

    terminal_write(data, len);
    input_line_dirty = 1;
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
    input_line_dirty = 0;
    cwd[0] = '/';
    cwd[1] = '\0';
    for (int i = 0; i < SHELL_BG_JOBS_MAX; ++i) {
        shell_jobs_remove_index(i);
    }
    shell_print_prompt();
    terminal_enable_cursor();  // Enable cursor now that we're ready for input
}

/**
 * Process a character input to the shell
 */
void shell_process_char(char c) {
    shell_jobctl_ensure();
    shell_jobs_reap();

    if ((unsigned char)c == 12u) {
        shell_clear_screen(1);
        return;
    }

    if (input_line_dirty) {
        terminal_putchar('\n');
        shell_print_prompt();
        shell_redraw_current_input();
        input_line_dirty = 0;
    }

    if ((unsigned char)c == 3u) {
        terminal_writestring("^C\n");
        command_length = 0;
        cursor_pos = 0;
        command_buffer[0] = '\0';
        history_pos = history_len;
        esc_state = ESC_IDLE;
        if (shell_jobctl_ready && shell_pgid > 0) {
            (void)terminal_set_foreground_pgid(shell_pgid);
        }
        shell_print_prompt();
        return;
    }

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
        } else if (c == '3') {
            // Begin Delete sequence: ESC [ 3 ~
            esc_state = ESC_GOT_BRACKET_3;
            return;
        }
        esc_state = ESC_IDLE;
        return;
    } else if (esc_state == ESC_GOT_BRACKET_3) {
        if (c == '~' && cursor_pos < command_length) {
            size_t chars_after = command_length - cursor_pos - 1;
            memmove(&command_buffer[cursor_pos], &command_buffer[cursor_pos + 1], chars_after);
            command_length--;
            command_buffer[command_length] = '\0';

            /* Redraw tail and erase stale last character cell. */
            for (size_t i = cursor_pos; i < command_length; i++) {
                terminal_putchar(command_buffer[i]);
            }
            terminal_putchar(' ');

            for (size_t i = 0; i <= chars_after; i++) {
                terminal_move_cursor_left();
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
    } else {
        unsigned char uc = (unsigned char)c;
        if (uc >= ' ' && uc != 0x7F && command_length < MAX_COMMAND_LENGTH - 1) {
            shell_insert_char_at_cursor(c);
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

    if (strchr(command, '|') != NULL || strchr(command, '<') != NULL || strchr(command, '>') != NULL) {
        shell_execute_pipeline(command);
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

#define SHELL_PIPE_MAX_STAGES 8

typedef struct {
    char path[VFS_PATH_MAX];
    char in_path[VFS_PATH_MAX];
    char out_path[VFS_PATH_MAX];
    int  has_in;
    int  has_out;
    int  append_out;
} shell_stage_spec_t;

static size_t shell_stage_next_token(const char** p_in, char* out, size_t out_sz) {
    if (!p_in || !*p_in || !out || out_sz == 0) return 0;
    const char* p = *p_in;

    while (*p == ' ') p++;
    if (*p == '\0') {
        *p_in = p;
        out[0] = '\0';
        return 0;
    }

    if (*p == '<') {
        if (out_sz < 2) return 0;
        out[0] = '<';
        out[1] = '\0';
        p++;
        *p_in = p;
        return 1;
    }

    if (*p == '>') {
        if (*(p + 1) == '>') {
            if (out_sz < 3) return 0;
            out[0] = '>';
            out[1] = '>';
            out[2] = '\0';
            p += 2;
            *p_in = p;
            return 2;
        }
        if (out_sz < 2) return 0;
        out[0] = '>';
        out[1] = '\0';
        p++;
        *p_in = p;
        return 1;
    }

    size_t i = 0;
    while (p[i] && p[i] != ' ' && p[i] != '<' && p[i] != '>' && i < out_sz - 1) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    p += i;
    *p_in = p;
    return i;
}

static char* shell_trim_spaces(char* s) {
    while (*s == ' ') s++;

    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') {
        s[n - 1] = '\0';
        n--;
    }
    return s;
}

static int shell_stage_parse(char* segment, shell_stage_spec_t* spec) {
    if (!segment || !spec) return -1;

    memset(spec, 0, sizeof(*spec));
    const char* p = shell_trim_spaces(segment);
    if (*p == '\0') return -1;

    char token[VFS_PATH_MAX];
    size_t n = shell_stage_next_token(&p, token, sizeof(token));
    if (n == 0) return -1;

    if (strcmp(token, "exec") == 0) {
        while (*p == ' ') p++;
    }

    while (*p) {
        n = shell_stage_next_token(&p, token, sizeof(token));
        if (n == 0) break;

        if (strcmp(token, "<") == 0 || strcmp(token, ">") == 0 || strcmp(token, ">>") == 0) {
            int is_in = (token[0] == '<');
            int is_append = (token[0] == '>' && token[1] == '>');

            char file_token[VFS_PATH_MAX];
            size_t fn = shell_stage_next_token(&p, file_token, sizeof(file_token));
            if (fn == 0) return -1;
            if (strcmp(file_token, "<") == 0 || strcmp(file_token, ">") == 0 || strcmp(file_token, ">>") == 0) {
                return -1;
            }

            if (is_in) {
                if (spec->has_in) return -1;
                strncpy(spec->in_path, file_token, sizeof(spec->in_path) - 1);
                spec->in_path[sizeof(spec->in_path) - 1] = '\0';
                spec->has_in = 1;
            } else {
                if (spec->has_out) return -1;
                strncpy(spec->out_path, file_token, sizeof(spec->out_path) - 1);
                spec->out_path[sizeof(spec->out_path) - 1] = '\0';
                spec->has_out = 1;
                spec->append_out = is_append;
            }
            continue;
        }

        if (spec->path[0] != '\0') {
            return -1;
        }

        strncpy(spec->path, token, sizeof(spec->path) - 1);
        spec->path[sizeof(spec->path) - 1] = '\0';
    }

    return (spec->path[0] != '\0') ? 0 : -1;
}

static void shell_execute_pipeline(const char* command) {
    if (!command || *command == '\0') return;
    shell_jobctl_ensure();

    char line[MAX_COMMAND_LENGTH];
    strncpy(line, command, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    char* stages[SHELL_PIPE_MAX_STAGES];
    shell_stage_spec_t specs[SHELL_PIPE_MAX_STAGES];
    int stage_count = 0;
    char* cur = line;

    while (stage_count < SHELL_PIPE_MAX_STAGES) {
        char* bar = strchr(cur, '|');
        if (bar) *bar = '\0';

        char* seg = shell_trim_spaces(cur);
        if (*seg == '\0') {
            terminal_writestring("pipe: empty stage\n");
            return;
        }
        stages[stage_count++] = seg;

        if (!bar) break;
        cur = bar + 1;
    }

    if (strchr(cur, '|') != NULL) {
        terminal_writestring("pipe: too many stages\n");
        return;
    }

    for (int i = 0; i < stage_count; ++i) {
        if (shell_stage_parse(stages[i], &specs[i]) < 0) {
            terminal_writestring("pipe: invalid stage syntax\n");
            return;
        }
        if (i > 0 && specs[i].has_in) {
            terminal_writestring("pipe: '<' only supported on the first stage\n");
            return;
        }
        if (i < stage_count - 1 && specs[i].has_out) {
            terminal_writestring("pipe: '>'/'>>' only supported on the last stage\n");
            return;
        }
    }

    int pipes[SHELL_PIPE_MAX_STAGES - 1][2];
    for (int i = 0; i < SHELL_PIPE_MAX_STAGES - 1; ++i) {
        pipes[i][0] = -1;
        pipes[i][1] = -1;
    }

    for (int i = 0; i < stage_count - 1; ++i) {
        if (vfs_fd_pipe(pipes[i]) < 0) {
            terminal_writestring("pipe: failed to allocate pipe\n");
            for (int j = 0; j < i; ++j) {
                if (pipes[j][0] >= 0) vfs_fd_close(pipes[j][0]);
                if (pipes[j][1] >= 0) vfs_fd_close(pipes[j][1]);
            }
            return;
        }
    }

    int pids[SHELL_PIPE_MAX_STAGES];
    int started = 0;
    int launch_failed = 0;
    int pipeline_pgid = 0;
    for (int i = 0; i < stage_count; ++i) {
        char resolved[VFS_PATH_MAX];
        const char* path = resolve_shell_path(specs[i].path, resolved, sizeof(resolved));
        int in_fd = (i == 0) ? -1 : pipes[i - 1][0];
        int out_fd = (i == stage_count - 1) ? -1 : pipes[i][1];

        int opened_in_fd = -1;
        int opened_out_fd = -1;

        if (specs[i].has_in) {
            char in_resolved[VFS_PATH_MAX];
            const char* in_path = resolve_shell_path(specs[i].in_path, in_resolved, sizeof(in_resolved));
            opened_in_fd = vfs_fd_open(in_path, VFS_O_RDONLY);
            if (opened_in_fd < 0) {
                terminal_writestring("pipe: failed to open input: ");
                terminal_writestring(in_path);
                terminal_writestring("\n");
                launch_failed = 1;
                break;
            }
            in_fd = opened_in_fd;
        }

        if (specs[i].has_out) {
            char out_resolved[VFS_PATH_MAX];
            const char* out_path = resolve_shell_path(specs[i].out_path, out_resolved, sizeof(out_resolved));
            uint32_t oflags = VFS_O_WRONLY | VFS_O_CREATE;
            oflags |= specs[i].append_out ? VFS_O_APPEND : VFS_O_TRUNC;
            opened_out_fd = vfs_fd_open(out_path, oflags);
            if (opened_out_fd < 0) {
                if (opened_in_fd >= 0) vfs_fd_close(opened_in_fd);
                terminal_writestring("pipe: failed to open output: ");
                terminal_writestring(out_path);
                terminal_writestring("\n");
                launch_failed = 1;
                break;
            }
            out_fd = opened_out_fd;
        }

        int pid = elf_exec_with_stdio(path, 0, in_fd, out_fd);
        if (opened_in_fd >= 0) vfs_fd_close(opened_in_fd);
        if (opened_out_fd >= 0) vfs_fd_close(opened_out_fd);
        if (pid < 0) {
            terminal_writestring("pipe: launch failed for ");
            terminal_writestring(path);
            terminal_writestring("\n");
            launch_failed = 1;
            break;
        }

        pids[started++] = pid;

        if (pipeline_pgid == 0) {
            pipeline_pgid = pid;
            (void)task_setpgid(pid, pid);
        } else {
            (void)task_setpgid(pid, pipeline_pgid);
        }
    }

    for (int i = 0; i < stage_count - 1; ++i) {
        if (pipes[i][0] >= 0) vfs_fd_close(pipes[i][0]);
        if (pipes[i][1] >= 0) vfs_fd_close(pipes[i][1]);
    }

    if (launch_failed || started < stage_count) {
        for (int i = 0; i < started; ++i) {
            (void)task_kill(pids[i]);
        }
    }

    if (!launch_failed && started > 0 && pipeline_pgid > 0) {
        (void)terminal_set_foreground_pgid(pipeline_pgid);
    }

    for (int i = 0; i < started; ++i) {
        int status = 0;
        (void)task_waitpid(pids[i], &status, 0);
    }

    if (shell_jobctl_ready && shell_pgid > 0) {
        (void)terminal_set_foreground_pgid(shell_pgid);
    }
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
        if (shell_is_hidden_help_command(commands[i].name)) continue;
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

static int shell_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return 10 + (int)(c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (int)(c - 'A');
    return -1;
}

static int shell_parse_color_token(const char* token, uint32_t* out_color) {
    if (!token || !out_color || *token == '\0') return 0;

    if (strcmp(token, "black") == 0) { *out_color = 0x000000; return 1; }
    if (strcmp(token, "white") == 0) { *out_color = 0xFFFFFF; return 1; }
    if (strcmp(token, "red") == 0) { *out_color = 0xFF0000; return 1; }
    if (strcmp(token, "green") == 0) { *out_color = 0x00FF00; return 1; }
    if (strcmp(token, "blue") == 0) { *out_color = 0x0000FF; return 1; }
    if (strcmp(token, "yellow") == 0) { *out_color = 0xFFFF00; return 1; }
    if (strcmp(token, "cyan") == 0) { *out_color = 0x00FFFF; return 1; }
    if (strcmp(token, "magenta") == 0) { *out_color = 0xFF00FF; return 1; }
    if (strcmp(token, "gray") == 0 || strcmp(token, "grey") == 0) { *out_color = 0x808080; return 1; }

    const char* p = token;
    if (*p == '#') {
        p++;
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    uint32_t value = 0;
    for (int i = 0; i < 6; ++i) {
        int nib = shell_hex_nibble(p[i]);
        if (nib < 0) return 0;
        value = (value << 4) | (uint32_t)nib;
    }
    if (p[6] != '\0') return 0;

    *out_color = value;
    return 1;
}

/**
 * Color command - change background/text colors
 */
static void cmd_color(const char* args) {
    const char* p = args;
    char tok1[24];
    char tok2[24];
    uint32_t bg = terminal_get_background();
    uint32_t fg = 0;
    int has_bg_arg = 0;

    while (p && *p == ' ') p++;
    size_t n1 = copy_token(p, tok1, sizeof(tok1));
    if (n1 == 0) {
        terminal_writestring("Usage: color <text> | color <background> <text>\n");
        terminal_writestring("Colors: black white red green blue yellow cyan magenta gray\n");
        terminal_writestring("Hex: #RRGGBB or 0xRRGGBB\n");
        return;
    }

    p += n1;
    while (*p == ' ') p++;

    size_t n2 = copy_token(p, tok2, sizeof(tok2));
    if (n2 == 0) {
        if (!shell_parse_color_token(tok1, &fg)) {
            terminal_writestring("color: invalid text color\n");
            return;
        }
    } else {
        has_bg_arg = 1;
        p += n2;
        while (*p == ' ') p++;
        if (*p != '\0') {
            terminal_writestring("Usage: color <text> | color <background> <text>\n");
            return;
        }
        if (!shell_parse_color_token(tok1, &bg)) {
            terminal_writestring("color: invalid background color\n");
            return;
        }
        if (!shell_parse_color_token(tok2, &fg)) {
            terminal_writestring("color: invalid text color\n");
            return;
        }
    }

    if (has_bg_arg) {
        terminal_setbackground(bg);
    }
    terminal_setcolor(fg);

    if (has_bg_arg) {
        terminal_writestring("Background and text colors updated\n");
    } else {
        terminal_writestring("Text color updated\n");
    }
}

/**
 * Clear screen command
 */
static void cmd_cls(const char* args) {
    (void)args;
    shell_clear_screen(0);
}

/**
 * About command - show information about the OS
 */
static void cmd_about(const char* args) {
    (void)args; // Unused
    
    terminal_writestring("DeanOS - A minimal operating system\n");
    terminal_writestring("Created as a learning project\n");
    terminal_writestring("Version 0.7\n");
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

static void shell_insert_char_at_cursor(char c) {
    if (command_length >= MAX_COMMAND_LENGTH - 1) return;

    /* Insert and redraw tail so mid-line edits remain visually consistent. */
    memmove(&command_buffer[cursor_pos + 1], &command_buffer[cursor_pos],
            command_length - cursor_pos);
    command_buffer[cursor_pos] = c;
    command_length++;
    command_buffer[command_length] = '\0';

    for (size_t i = cursor_pos; i < command_length; i++) {
        terminal_putchar(command_buffer[i]);
    }

    size_t move_back = command_length - cursor_pos - 1;
    for (size_t i = 0; i < move_back; i++) {
        terminal_move_cursor_left();
    }
    cursor_pos++;
}

/**
 * Tab-autocomplete: complete the current token against command names (first
 * word) or VFS paths (subsequent words).  Completes to the first match found.
 */
static void shell_autocomplete(void) {
    command_buffer[command_length] = '\0';

    /* Find the start of the word at cursor position. */
    const char* word_start = command_buffer;
    for (size_t i = 0; i < cursor_pos; i++) {
        if (command_buffer[i] == ' ') {
            word_start = &command_buffer[i + 1];
        }
    }
    size_t word_len = (size_t)(command_buffer + cursor_pos - word_start);

    if (word_len == 0) return;

    if (word_start == command_buffer) {
        /* ---- Command name completion ---- */
        for (size_t i = 0; commands[i].name != NULL; i++) {
            if (strncmp(commands[i].name, word_start, word_len) == 0) {
                const char* rest = commands[i].name + word_len;

                /* Avoid duplicating already-present suffix under cursor. */
                size_t skip = 0;
                while (rest[skip] && (cursor_pos + skip) < command_length &&
                       command_buffer[cursor_pos + skip] == rest[skip]) {
                    skip++;
                }

                rest += skip;
                while (*rest && command_length < MAX_COMMAND_LENGTH - 1) {
                    shell_insert_char_at_cursor(*rest++);
                }
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

                /* Avoid duplicating already-present suffix under cursor. */
                size_t skip = 0;
                while (rest[skip] && (cursor_pos + skip) < command_length &&
                       command_buffer[cursor_pos + skip] == rest[skip]) {
                    skip++;
                }

                rest += skip;
                while (*rest && command_length < MAX_COMMAND_LENGTH - 1) {
                    shell_insert_char_at_cursor(*rest++);
                }

                /* Append a trailing '/' for directories */
                if ((entry.type & VFS_DIRECTORY) && command_length < MAX_COMMAND_LENGTH - 1) {
                    shell_insert_char_at_cursor('/');
                }
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

static uint32_t parse_u32_auto(const char* s) {
    while (*s == ' ') s++;
    uint32_t base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    uint32_t v = 0;
    for (;;) {
        char c = *s;
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') d = 10u + (uint32_t)(c - 'a');
        else if (base == 16 && c >= 'A' && c <= 'F') d = 10u + (uint32_t)(c - 'A');
        else break;
        v = v * base + d;
        s++;
    }
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

static int is_octal_token(const char* s) {
    if (!s || *s == '\0') return 0;
    while (*s && *s != ' ') {
        if (*s < '0' || *s > '7') return 0;
        s++;
    }
    return 1;
}

static int parse_mode_octal(const char* token, uint16_t* out_mode) {
    if (!token || !out_mode) return -1;
    if (!is_octal_token(token)) return -1;

    uint32_t mode = 0;
    const char* p = token;
    while (*p && *p != ' ') {
        mode = (mode << 3u) + (uint32_t)(*p - '0');
        p++;
    }
    if (mode > 0777u) return -1;
    *out_mode = (uint16_t)mode;
    return 0;
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

static void term_write_ipv4(const uint8_t ip[4]) {
    term_write_u32(ip[0]); terminal_writestring(".");
    term_write_u32(ip[1]); terminal_writestring(".");
    term_write_u32(ip[2]); terminal_writestring(".");
    term_write_u32(ip[3]);
}

static int parse_ipv4_token(const char* token, uint8_t out_ip[4]) {
    uint8_t parsed[4] = {0, 0, 0, 0};
    uint32_t part = 0;
    int octet = 0;
    const char* s = token;

    if (!token || !out_ip) return -1;

    while (*s && *s != ' ') {
        if (*s >= '0' && *s <= '9') {
            part = part * 10u + (uint32_t)(*s - '0');
            if (part > 255u) return -1;
            s++;
            continue;
        }
        if (*s == '.') {
            if (octet >= 3) return -1;
            parsed[octet++] = (uint8_t)part;
            part = 0;
            s++;
            continue;
        }
        return -1;
    }

    if (octet != 3) return -1;
    parsed[3] = (uint8_t)part;

    out_ip[0] = parsed[0];
    out_ip[1] = parsed[1];
    out_ip[2] = parsed[2];
    out_ip[3] = parsed[3];
    return 0;
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
    terminal_writestring("calling exit(");
    char buf[16];
    itoa((int)code, buf, 10);
    terminal_writestring(buf);
    terminal_writestring(")...\n");
    (void)ksyscall3_vec(0x80, SYS_exit, code, 0, 0);
}

static void cmd_tasks(const char* args) {
    (void)args;
    static const char* state_names[] = {"READY", "RUN  ", "BLOCK", "DEAD "};
    char buf[16];

    terminal_writestring("ID  PPID  SID  PGID  STATE  QUANTUM  NAME\n");
    terminal_writestring("--  ----  ---  ----  -----  -------  ----\n");

    uint32_t count = task_count();
    int cur = task_current_id();

    for (uint32_t i = 0; i < count; ++i) {
        const task_t* t = task_get(i);
        if (!t) continue;

        itoa(t->id, buf, 10);
        if ((int)t->id == cur) terminal_writestring("*");
        else terminal_writestring(" ");
        terminal_writestring(buf);
        terminal_writestring("  ");

        itoa((int)t->parent_id, buf, 10);
        terminal_writestring(buf);
        terminal_writestring("    ");

        itoa((int)t->sid, buf, 10);
        terminal_writestring(buf);
        terminal_writestring("   ");

        itoa((int)t->pgid, buf, 10);
        terminal_writestring(buf);
        terminal_writestring("    ");

        if (t->state <= TASK_DEAD) terminal_writestring(state_names[t->state]);
        else terminal_writestring("???? ");
        terminal_writestring("  ");

        itoa(t->quantum, buf, 10);
        terminal_writestring(buf);
        terminal_writestring("        ");

        terminal_writestring(t->name);
        terminal_writestring("\n");
    }
}

static int shell_parse_signal_token(const char* token) {
    if (!token || *token == '\0') return -1;
    if (*token == '-') token++;
    if (*token == '\0') return -1;

    if (*token >= '0' && *token <= '9') {
        int sig = (int)parse_uint(token);
        return signal_is_supported(sig) ? sig : -1;
    }

    if ((token[0] == 'S' || token[0] == 's') &&
        (token[1] == 'I' || token[1] == 'i') &&
        (token[2] == 'G' || token[2] == 'g')) {
        token += 3;
    }

    if (strcmp(token, "INT") == 0 || strcmp(token, "int") == 0) return KSIGINT;
    if (strcmp(token, "TERM") == 0 || strcmp(token, "term") == 0) return KSIGTERM;
    if (strcmp(token, "KILL") == 0 || strcmp(token, "kill") == 0) return KSIGKILL;
    if (strcmp(token, "CHLD") == 0 || strcmp(token, "chld") == 0) return KSIGCHLD;
    return -1;
}

static void cmd_kill(const char* args) {
    if (!args || *args == '\0') {
        terminal_writestring("usage: kill [-INT|-TERM|-KILL|-<num>] <ppid>\n");
        return;
    }

    const char* p = args;
    while (*p == ' ') p++;

    char tok1[24];
    char tok2[24];
    size_t n1 = copy_token(p, tok1, sizeof(tok1));
    if (n1 == 0) {
        terminal_writestring("usage: kill [-INT|-TERM|-KILL|-<num>] <ppid>\n");
        return;
    }
    p += n1;
    while (*p == ' ') p++;

    int sig = KSIGKILL;
    const char* ppid_tok = tok1;
    if (tok1[0] == '-') {
        sig = shell_parse_signal_token(tok1);
        if (sig <= 0) {
            terminal_writestring("kill: invalid signal (use INT, TERM, KILL, CHLD, or number)\n");
            return;
        }

        size_t n2 = copy_token(p, tok2, sizeof(tok2));
        if (n2 == 0) {
            terminal_writestring("usage: kill [-INT|-TERM|-KILL|-<num>] <ppid>\n");
            return;
        }
        ppid_tok = tok2;
    }

    if (!is_decimal_token(ppid_tok)) {
        terminal_writestring("kill: invalid ppid\n");
        return;
    }

    int ppid = (int)parse_uint(ppid_tok);
    if (ppid <= 0) {
        terminal_writestring("kill: invalid ppid\n");
        return;
    }

    uint32_t count = task_count();
    int killed = 0;
    int refused_idle = 0;

    for (uint32_t i = 0; i < count; ++i) {
        const task_t* t = task_get(i);
        if (!t) continue;
        if ((int)t->parent_id != ppid) continue;
        if (t->state == TASK_DEAD) continue;

        int rc = signal_send_task((int)t->id, sig);
        if (rc == 0) {
            killed++;
        } else if (rc == -2) {
            refused_idle = 1;
        }
    }

    if (killed > 0) {
        terminal_writestring("kill: signaled ");
        char buf[16];
        itoa(killed, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" task(s) with ppid ");
        itoa(ppid, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" using sig ");
        itoa(sig, buf, 10);
        terminal_writestring(buf);
        terminal_writestring("\n");
    } else if (refused_idle) {
        terminal_writestring("kill: refusing to kill idle task\n");
    } else {
        terminal_writestring("kill: no tasks with that ppid\n");
    }
}

static void cmd_wait(const char* args) {
    int pid = -1;
    if (args && *args) {
        if (strcmp(args, "any") == 0) {
            pid = -1;
        } else {
            pid = (int)parse_uint(args);
            if (pid <= 0) {
                terminal_writestring("wait: invalid pid (use positive pid or 'any')\n");
                return;
            }
        }
    }

    int status = 0;
    int ret = task_waitpid(pid, &status, 0);
    if (ret > 0) {
        char buf[16];
        terminal_writestring("wait: child ");
        itoa(ret, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" exited with status ");
        itoa(status, buf, 10);
        terminal_writestring(buf);
        terminal_writestring("\n");
        return;
    }

    if (ret == -3) {
        terminal_writestring("wait: no matching child\n");
    } else {
        terminal_writestring("wait: failed\n");
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

    if (strcmp(args, "cache") == 0 || strcmp(args, "async") == 0) {
        blockdev_cache_stats_t st = {0};
        blockdev_cache_stats(&st);
        char buf[24];

        terminal_writestring("blk cache: entries=");
        itoa((int)st.entries, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" hits=");
        itoa((int)st.hits, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" misses=");
        itoa((int)st.misses, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" evictions=");
        itoa((int)st.evictions, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" writebacks=");
        itoa((int)st.writebacks, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" async_submitted=");
        itoa((int)st.async_submitted, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" async_completed=");
        itoa((int)st.async_completed, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" async_failed=");
        itoa((int)st.async_failed, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" async_pending=");
        itoa((int)st.async_pending, buf, 10);
        terminal_writestring(buf);
        terminal_writestring("\n");
        return;
    }

    if (strcmp(args, "flush") == 0) {
        if (blockdev_flush_all() < 0) {
            terminal_writestring("blk: flush failed\n");
            return;
        }
        terminal_writestring("blk: all cached blocks flushed\n");
        return;
    }

    if (strncmp(args, "flush ", 6) == 0) {
        uint32_t dev = parse_uint(args + 6);
        if (blockdev_flush(dev) < 0) {
            terminal_writestring("blk: device flush failed\n");
            return;
        }
        terminal_writestring("blk: device cache flushed\n");
        return;
    }

    terminal_writestring("usage: blk list | blk read <dev> <lba> | blk write <dev> <lba> <seed-byte> | blk cache | blk async | blk flush [dev]\n");
}

static void cmd_disk(const char* args) {
    if (!args || *args == '\0' || strcmp(args, "help") == 0) {
        terminal_writestring("usage:\n");
        terminal_writestring("  disk parts\n");
        terminal_writestring("  disk init <disk>\n");
        terminal_writestring("  disk initfat32 <disk>\n");
        terminal_writestring("  disk mkfs <partition>\n");
        terminal_writestring("  disk mkfsfat32 <partition>\n");
        terminal_writestring("  disk mount <partition>\n");
        terminal_writestring("  disk markdirty <partition>\n");
        terminal_writestring("  disk setup <disk>\n");
        terminal_writestring("  disk mountfat32 <partition>\n");
        terminal_writestring("  disk setupfat32 <disk>\n");
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

    if (strncmp(args, "init ", 5) == 0 || strncmp(args, "initfat32 ", 10) == 0 ||
        strncmp(args, "mkfs ", 5) == 0 || strncmp(args, "mkfsfat32 ", 10) == 0 ||
        strncmp(args, "mount ", 6) == 0 || strncmp(args, "markdirty ", 10) == 0 ||
        strncmp(args, "setup ", 6) == 0 || strncmp(args, "mountfat32 ", 11) == 0 ||
        strncmp(args, "setupfat32 ", 11) == 0) {
        int is_init = strncmp(args, "init ", 5) == 0;
        int is_initfat32 = strncmp(args, "initfat32 ", 10) == 0;
        int is_mkfs = strncmp(args, "mkfs ", 5) == 0;
        int is_mkfsfat32 = strncmp(args, "mkfsfat32 ", 10) == 0;
        int is_mount = strncmp(args, "mount ", 6) == 0;
        int is_markdirty = strncmp(args, "markdirty ", 10) == 0;
        int is_setup = strncmp(args, "setup ", 6) == 0;
        int is_mountfat32 = strncmp(args, "mountfat32 ", 11) == 0;
        int is_setupfat32 = strncmp(args, "setupfat32 ", 11) == 0;
        const char* p = args + (is_init ? 5 :
                                is_initfat32 ? 10 :
                                is_mkfs ? 5 :
                                is_mkfsfat32 ? 10 :
                                is_mount ? 6 :
                                is_markdirty ? 10 :
                                is_setup ? 6 : 11);
        char devtok[16];
        const block_device_t* dev = resolve_blockdev_token(p, devtok, sizeof(devtok));
        if (!dev) {
            terminal_writestring("disk: unknown device\n");
            return;
        }

        if (is_init || is_initfat32) {
            uint8_t ptype = is_initfat32 ? FAT32_PARTITION_TYPE_FAT32_LBA : 0x83u;
            int rc = mbr_create_single_partition(dev->id, ptype);
            if (rc < 0) {
                terminal_writestring("disk: failed to write MBR\n");
                return;
            }
            mbr_scan_all();
            terminal_writestring("disk: created single ");
            terminal_writestring(is_initfat32 ? "FAT32" : "Linux");
            terminal_writestring(" partition on ");
            terminal_writestring(dev->name);
            terminal_writestring(" (not formatted)\n");
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

        if (is_mkfsfat32) {
            int rc = fat32_format(dev->id);
            if (rc < 0) {
                terminal_writestring("disk: mkfsfat32 failed\n");
                return;
            }
            terminal_writestring("disk: FAT32 formatted on ");
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

        if (is_markdirty) {
            int rc = minfs_test_mark_dirty(dev->id);
            if (rc < 0) {
                terminal_writestring("disk: markdirty failed (mount minfs first)\n");
                return;
            }
            terminal_writestring("disk: recovery marker set DIRTY on ");
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

        if (is_mountfat32) {
            char mount_path[VFS_PATH_MAX];
            strcpy(mount_path, "/mnt/");
            strncat(mount_path, dev->name, VFS_PATH_MAX - strlen(mount_path) - 1);
            int rc = fat32_mount(dev->id, 0, mount_path);
            if (rc < 0) {
                terminal_writestring("disk: FAT32 mount failed\n");
                return;
            }
            terminal_writestring("disk: mounted FAT32 at ");
            terminal_writestring(mount_path);
            terminal_writestring("\n");
            return;
        }

        if (is_setupfat32) {
            int rc = mbr_create_single_partition(dev->id, FAT32_PARTITION_TYPE_FAT32_LBA);
            if (rc < 0) {
                terminal_writestring("disk: setupfat32 failed while creating MBR\n");
                return;
            }
            mbr_scan_all();

            char pname[16];
            strncpy(pname, dev->name, sizeof(pname) - 3);
            pname[sizeof(pname) - 3] = '\0';
            strcat(pname, "p1");

            const block_device_t* part = blockdev_find_by_name(pname);
            if (!part) {
                terminal_writestring("disk: setupfat32 could not find new partition\n");
                return;
            }

            if (fat32_format(part->id) < 0) {
                terminal_writestring("disk: setupfat32 format failed\n");
                return;
            }

            char mount_path[VFS_PATH_MAX];
            strcpy(mount_path, "/mnt/");
            strncat(mount_path, pname, VFS_PATH_MAX - strlen(mount_path) - 1);
            if (fat32_mount(part->id, 0, mount_path) < 0) {
                terminal_writestring("disk: setupfat32 mount failed\n");
                return;
            }

            terminal_writestring("disk: FAT32 ready at ");
            terminal_writestring(mount_path);
            terminal_writestring("\n");
            return;
        }
    }

    terminal_writestring("usage: disk parts | disk init <disk> | disk initfat32 <disk> | disk mkfs <partition> | disk mkfsfat32 <partition> | disk mount <partition> | disk markdirty <partition> | disk setup <disk> | disk mountfat32 <partition> | disk setupfat32 <disk>\n");
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

static void cmd_fat32test(const char* args) {
    if (!args || *args == '\0') {
        terminal_writestring("usage: fat32test <existing-file-path>\n");
        terminal_writestring("note: file must already exist on a FAT32 mount\n");
        return;
    }

    char path_arg[VFS_PATH_MAX];
    copy_token(args, path_arg, sizeof(path_arg));
    if (!*path_arg) {
        terminal_writestring("usage: fat32test <existing-file-path>\n");
        return;
    }

    char pathbuf[VFS_PATH_MAX];
    const char* path = resolve_shell_path(path_arg, pathbuf, sizeof(pathbuf));
    vfs_node_t* node = vfs_namei(path);
    if (!node || !(node->type & VFS_FILE)) {
        terminal_writestring("fat32test: file not found: ");
        terminal_writestring(path);
        terminal_writestring("\n");
        return;
    }

    int pass = 0;
    int fail = 0;

    terminal_writestring("[fat32test] begin ");
    terminal_writestring(path);
    terminal_writestring("\n");

    /* Test 1: truncate to zero via open flags. */
    {
        int fd = vfs_fd_open(path, VFS_O_RDWR | VFS_O_TRUNC);
        if (fd < 0) {
            terminal_writestring("[fat32test] FAIL truncate open\n");
            fail++;
        } else {
            vfs_fd_close(fd);
            node = vfs_namei(path);
            if (node && node->size == 0) {
                terminal_writestring("[fat32test] PASS truncate size=0\n");
                pass++;
            } else {
                terminal_writestring("[fat32test] FAIL truncate size check\n");
                fail++;
            }
        }
    }

    /* Test 2: write deterministic bytes and read back. */
    {
        uint8_t wbuf[256];
        uint8_t rbuf[256];
        for (uint32_t i = 0; i < sizeof(wbuf); ++i) wbuf[i] = (uint8_t)((0x11u + i) & 0xFFu);

        int fd = vfs_fd_open(path, VFS_O_RDWR);
        int ok = 1;
        if (fd < 0) ok = 0;
        if (ok && vfs_fd_write(fd, wbuf, sizeof(wbuf)) != (int32_t)sizeof(wbuf)) ok = 0;
        if (fd >= 0) vfs_fd_close(fd);

        node = vfs_namei(path);
        if (!node || vfs_read(node, 0, sizeof(rbuf), rbuf) != (int32_t)sizeof(rbuf)) ok = 0;
        if (ok && memcmp(wbuf, rbuf, sizeof(wbuf)) != 0) ok = 0;

        if (ok) {
            terminal_writestring("[fat32test] PASS write/read 256\n");
            pass++;
        } else {
            terminal_writestring("[fat32test] FAIL write/read 256\n");
            fail++;
        }
    }

    /* Test 3: in-place overwrite at non-zero offset. */
    {
        uint8_t patch[64];
        uint8_t check[64];
        for (uint32_t i = 0; i < sizeof(patch); ++i) patch[i] = (uint8_t)((0xA0u + i) & 0xFFu);

        int ok = 1;
        node = vfs_namei(path);
        if (!node) ok = 0;
        if (ok && vfs_write(node, 96, sizeof(patch), patch) != (int32_t)sizeof(patch)) ok = 0;
        if (ok && vfs_read(node, 96, sizeof(check), check) != (int32_t)sizeof(check)) ok = 0;
        if (ok && memcmp(patch, check, sizeof(patch)) != 0) ok = 0;

        if (ok) {
            terminal_writestring("[fat32test] PASS overwrite@96\n");
            pass++;
        } else {
            terminal_writestring("[fat32test] FAIL overwrite@96\n");
            fail++;
        }
    }

    /* Test 4: append semantics and appended region integrity. */
    {
        uint8_t app[73];
        uint8_t chk[73];
        for (uint32_t i = 0; i < sizeof(app); ++i) app[i] = (uint8_t)((0x55u + i) & 0xFFu);

        int ok = 1;
        node = vfs_namei(path);
        uint32_t old_size = node ? node->size : 0;
        int fd = vfs_fd_open(path, VFS_O_WRONLY | VFS_O_APPEND);
        if (fd < 0) ok = 0;
        if (ok && vfs_fd_write(fd, app, sizeof(app)) != (int32_t)sizeof(app)) ok = 0;
        if (fd >= 0) vfs_fd_close(fd);

        node = vfs_namei(path);
        if (!node || node->size != old_size + (uint32_t)sizeof(app)) ok = 0;
        if (ok && vfs_read(node, old_size, sizeof(chk), chk) != (int32_t)sizeof(chk)) ok = 0;
        if (ok && memcmp(app, chk, sizeof(app)) != 0) ok = 0;

        if (ok) {
            terminal_writestring("[fat32test] PASS append\n");
            pass++;
        } else {
            terminal_writestring("[fat32test] FAIL append\n");
            fail++;
        }
    }

    /* Test 5: large write/read crossing cluster boundaries. */
    {
        const uint32_t total = 70000u;
        uint8_t chunk[257];
        uint8_t readback[257];
        uint32_t done = 0;
        int ok = 1;

        int fd = vfs_fd_open(path, VFS_O_RDWR | VFS_O_TRUNC);
        if (fd < 0) ok = 0;

        while (ok && done < total) {
            uint32_t n = total - done;
            if (n > sizeof(chunk)) n = sizeof(chunk);
            for (uint32_t i = 0; i < n; ++i) {
                chunk[i] = (uint8_t)((0x3Cu + done + i) & 0xFFu);
            }
            if (vfs_fd_write(fd, chunk, n) != (int32_t)n) ok = 0;
            done += n;
        }
        if (fd >= 0) vfs_fd_close(fd);

        node = vfs_namei(path);
        if (!node || node->size != total) ok = 0;

        done = 0;
        fd = vfs_fd_open(path, VFS_O_RDONLY);
        if (fd < 0) ok = 0;
        while (ok && done < total) {
            uint32_t n = total - done;
            if (n > sizeof(readback)) n = sizeof(readback);
            if (vfs_fd_read(fd, readback, n) != (int32_t)n) {
                ok = 0;
                break;
            }
            for (uint32_t i = 0; i < n; ++i) {
                uint8_t expect = (uint8_t)((0x3Cu + done + i) & 0xFFu);
                if (readback[i] != expect) {
                    ok = 0;
                    break;
                }
            }
            done += n;
        }
        if (fd >= 0) vfs_fd_close(fd);

        if (ok) {
            terminal_writestring("[fat32test] PASS large-io\n");
            pass++;
        } else {
            terminal_writestring("[fat32test] FAIL large-io\n");
            fail++;
        }
    }

    terminal_writestring("[fat32test] result: ");
    term_write_u32((uint32_t)pass);
    terminal_writestring(" pass, ");
    term_write_u32((uint32_t)fail);
    terminal_writestring(" fail\n");
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

static void cmd_vm(const char* args) {
    char op[16];
    copy_token(args ? args : "", op, sizeof(op));

    if (op[0] == '\0' || strcmp(op, "stats") == 0) {
        paging_stats_t st;
        paging_get_stats(&st);
        char n[16];
        terminal_writestring("VM stats:\n");
        terminal_writestring("  demand regions: ");
        itoa((int)st.demand_regions, n, 10);
        terminal_writestring(n);
        terminal_writestring("\n  demand faults:  ");
        itoa((int)st.demand_faults, n, 10);
        terminal_writestring(n);
        terminal_writestring("\n  COW faults:     ");
        itoa((int)st.cow_faults, n, 10);
        terminal_writestring(n);
        terminal_writestring("\n  swap enabled:   ");
        terminal_writestring(st.swap_enabled ? "yes" : "no");
        terminal_writestring("\n  swap slots:     ");
        itoa((int)st.swap_slots_used, n, 10);
        terminal_writestring(n);
        terminal_writestring("/");
        itoa((int)st.swap_slots_total, n, 10);
        terminal_writestring(n);
        terminal_writestring("\n  swap page-outs: ");
        itoa((int)st.swap_pageouts, n, 10);
        terminal_writestring(n);
        terminal_writestring("\n  swap page-ins:  ");
        itoa((int)st.swap_pageins, n, 10);
        terminal_writestring(n);
        terminal_writestring("\n  swap faults:    ");
        itoa((int)st.swap_faults, n, 10);
        terminal_writestring(n);
        terminal_writestring("\n");
        return;
    }

    if (strcmp(op, "demand") == 0) {
        const char* p = next_token(args ? args : "");

        uintptr_t base = 0x50000000u;
        uint32_t pages = 4;

        if (p && *p) {
            base = (uintptr_t)parse_u32_auto(p);
            p = next_token(p);
            if (p && *p) pages = parse_uint(p);
        }
        if (pages == 0) pages = 1;

        uint32_t size = pages * 4096u;
        int rc = paging_register_demand_region(base, size, PAGING_FLAG_WRITE | PAGING_FLAG_USER);
        if (rc < 0) {
            terminal_writestring("vm demand: register failed\n");
            return;
        }

        volatile uint8_t* mem = (volatile uint8_t*)base;
        for (uint32_t i = 0; i < pages; ++i) {
            mem[i * 4096u] = (uint8_t)(0xA0u + (i & 0x0Fu));
        }

        terminal_writestring("vm demand: touched pages at 0x");
        char b[16];
        itoa((int)base, b, 16);
        terminal_writestring(b);
        terminal_writestring("\n");
        return;
    }

    if (strcmp(op, "cow") == 0) {
        uintptr_t va = 0x51000000u;
        int rc = paging_map_user(va);
        if (rc < 0) {
            terminal_writestring("vm cow: map failed\n");
            return;
        }

        volatile uint8_t* p = (volatile uint8_t*)va;
        p[0] = 0x11;

        if (paging_mark_cow(va) < 0) {
            terminal_writestring("vm cow: mark failed\n");
            return;
        }

        p[0] = 0x22; /* write triggers COW path/hook */
        terminal_writestring("vm cow: ok\n");
        return;
    }

    if (strcmp(op, "refs") == 0) {
        task_t* cur = task_current();
        uint32_t task_cr3 = cur ? (cur->mm_cr3 & ~0xFFFu) : 0;
        uint32_t active_cr3 = paging_current_cr3() & ~0xFFFu;
        uint32_t refs = paging_mm_refcount(task_cr3 ? task_cr3 : active_cr3);

        char n[16];
        terminal_writestring("VM refs:\n  task mm_cr3: 0x");
        itoa((int)task_cr3, n, 16);
        terminal_writestring(n);
        terminal_writestring("\n  active cr3:  0x");
        itoa((int)active_cr3, n, 16);
        terminal_writestring(n);
        terminal_writestring("\n  mm refs:     ");
        itoa((int)refs, n, 10);
        terminal_writestring(n);
        terminal_writestring("\n");
        return;
    }

    terminal_writestring("usage: vm stats | vm demand [addr pages] | vm cow | vm refs\n");
}

static void cmd_net(const char* args) {
    if (args && strcmp(args, "netstat") == 0) {
        const netif_t* nif;
        uint8_t mac[6];
        uint8_t ip[4];
        uint8_t mask[4];
        uint8_t gw[4];
        net_stats_t st;
        net_arp_stats_t ast;
        net_arp_entry_t entries[16];
        net_tcp_debug_stats_t tcp;
        uint32_t arp_count;

        net_get_mac(mac);
        net_get_ipv4(ip);
        net_get_ipv4_netmask(mask);
        net_get_ipv4_gateway(gw);
        net_get_stats(&st);
        net_get_arp_stats(&ast);
        net_tcp_get_debug_stats(&tcp);
        arp_count = net_get_arp_cache(entries, 16u);
        nif = net_default_netif();

        terminal_writestring("netstat-lite:\n");
        terminal_writestring("  driver=");
        terminal_writestring(net_driver_name());
        terminal_writestring(" link=");
        terminal_writestring(net_link_up() ? "up" : "down");
        terminal_writestring(" mac=");
        term_write_hex8(mac[0]); terminal_writestring(":");
        term_write_hex8(mac[1]); terminal_writestring(":");
        term_write_hex8(mac[2]); terminal_writestring(":");
        term_write_hex8(mac[3]); terminal_writestring(":");
        term_write_hex8(mac[4]); terminal_writestring(":");
        term_write_hex8(mac[5]);
        terminal_writestring("\n");

        terminal_writestring("  inet=");
        term_write_ipv4(ip);
        terminal_writestring(" mask=");
        term_write_ipv4(mask);
        terminal_writestring(" gw=");
        term_write_ipv4(gw);
        terminal_writestring("\n");

        terminal_writestring("  nic: irq=");
        term_write_u32(st.interrupts);
        terminal_writestring(" rx=");
        term_write_u32(st.rx_packets);
        terminal_writestring(" tx=");
        term_write_u32(st.tx_packets);
        terminal_writestring(" drops=");
        term_write_u32(st.rx_drops);
        terminal_writestring("\n");

        if (nif) {
            terminal_writestring("  netif: flags=0x");
            term_write_hex8(nif->flags);
            terminal_writestring(" rx=");
            term_write_u32(nif->rx_frames);
            terminal_writestring(" tx=");
            term_write_u32(nif->tx_frames);
            terminal_writestring(" drops=");
            term_write_u32(nif->rx_drops);
            terminal_writestring(" linkchg=");
            term_write_u32(nif->link_changes);
            terminal_writestring("\n");
        }

        terminal_writestring("  arp: entries=");
        term_write_u32(arp_count);
        terminal_writestring(" hit=");
        term_write_u32(ast.cache_hits);
        terminal_writestring(" miss=");
        term_write_u32(ast.cache_misses);
        terminal_writestring(" rx=");
        term_write_u32(ast.rx_arp_packets);
        terminal_writestring(" txreq=");
        term_write_u32(ast.tx_arp_requests);
        terminal_writestring(" txrep=");
        term_write_u32(ast.tx_arp_replies);
        terminal_writestring("\n");

        if (arp_count > 0u) {
            terminal_writestring("  arp-cache:\n");
            for (uint32_t i = 0; i < arp_count; ++i) {
                terminal_writestring("    ");
                term_write_ipv4(entries[i].ip);
                terminal_writestring(" -> ");
                term_write_hex8(entries[i].mac[0]); terminal_writestring(":");
                term_write_hex8(entries[i].mac[1]); terminal_writestring(":");
                term_write_hex8(entries[i].mac[2]); terminal_writestring(":");
                term_write_hex8(entries[i].mac[3]); terminal_writestring(":");
                term_write_hex8(entries[i].mac[4]); terminal_writestring(":");
                term_write_hex8(entries[i].mac[5]);
                terminal_writestring("\n");
            }
        }

        terminal_writestring("  tcp-probe: syn=");
        term_write_u32(tcp.syn_sent);
        terminal_writestring(" syn_retx=");
        term_write_u32(tcp.syn_retx);
        terminal_writestring(" synack=");
        term_write_u32(tcp.synack_seen);
        terminal_writestring(" rst=");
        term_write_u32(tcp.rst_seen);
        terminal_writestring(" ok=");
        term_write_u32(tcp.connect_ok);
        terminal_writestring(" timeout=");
        term_write_u32(tcp.connect_timeout);
        terminal_writestring("\n");
        return;
    }

    if (args && strncmp(args, "dhcp", 4) == 0 && (args[4] == '\0' || args[4] == ' ')) {
        const char* tok = next_token(args);
        uint32_t timeout_ms = 6000u;
        uint8_t mac[6];
        uint8_t bcast[4] = {255u, 255u, 255u, 255u};
        uint8_t discover[300];
        uint8_t request[300];
        uint8_t response[600];
        uint8_t from_ip[4];
        uint8_t offer_ip[4] = {0, 0, 0, 0};
        uint8_t mask_ip[4] = {255u, 255u, 255u, 0u};
        uint8_t gw_ip[4] = {0, 0, 0, 0};
        uint8_t server_id[4] = {0, 0, 0, 0};
        uint32_t lease_s = 3600u;
        uint32_t t1_s = 0u;
        uint32_t t2_s = 0u;
        net_udp_endpoint_t from;
        uint32_t xid;
        uint32_t start_ms;
        int sock;

        if (tok && *tok) {
            if (!is_decimal_token(tok)) {
                terminal_writestring("usage: net dhcp [timeout_ms]\n");
                return;
            }
            timeout_ms = parse_uint(tok);
            if (timeout_ms == 0u) timeout_ms = 1u;
            if (timeout_ms > 30000u) timeout_ms = 30000u;
        }

        if (!net_is_ready()) {
            terminal_writestring("net dhcp: NIC not ready\n");
            return;
        }

        net_get_mac(mac);
        xid = (uint32_t)pit_get_uptime_ms() ^ 0x44484350u;
        sock = net_udp_socket_open();
        if (sock < 0) {
            terminal_writestring("net dhcp: socket open failed\n");
            return;
        }
        if (net_udp_socket_bind(sock, 68u) < 0) {
            terminal_writestring("net dhcp: bind 68 failed\n");
            (void)net_udp_socket_close(sock);
            return;
        }

        memset(discover, 0, sizeof(discover));
        discover[0] = 1u;   /* BOOTREQUEST */
        discover[1] = 1u;   /* Ethernet */
        discover[2] = 6u;   /* MAC length */
        discover[4] = (uint8_t)((xid >> 24) & 0xFFu);
        discover[5] = (uint8_t)((xid >> 16) & 0xFFu);
        discover[6] = (uint8_t)((xid >> 8) & 0xFFu);
        discover[7] = (uint8_t)(xid & 0xFFu);
        discover[10] = 0x80u; /* broadcast flag */
        memcpy(discover + 28, mac, 6);
        discover[236] = 99u;
        discover[237] = 130u;
        discover[238] = 83u;
        discover[239] = 99u;
        {
            uint16_t off = 240u;
            discover[off++] = 53u; discover[off++] = 1u; discover[off++] = 1u; /* DISCOVER */
            discover[off++] = 55u; discover[off++] = 3u; discover[off++] = 1u; discover[off++] = 3u; discover[off++] = 6u;
            discover[off++] = 255u;
            if (net_udp_socket_sendto(sock, bcast, 67u, discover, off) != NET_UDP_OK) {
                terminal_writestring("net dhcp: discover send failed\n");
                (void)net_udp_socket_close(sock);
                return;
            }
        }

        terminal_writestring("net dhcp: waiting for OFFER...\n");
        start_ms = (uint32_t)pit_get_uptime_ms();
        for (;;) {
            uint32_t elapsed = (uint32_t)pit_get_uptime_ms() - start_ms;
            uint32_t remain = elapsed >= timeout_ms ? 0u : (timeout_ms - elapsed);
            uint16_t rx_len = 0;
            int rx_rc;
            uint8_t msg_type = 0;
            uint32_t rx_xid;

            if (remain == 0u) {
                terminal_writestring("net dhcp: OFFER timeout\n");
                (void)net_udp_socket_close(sock);
                return;
            }
            if (remain > 500u) remain = 500u;

            rx_rc = net_udp_socket_recvfrom(sock, response, sizeof(response), &rx_len, &from, remain);
            if (rx_rc == NET_UDP_ERR_WOULD_BLOCK) continue;
            if (rx_rc < 0 || rx_len < 244u) continue;
            if (response[0] != 2u || response[1] != 1u || response[2] != 6u) continue;

            rx_xid = ((uint32_t)response[4] << 24) |
                     ((uint32_t)response[5] << 16) |
                     ((uint32_t)response[6] << 8) |
                     (uint32_t)response[7];
            if (rx_xid != xid) continue;
            if (response[236] != 99u || response[237] != 130u || response[238] != 83u || response[239] != 99u) continue;

            if (net_dhcp_parse_options(response, rx_len, &msg_type, server_id, mask_ip, gw_ip, &lease_s, &t1_s, &t2_s) != 0) {
                continue;
            }

            if (msg_type != 2u) continue;
            memcpy(offer_ip, response + 16, 4);
            if ((server_id[0] | server_id[1] | server_id[2] | server_id[3]) == 0u) {
                memcpy(server_id, from.ip, 4);
            }
            break;
        }

        memset(request, 0, sizeof(request));
        request[0] = 1u;
        request[1] = 1u;
        request[2] = 6u;
        request[4] = (uint8_t)((xid >> 24) & 0xFFu);
        request[5] = (uint8_t)((xid >> 16) & 0xFFu);
        request[6] = (uint8_t)((xid >> 8) & 0xFFu);
        request[7] = (uint8_t)(xid & 0xFFu);
        request[10] = 0x80u;
        memcpy(request + 28, mac, 6);
        request[236] = 99u;
        request[237] = 130u;
        request[238] = 83u;
        request[239] = 99u;
        {
            uint16_t off = 240u;
            request[off++] = 53u; request[off++] = 1u; request[off++] = 3u; /* REQUEST */
            request[off++] = 50u; request[off++] = 4u;
            request[off++] = offer_ip[0]; request[off++] = offer_ip[1]; request[off++] = offer_ip[2]; request[off++] = offer_ip[3];
            request[off++] = 54u; request[off++] = 4u;
            request[off++] = server_id[0]; request[off++] = server_id[1]; request[off++] = server_id[2]; request[off++] = server_id[3];
            request[off++] = 55u; request[off++] = 3u; request[off++] = 1u; request[off++] = 3u; request[off++] = 6u;
            request[off++] = 255u;
            if (net_udp_socket_sendto(sock, bcast, 67u, request, off) != NET_UDP_OK) {
                terminal_writestring("net dhcp: request send failed\n");
                (void)net_udp_socket_close(sock);
                return;
            }
        }

        terminal_writestring("net dhcp: waiting for ACK...\n");
        start_ms = (uint32_t)pit_get_uptime_ms();
        for (;;) {
            uint32_t elapsed = (uint32_t)pit_get_uptime_ms() - start_ms;
            uint32_t remain = elapsed >= timeout_ms ? 0u : (timeout_ms - elapsed);
            uint16_t rx_len = 0;
            int rx_rc;
            uint8_t msg_type = 0;
            uint32_t rx_xid;

            if (remain == 0u) {
                terminal_writestring("net dhcp: ACK timeout\n");
                (void)net_udp_socket_close(sock);
                return;
            }
            if (remain > 500u) remain = 500u;

            rx_rc = net_udp_socket_recvfrom(sock, response, sizeof(response), &rx_len, &from, remain);
            if (rx_rc == NET_UDP_ERR_WOULD_BLOCK) continue;
            if (rx_rc < 0 || rx_len < 244u) continue;
            if (response[0] != 2u || response[1] != 1u || response[2] != 6u) continue;

            rx_xid = ((uint32_t)response[4] << 24) |
                     ((uint32_t)response[5] << 16) |
                     ((uint32_t)response[6] << 8) |
                     (uint32_t)response[7];
            if (rx_xid != xid) continue;
            if (response[236] != 99u || response[237] != 130u || response[238] != 83u || response[239] != 99u) continue;

            if (net_dhcp_parse_options(response, rx_len, &msg_type, server_id, mask_ip, gw_ip, &lease_s, &t1_s, &t2_s) != 0) {
                continue;
            }

            if (msg_type == 6u) {
                terminal_writestring("net dhcp: NAK received\n");
                (void)net_udp_socket_close(sock);
                return;
            }
            if (msg_type != 5u) continue;
            memcpy(from_ip, response + 16, 4);
            if ((from_ip[0] | from_ip[1] | from_ip[2] | from_ip[3]) != 0u) {
                memcpy(offer_ip, from_ip, 4);
            }
            break;
        }

        (void)net_udp_socket_close(sock);
        net_set_ipv4(offer_ip[0], offer_ip[1], offer_ip[2], offer_ip[3]);
        net_set_ipv4_netmask(mask_ip[0], mask_ip[1], mask_ip[2], mask_ip[3]);
        if ((gw_ip[0] | gw_ip[1] | gw_ip[2] | gw_ip[3]) != 0u) {
            net_set_ipv4_gateway(gw_ip[0], gw_ip[1], gw_ip[2], gw_ip[3]);
        }
        net_dhcp_client_seed(server_id, offer_ip, mask_ip, gw_ip, lease_s, t1_s, t2_s);

        terminal_writestring("net dhcp: lease acquired ip=");
        term_write_ipv4(offer_ip);
        terminal_writestring(" mask=");
        term_write_ipv4(mask_ip);
        terminal_writestring(" gw=");
        term_write_ipv4(gw_ip);
        terminal_writestring("\n");
        return;
    }

    if (args && strcmp(args, "regs") == 0) {
        net_debug_info_t dbg;
        char buf[24];
        net_get_debug_info(&dbg);

        terminal_writestring("net regs:\n");
        terminal_writestring("  pci=");
        itoa((int)dbg.vendor_id, buf, 16);
        terminal_writestring(buf);
        terminal_writestring(":");
        itoa((int)dbg.device_id, buf, 16);
        terminal_writestring(buf);
        terminal_writestring(" io=0x");
        itoa((int)dbg.io_base, buf, 16);
        terminal_writestring(buf);
        terminal_writestring(" irq=");
        itoa((int)dbg.irq, buf, 10);
        terminal_writestring(buf);
        terminal_writestring("\n");

        terminal_writestring("  reg_a=0x");
        itoa((int)dbg.reg_a, buf, 16);
        terminal_writestring(buf);
        terminal_writestring(" reg_b=0x");
        itoa((int)dbg.reg_b, buf, 16);
        terminal_writestring(buf);
        terminal_writestring("\n");

        terminal_writestring("  reg_c=0x");
        itoa((int)dbg.reg_c, buf, 16);
        terminal_writestring(buf);
        terminal_writestring(" reg_d=0x");
        itoa((int)dbg.reg_d, buf, 16);
        terminal_writestring(buf);
        terminal_writestring("\n");
        return;
    }

    if (args && (strcmp(args, "rxdefer") == 0 || strcmp(args, "dbg rxdefer") == 0)) {
        net_rx_defer_stats_t st;
        uint32_t queued_est = 0u;

        net_get_rx_defer_stats(&st);
        if (st.enqueued > st.dequeued) queued_est = st.enqueued - st.dequeued;

        terminal_writestring("net rxdefer: enq=");
        term_write_u32(st.enqueued);
        terminal_writestring(" deq=");
        term_write_u32(st.dequeued);
        terminal_writestring(" queued~=");
        term_write_u32(queued_est);
        terminal_writestring(" drop_pool=");
        term_write_u32(st.drop_pool_empty);
        terminal_writestring(" drop_qfull=");
        term_write_u32(st.drop_queue_full);
        terminal_writestring(" drop_oversz=");
        term_write_u32(st.drop_too_large);
        terminal_writestring(" drop_inv=");
        term_write_u32(st.drop_invalid);
        terminal_writestring("\n");
        return;
    }

    if (args && strcmp(args, "timers") == 0) {
        const netif_t* nif = net_default_netif();
        net_timer_debug_t t;
        net_p2_stats_t p2;

        net_get_timer_debug(&t);
        net_get_p2_stats(&p2);

        terminal_writestring("timers: link[");
        term_write_u32(t.link_refresh_count);
        terminal_writestring(" refresh, ");
        term_write_u32(t.link_state_changes);
        terminal_writestring(" changes, ");
        term_write_u32(t.link_refresh_period_ms);
        terminal_writestring("ms]\n");

        terminal_writestring("  dhcp lease=");
        term_write_u32(t.dhcp_lease_remaining_ms);
        terminal_writestring(" t1=");
        term_write_u32(t.dhcp_t1_remaining_ms);
        terminal_writestring(" t2=");
        term_write_u32(t.dhcp_t2_remaining_ms);
        terminal_writestring(" retry=");
        term_write_u32(t.dhcp_retry_remaining_ms);
        terminal_writestring(" count=");
        term_write_u32(t.dhcp_retry_count);
        terminal_writestring("\n");

        terminal_writestring("  tcp rtx active=");
        term_write_u32(t.tcp_rtx_active);
        terminal_writestring(" scans=");
        term_write_u32(t.tcp_rtx_scans);
        terminal_writestring(" due=");
        term_write_u32(t.tcp_rtx_due);
        terminal_writestring(" sent=");
        term_write_u32(t.tcp_rtx_sent);
        terminal_writestring(" timeout=");
        term_write_u32(t.tcp_rtx_timeout);
        terminal_writestring("\n");

        terminal_writestring("  ipv4 frag ok=");
        term_write_u32(p2.ipv4_frag_reasm_ok);
        terminal_writestring(" drop=");
        term_write_u32(p2.ipv4_frag_reasm_drop);
        terminal_writestring("\n");

        if (nif) {
            terminal_writestring("  netif rx=");
            term_write_u32(nif->rx_frames);
            terminal_writestring(" tx=");
            term_write_u32(nif->tx_frames);
            terminal_writestring(" drops=");
            term_write_u32(nif->rx_drops);
            terminal_writestring(" linkchg=");
            term_write_u32(nif->link_changes);
            terminal_writestring("\n");
        }
        return;
    }

    if (args && strcmp(args, "p2") == 0) {
        net_p2_stats_t p2;
        net_get_p2_stats(&p2);
        terminal_writestring("p2: ipv4_bad="); term_write_u32(p2.ipv4_malformed);
        terminal_writestring(" frag_rx="); term_write_u32(p2.ipv4_frag_rx);
        terminal_writestring(" frag_ok="); term_write_u32(p2.ipv4_frag_reasm_ok);
        terminal_writestring(" frag_drop="); term_write_u32(p2.ipv4_frag_reasm_drop);
        terminal_writestring(" icmp_unr="); term_write_u32(p2.icmp_rx_unreach);
        terminal_writestring(" icmp_tex="); term_write_u32(p2.icmp_rx_timeex);
        terminal_writestring(" icmp_parm="); term_write_u32(p2.icmp_rx_param);
        terminal_writestring("\n");

        terminal_writestring("    dns hit="); term_write_u32(p2.dns_cache_hit);
        terminal_writestring(" miss="); term_write_u32(p2.dns_cache_miss);
        terminal_writestring(" neg_hit="); term_write_u32(p2.dns_cache_neg_hit);
        terminal_writestring(" ins="); term_write_u32(p2.dns_cache_insert);
        terminal_writestring(" evict="); term_write_u32(p2.dns_cache_evict);
        terminal_writestring(" retry="); term_write_u32(p2.dns_query_retry);
        terminal_writestring(" backoff="); term_write_u32(p2.dns_query_backoff);
        terminal_writestring(" timeout="); term_write_u32(p2.dns_query_timeout);
        terminal_writestring(" cache_entries="); term_write_u32(net_dns_cache_count());
        terminal_writestring("\n");
        return;
    }

    if (args && strncmp(args, "test ", 5) == 0) {
        const char* mode = next_token(args);
        if (!mode) {
            terminal_writestring("usage: net test regress | net test fuzz [count] [seed] | net test stress [count] [seed]\n");
            return;
        }

        if (strncmp(mode, "regress", 7) == 0) {
            uint8_t f1[34] = {0};
            uint8_t f2[42] = {0};
            uint8_t ip[4];
            net_get_ipv4(ip);

            f1[12] = 0x08; f1[13] = 0x00;
            f1[14] = 0x44; /* bad IHL */
            f1[16] = 0x00; f1[17] = 0x14;
            memcpy(f1 + 30, ip, 4);
            net_core_input(f1, sizeof(f1));

            f2[12] = 0x08; f2[13] = 0x00;
            f2[14] = 0x45;
            f2[16] = 0x00; f2[17] = 0x1C;
            f2[23] = 17; /* UDP */
            f2[20] = 0x20; f2[21] = 0x01; /* MF + offset */
            memcpy(f2 + 30, ip, 4);
            net_core_input(f2, sizeof(f2));

            terminal_writestring("net test regress: injected malformed/fragment cases\n");
            return;
        }

        if (strncmp(mode, "fuzz", 4) == 0 || strncmp(mode, "stress", 6) == 0) {
            const char* n_tok = next_token(mode);
            const char* seed_tok = NULL;
            uint32_t n = 128u;
            uint32_t seed = 0x1234ABCDu;
            uint32_t seed_init;
            char seed_buf[24];
            uint8_t frame[96];

            if (n_tok && *n_tok) {
                if (!is_decimal_token(n_tok)) {
                    terminal_writestring("usage: net test fuzz [count] [seed] | net test stress [count] [seed]\n");
                    return;
                }
                n = parse_uint(n_tok);
                seed_tok = next_token(n_tok);
                if (seed_tok && *seed_tok) {
                    if (!is_decimal_token(seed_tok)) {
                        terminal_writestring("usage: net test fuzz [count] [seed] | net test stress [count] [seed]\n");
                        return;
                    }
                    seed = parse_uint(seed_tok);
                }
            }
            if (n == 0u) n = 1u;
            if (n > 10000u) n = 10000u;
            seed_init = seed;

            for (uint32_t i = 0; i < n; ++i) {
                uint32_t len = 34u + (seed % 60u);
                if (len > sizeof(frame)) len = sizeof(frame);
                for (uint32_t j = 0; j < len; ++j) {
                    seed = seed * 1664525u + 1013904223u;
                    frame[j] = (uint8_t)(seed >> 24);
                }
                frame[12] = 0x08; frame[13] = 0x00;
                frame[14] = (uint8_t)(0x40u | (frame[14] & 0x0Fu));
                net_core_input(frame, (uint16_t)len);
            }

            terminal_writestring("net test ");
            terminal_writestring(strncmp(mode, "stress", 6) == 0 ? "stress" : "fuzz");
            terminal_writestring(": injected frames=");
            term_write_u32(n);
            terminal_writestring(" seed=");
            itoa((int)seed_init, seed_buf, 10);
            terminal_writestring(seed_buf);
            terminal_writestring("\n");
            return;
        }

        terminal_writestring("usage: net test regress | net test fuzz [count] [seed] | net test stress [count] [seed]\n");
        return;
    }

    if (args && strcmp(args, "arp") == 0) {
        net_arp_entry_t entries[16];
        net_arp_stats_t ast;
        uint32_t count;

        net_get_arp_stats(&ast);
        count = net_get_arp_cache(entries, 16);

        terminal_writestring("arp: cache_entries=");
        term_write_u32(count);
        terminal_writestring(" rx=");
        term_write_u32(ast.rx_arp_packets);
        terminal_writestring(" req=");
        term_write_u32(ast.rx_arp_requests);
        terminal_writestring(" rep=");
        term_write_u32(ast.rx_arp_replies);
        terminal_writestring(" txreq=");
        term_write_u32(ast.tx_arp_requests);
        terminal_writestring(" txrep=");
        term_write_u32(ast.tx_arp_replies);
        terminal_writestring(" hit=");
        term_write_u32(ast.cache_hits);
        terminal_writestring(" miss=");
        term_write_u32(ast.cache_misses);
        terminal_writestring(" drop=");
        term_write_u32(ast.dropped_frames);
        terminal_writestring("\n");

        for (uint32_t i = 0; i < count; ++i) {
            term_write_ipv4(entries[i].ip);
            terminal_writestring(" -> ");
            term_write_hex8(entries[i].mac[0]); terminal_writestring(":");
            term_write_hex8(entries[i].mac[1]); terminal_writestring(":");
            term_write_hex8(entries[i].mac[2]); terminal_writestring(":");
            term_write_hex8(entries[i].mac[3]); terminal_writestring(":");
            term_write_hex8(entries[i].mac[4]); terminal_writestring(":");
            term_write_hex8(entries[i].mac[5]);
            terminal_writestring("\n");
        }
        return;
    }

    if (args && strcmp(args, "ip") == 0) {
        uint8_t ip[4];
        net_get_ipv4(ip);
        terminal_writestring("net ip: ");
        term_write_ipv4(ip);
        terminal_writestring("\n");
        return;
    }

    if (args && strcmp(args, "mask") == 0) {
        uint8_t mask[4];
        net_get_ipv4_netmask(mask);
        terminal_writestring("net mask: ");
        term_write_ipv4(mask);
        terminal_writestring("\n");
        return;
    }

    if (args && strcmp(args, "gw") == 0) {
        uint8_t gw[4];
        net_get_ipv4_gateway(gw);
        terminal_writestring("net gw: ");
        term_write_ipv4(gw);
        terminal_writestring("\n");
        return;
    }

    if (args && strncmp(args, "ip ", 3) == 0) {
        uint8_t ip[4];
        const char* tok = next_token(args);
        if (!tok || parse_ipv4_token(tok, ip) != 0) {
            terminal_writestring("usage: net ip <a.b.c.d>\n");
            return;
        }
        net_set_ipv4(ip[0], ip[1], ip[2], ip[3]);
        terminal_writestring("net ip set to ");
        term_write_ipv4(ip);
        terminal_writestring("\n");
        return;
    }

    if (args && strncmp(args, "mask ", 5) == 0) {
        uint8_t mask[4];
        const char* tok = next_token(args);
        if (!tok || parse_ipv4_token(tok, mask) != 0) {
            terminal_writestring("usage: net mask <a.b.c.d>\n");
            return;
        }
        net_set_ipv4_netmask(mask[0], mask[1], mask[2], mask[3]);
        terminal_writestring("net mask set to ");
        term_write_ipv4(mask);
        terminal_writestring("\n");
        return;
    }

    if (args && strncmp(args, "gw ", 3) == 0) {
        uint8_t gw[4];
        const char* tok = next_token(args);
        if (!tok || parse_ipv4_token(tok, gw) != 0) {
            terminal_writestring("usage: net gw <a.b.c.d>\n");
            return;
        }
        net_set_ipv4_gateway(gw[0], gw[1], gw[2], gw[3]);
        terminal_writestring("net gw set to ");
        term_write_ipv4(gw);
        terminal_writestring("\n");
        return;
    }

    if (args && strncmp(args, "arping ", 7) == 0) {
        uint8_t ip[4];
        uint8_t mac[6];
        const char* tok = next_token(args);
        if (!tok || parse_ipv4_token(tok, ip) != 0) {
            terminal_writestring("usage: net arping <a.b.c.d>\n");
            return;
        }

        if (net_arp_resolve_retry(ip, mac, 3u, 200u) == 0) {
            terminal_writestring("arping: ");
            term_write_ipv4(ip);
            terminal_writestring(" is at ");
            term_write_hex8(mac[0]); terminal_writestring(":");
            term_write_hex8(mac[1]); terminal_writestring(":");
            term_write_hex8(mac[2]); terminal_writestring(":");
            term_write_hex8(mac[3]); terminal_writestring(":");
            term_write_hex8(mac[4]); terminal_writestring(":");
            term_write_hex8(mac[5]);
            terminal_writestring("\n");
        } else {
            terminal_writestring("arping: no reply\n");
        }
        return;
    }

    if (args && strncmp(args, "ping ", 5) == 0) {
        uint8_t ip[4];
        char target[128];
        uint32_t count = 1u;
        uint32_t timeout_ms = 1200u;
        uint32_t sent = 0;
        uint32_t recv = 0;
        uint64_t total_rtt = 0;
        uint32_t min_rtt = 0xFFFFFFFFu;
        uint32_t max_rtt = 0u;
        const char* tok = next_token(args);
        const char* cursor;
        int saw_pos_count = 0;
        int until_ok = 0;
        char opt[16];
        char num[24];
        if (!tok) {
            terminal_writestring("usage: net ping <host|a.b.c.d> [count] [-c count] [-W timeout_ms] [-U]\n");
            return;
        }
        copy_token(tok, target, sizeof(target));

        if (parse_ipv4_token(tok, ip) != 0) {
            int dns_rc = net_dns_query_a(target, NULL, ip, 2000u);
            if (dns_rc != NET_DNS_OK) {
                terminal_writestring("ping: dns lookup failed host=");
                terminal_writestring(target);
                terminal_writestring(" rc=");
                itoa(dns_rc, num, 10);
                terminal_writestring(num);
                terminal_writestring("\n");
                return;
            }
        }

        cursor = next_token(tok);
        while (cursor && *cursor) {
            copy_token(cursor, opt, sizeof(opt));

            if (strcmp(opt, "-c") == 0) {
                cursor = next_token(cursor);
                if (!cursor || !*cursor || !is_decimal_token(cursor)) {
                    terminal_writestring("usage: net ping <host|a.b.c.d> [count] [-c count] [-W timeout_ms] [-U]\n");
                    return;
                }
                count = parse_uint(cursor);
                cursor = next_token(cursor);
                continue;
            }

            if (strcmp(opt, "-W") == 0) {
                cursor = next_token(cursor);
                if (!cursor || !*cursor || !is_decimal_token(cursor)) {
                    terminal_writestring("usage: net ping <host|a.b.c.d> [count] [-c count] [-W timeout_ms] [-U]\n");
                    return;
                }
                timeout_ms = parse_uint(cursor);
                cursor = next_token(cursor);
                continue;
            }

            if (strcmp(opt, "-U") == 0) {
                until_ok = 1;
                cursor = next_token(cursor);
                continue;
            }

            if (!saw_pos_count && is_decimal_token(opt)) {
                count = parse_uint(opt);
                saw_pos_count = 1;
                cursor = next_token(cursor);
                continue;
            }

            terminal_writestring("usage: net ping <host|a.b.c.d> [count] [-c count] [-W timeout_ms] [-U]\n");
            return;
        }

        if (count == 0u) count = 1u;
        if (count > 32u) count = 32u;
        if (timeout_ms == 0u) timeout_ms = 1u;
        if (timeout_ms > 10000u) timeout_ms = 10000u;

        terminal_writestring("PING ");
        terminal_writestring(target);
        terminal_writestring(" (");
        term_write_ipv4(ip);
        terminal_writestring("): ");
        term_write_u32(count);
        if (until_ok) {
            terminal_writestring(" probe(s, until first reply), timeout=");
        } else {
            terminal_writestring(" probe(s), timeout=");
        }
        term_write_u32(timeout_ms);
        terminal_writestring("ms\n");

        for (uint32_t i = 0; until_ok || i < count; ++i) {
            uint64_t t0 = pit_get_uptime_ms();
            int rc = net_ping_ipv4(ip, (uint16_t)(i + 1u), timeout_ms);
            uint32_t rtt = (uint32_t)(pit_get_uptime_ms() - t0);
            sent++;

            if (rc == NET_PING_OK) {
                recv++;
                total_rtt += rtt;
                if (rtt < min_rtt) min_rtt = rtt;
                if (rtt > max_rtt) max_rtt = rtt;

                terminal_writestring("reply from ");
                term_write_ipv4(ip);
                terminal_writestring(": seq=");
                term_write_u32(i + 1u);
                terminal_writestring(" time=");
                term_write_u32(rtt);
                terminal_writestring("ms\n");
                if (until_ok) break;
            } else if (rc == NET_PING_ERR_ARP_UNRESOLVED) {
                terminal_writestring("no arp reply: seq=");
                term_write_u32(i + 1u);
                terminal_writestring("\n");
            } else if (rc == NET_PING_ERR_DEST_UNREACH) {
                terminal_writestring("dest unreachable: seq=");
                term_write_u32(i + 1u);
                terminal_writestring("\n");
            } else if (rc == NET_PING_ERR_TIME_EXCEEDED) {
                terminal_writestring("time exceeded: seq=");
                term_write_u32(i + 1u);
                terminal_writestring("\n");
            } else if (rc == NET_PING_ERR_TX) {
                terminal_writestring("tx failed: seq=");
                term_write_u32(i + 1u);
                terminal_writestring("\n");
            } else {
                terminal_writestring("timeout: seq=");
                term_write_u32(i + 1u);
                terminal_writestring("\n");
            }
        }

        terminal_writestring("ping stats: sent=");
        term_write_u32(sent);
        terminal_writestring(" recv=");
        term_write_u32(recv);
        terminal_writestring(" loss=");
        term_write_u32(sent > recv ? (uint32_t)(((sent - recv) * 100u) / sent) : 0u);
        terminal_writestring("%\n");

        if (recv > 0u) {
            terminal_writestring("rtt min/avg/max=");
            term_write_u32(min_rtt);
            terminal_writestring("/");
            term_write_u32((uint32_t)(total_rtt / recv));
            terminal_writestring("/");
            term_write_u32(max_rtt);
            terminal_writestring(" ms\n");
        }

        return;
    }

    if (args && strncmp(args, "udp ", 4) == 0) {
        const char* mode = next_token(args);
        char mode_buf[16];

        if (!mode) {
            terminal_writestring("usage: net udp self <port> <token>\n");
            return;
        }

        copy_token(mode, mode_buf, sizeof(mode_buf));
        if (strcmp(mode_buf, "self") != 0) {
            terminal_writestring("usage: net udp self <port> <token>\n");
            return;
        }

        {
            const char* port_tok = next_token(mode);
            const char* msg_tok;
            char msg_buf[96];
            uint16_t rx_len = 0;
            uint8_t rx_buf[128];
            net_udp_endpoint_t from;
            uint8_t local_ip[4];
            int sock;
            int bind_rc;
            int tx_rc;
            int rx_rc;
            uint32_t port;
            char num[24];

            if (!port_tok || !is_decimal_token(port_tok)) {
                terminal_writestring("usage: net udp self <port> <token>\n");
                return;
            }
            msg_tok = next_token(port_tok);
            if (!msg_tok || *msg_tok == '\0') {
                terminal_writestring("usage: net udp self <port> <token>\n");
                return;
            }

            port = parse_uint(port_tok);
            if (port == 0u || port > 65535u) {
                terminal_writestring("net udp: invalid port\n");
                return;
            }
            copy_token(msg_tok, msg_buf, sizeof(msg_buf));

            sock = net_udp_socket_open();
            if (sock < 0) {
                terminal_writestring("net udp: socket open failed\n");
                return;
            }

            bind_rc = net_udp_socket_bind(sock, (uint16_t)port);
            if (bind_rc < 0) {
                terminal_writestring("net udp: bind failed rc=");
                itoa(bind_rc, num, 10);
                terminal_writestring(num);
                terminal_writestring("\n");
                (void)net_udp_socket_close(sock);
                return;
            }

            net_get_ipv4(local_ip);
            tx_rc = net_udp_socket_sendto(sock, local_ip, (uint16_t)port, msg_buf, (uint16_t)strlen(msg_buf));
            if (tx_rc != NET_UDP_OK) {
                terminal_writestring("net udp: send failed rc=");
                itoa(tx_rc, num, 10);
                terminal_writestring(num);
                terminal_writestring("\n");
                (void)net_udp_socket_close(sock);
                return;
            }

            rx_rc = net_udp_socket_recvfrom(sock, rx_buf, sizeof(rx_buf) - 1u, &rx_len, &from, 1000u);
            if (rx_rc < 0 && rx_rc != NET_UDP_ERR_MSG_TRUNC) {
                terminal_writestring("net udp: recv failed rc=");
                itoa(rx_rc, num, 10);
                terminal_writestring(num);
                terminal_writestring("\n");
                (void)net_udp_socket_close(sock);
                return;
            }

            rx_buf[rx_len] = '\0';
            terminal_writestring("net udp self ok: from ");
            term_write_ipv4(from.ip);
            terminal_writestring(":");
            term_write_u32((uint32_t)from.port);
            terminal_writestring(" payload='");
            terminal_writestring((const char*)rx_buf);
            terminal_writestring("'\n");
            (void)net_udp_socket_close(sock);
            return;
        }
    }

    if (args && strncmp(args, "tcp ", 4) == 0) {
        const char* mode = next_token(args);
        char mode_buf[16];

        if (!mode) {
            terminal_writestring("usage: net tcp connect <host|a.b.c.d> <port> [-W timeout_ms] | net tcp http <host|a.b.c.d> <port> <path> [-W timeout_ms]\n");
            return;
        }

        copy_token(mode, mode_buf, sizeof(mode_buf));
        if (strcmp(mode_buf, "stats") == 0) {
            net_tcp_debug_stats_t st;
            net_tcp_get_debug_stats(&st);
            terminal_writestring("tcp: syn_sent=");
            term_write_u32(st.syn_sent);
            terminal_writestring(" syn_retx=");
            term_write_u32(st.syn_retx);
            terminal_writestring(" synack=");
            term_write_u32(st.synack_seen);
            terminal_writestring(" rst=");
            term_write_u32(st.rst_seen);
            terminal_writestring(" csum_drop=");
            term_write_u32(st.checksum_drop);
            terminal_writestring(" tuple_miss=");
            term_write_u32(st.tuple_miss);
            terminal_writestring(" ok=");
            term_write_u32(st.connect_ok);
            terminal_writestring(" timeout=");
            term_write_u32(st.connect_timeout);
            terminal_writestring("\n");
            if (st.tuple_miss > 0u) {
                terminal_writestring("tcp: last_miss src=");
                term_write_ipv4(st.last_miss_src_ip);
                terminal_writestring(":");
                term_write_u32(st.last_miss_src_port);
                terminal_writestring(" dport=");
                term_write_u32(st.last_miss_dst_port);
                terminal_writestring(" flags=0x");
                {
                    char hx[8];
                    uint8_t f = st.last_miss_flags;
                    hx[0] = "0123456789abcdef"[(f >> 4) & 0xFu];
                    hx[1] = "0123456789abcdef"[f & 0xFu];
                    hx[2] = '\0';
                    terminal_writestring(hx);
                }
                terminal_writestring(" seq=");
                term_write_u32(st.last_miss_seq);
                terminal_writestring(" ack=");
                term_write_u32(st.last_miss_ack);
                terminal_writestring("\n");
                terminal_writestring("tcp: last_miss arrival_ms=");
                term_write_u32(st.last_miss_arrival_ms);
                terminal_writestring(" syn_sent_ms=");
                term_write_u32(st.last_syn_sent_ms);
                terminal_writestring(" delta_ms=");
                term_write_u32(st.last_miss_arrival_ms - st.last_syn_sent_ms);
                terminal_writestring("\n");
            }
            return;
        }

        if (strcmp(mode_buf, "http") == 0) {
            const char* host_tok = next_token(mode);
            const char* port_tok;
            const char* path_tok;
            const char* cursor;
            char host[128];
            char path[160];
            char req[320];
            char opt[16];
            char num[24];
            uint8_t ip[4];
            uint8_t rx[256];
            uint16_t rx_len;
            uint32_t timeout_ms = 5000u;
            uint32_t total = 0u;
            uint32_t port;
            int sock = -1;
            int rc;

            if (!host_tok) {
                terminal_writestring("usage: net tcp http <host|a.b.c.d> <port> <path> [-W timeout_ms]\n");
                return;
            }
            copy_token(host_tok, host, sizeof(host));

            port_tok = next_token(host_tok);
            path_tok = next_token(port_tok);
            if (!port_tok || !is_decimal_token(port_tok) || !path_tok || *path_tok == '\0') {
                terminal_writestring("usage: net tcp http <host|a.b.c.d> <port> <path> [-W timeout_ms]\n");
                return;
            }

            port = parse_uint(port_tok);
            if (port == 0u || port > 65535u) {
                terminal_writestring("net tcp: invalid port\n");
                return;
            }
            copy_token(path_tok, path, sizeof(path));

            cursor = next_token(path_tok);
            while (cursor && *cursor) {
                copy_token(cursor, opt, sizeof(opt));
                if (strcmp(opt, "-W") == 0) {
                    cursor = next_token(cursor);
                    if (!cursor || !*cursor || !is_decimal_token(cursor)) {
                        terminal_writestring("usage: net tcp http <host|a.b.c.d> <port> <path> [-W timeout_ms]\n");
                        return;
                    }
                    timeout_ms = parse_uint(cursor);
                    cursor = next_token(cursor);
                    continue;
                }
                terminal_writestring("usage: net tcp http <host|a.b.c.d> <port> <path> [-W timeout_ms]\n");
                return;
            }

            if (parse_ipv4_token(host_tok, ip) != 0) {
                int dns_rc = net_dns_query_a(host, NULL, ip, timeout_ms);
                if (dns_rc != NET_DNS_OK) {
                    terminal_writestring("net tcp http: dns lookup failed rc=");
                    itoa(dns_rc, num, 10);
                    terminal_writestring(num);
                    terminal_writestring("\n");
                    return;
                }
            }

            rc = net_tcp_client_connect(ip, (uint16_t)port, timeout_ms, &sock);
            if (rc != NET_TCP_OK) {
                terminal_writestring("net tcp http: connect failed rc=");
                itoa(rc, num, 10);
                terminal_writestring(num);
                terminal_writestring("\n");
                return;
            }

            if (path[0] != '/') {
                char tmp[sizeof(path)];
                strcpy(tmp, path);
                path[0] = '/';
                path[1] = '\0';
                strncat(path, tmp, sizeof(path) - 2u);
            }
            req[0] = '\0';
            strncat(req, "GET ", sizeof(req) - strlen(req) - 1u);
            strncat(req, path, sizeof(req) - strlen(req) - 1u);
            strncat(req, " HTTP/1.0\r\nHost: ", sizeof(req) - strlen(req) - 1u);
            strncat(req, host, sizeof(req) - strlen(req) - 1u);
            strncat(req, "\r\nConnection: close\r\n\r\n", sizeof(req) - strlen(req) - 1u);

            rc = net_tcp_client_send(sock, req, (uint16_t)strlen(req), timeout_ms);
            if (rc != NET_TCP_OK) {
                terminal_writestring("net tcp http: send failed rc=");
                itoa(rc, num, 10);
                terminal_writestring(num);
                terminal_writestring("\n");
                (void)net_tcp_client_close(sock, 1000u);
                return;
            }

            terminal_writestring("net tcp http: response begin\n");
            for (;;) {
                rc = net_tcp_client_recv(sock, rx, sizeof(rx), &rx_len, timeout_ms);
                if (rc == NET_TCP_OK) {
                    if (rx_len > 0u) {
                        for (uint16_t i = 0; i < rx_len; ++i) terminal_putchar((char)rx[i]);
                        total += rx_len;
                    }
                    continue;
                }
                if (rc == NET_TCP_ERR_CLOSED) break;
                if (rc == NET_TCP_ERR_WOULD_BLOCK) break;
                terminal_writestring("\nnet tcp http: recv failed rc=");
                itoa(rc, num, 10);
                terminal_writestring(num);
                terminal_writestring("\n");
                break;
            }

            (void)net_tcp_client_close(sock, timeout_ms);
            terminal_writestring("\nnet tcp http: done bytes=");
            term_write_u32(total);
            terminal_writestring("\n");
            return;
        }

        if (strcmp(mode_buf, "connect") != 0) {
            terminal_writestring("usage: net tcp connect <host|a.b.c.d> <port> [-W timeout_ms] | net tcp http <host|a.b.c.d> <port> <path> [-W timeout_ms] | net tcp stats\n");
            return;
        }

        {
            const char* host_tok = next_token(mode);
            const char* port_tok;
            const char* cursor;
            char host[128];
            char opt[16];
            uint8_t ip[4];
            uint32_t timeout_ms = 5000u;
            uint32_t port;
            int rc;
            char num[24];

            if (!host_tok) {
                terminal_writestring("usage: net tcp connect <host|a.b.c.d> <port> [-W timeout_ms]\n");
                return;
            }
            copy_token(host_tok, host, sizeof(host));

            port_tok = next_token(host_tok);
            if (!port_tok || !is_decimal_token(port_tok)) {
                terminal_writestring("usage: net tcp connect <host|a.b.c.d> <port> [-W timeout_ms]\n");
                return;
            }

            port = parse_uint(port_tok);
            if (port == 0u || port > 65535u) {
                terminal_writestring("net tcp: invalid port\n");
                return;
            }

            cursor = next_token(port_tok);
            while (cursor && *cursor) {
                copy_token(cursor, opt, sizeof(opt));
                if (strcmp(opt, "-W") == 0) {
                    cursor = next_token(cursor);
                    if (!cursor || !*cursor || !is_decimal_token(cursor)) {
                        terminal_writestring("usage: net tcp connect <host|a.b.c.d> <port> [-W timeout_ms]\n");
                        return;
                    }
                    timeout_ms = parse_uint(cursor);
                    cursor = next_token(cursor);
                    continue;
                }

                terminal_writestring("usage: net tcp connect <host|a.b.c.d> <port> [-W timeout_ms]\n");
                return;
            }

            {
                int host_is_name = (parse_ipv4_token(host_tok, ip) != 0);
                uint32_t attempts = host_is_name ? 4u : 1u;

                rc = NET_TCP_ERR_TIMEOUT;
                for (uint32_t a = 0; a < attempts; ++a) {
                    if (host_is_name) {
                        int dns_rc = net_dns_query_a(host, NULL, ip, timeout_ms);
                        if (dns_rc != NET_DNS_OK) {
                            rc = dns_rc;
                            break;
                        }
                    }

                    terminal_writestring("TCP connect ");
                    terminal_writestring(host);
                    terminal_writestring(" (");
                    term_write_ipv4(ip);
                    terminal_writestring("):");
                    term_write_u32(port);
                    if (host_is_name) {
                        terminal_writestring(" try=");
                        term_write_u32(a + 1u);
                    }
                    terminal_writestring(" ... ");

                    {
                        int tcp_sock = -1;
                        rc = net_tcp_client_connect(ip, (uint16_t)port, timeout_ms == 0u ? 1u : timeout_ms, &tcp_sock);
                        if (rc == NET_TCP_OK && tcp_sock >= 0) {
                            (void)net_tcp_client_close(tcp_sock, 1000u);
                        }
                    }
                    if (rc == NET_TCP_OK) {
                        terminal_writestring("ok\n");
                        return;
                    }

                    terminal_writestring("failed rc=");
                    itoa(rc, num, 10);
                    terminal_writestring(num);
                    terminal_writestring("\n");

                    if (!host_is_name) break;
                }

                if (host_is_name && (rc == NET_DNS_ERR_TIMEOUT || rc == NET_DNS_ERR_FORMAT || rc == NET_DNS_ERR_NOT_FOUND)) {
                    terminal_writestring("net tcp: dns lookup failed host=");
                    terminal_writestring(host);
                    terminal_writestring(" rc=");
                    itoa(rc, num, 10);
                    terminal_writestring(num);
                    terminal_writestring("\n");
                    return;
                }
            }
            return;
        }
    }

    if (args && strncmp(args, "dns ", 4) == 0) {
        const char* host_tok = next_token(args);
        const char* dns_tok;
        char host[128];
        uint8_t dns_server[4];
        uint8_t resolved_ip[4];
        int explicit_dns = 0;
        int rc;
        char num[24];

        if (!host_tok) {
            terminal_writestring("usage: net dns <hostname> [dns_server_ip]\n");
            return;
        }

        if (strcmp(host_tok, "cache") == 0) {
            net_dns_cache_debug_entry_t entries[16];
            uint32_t n = net_dns_cache_dump(entries, 16u);
            terminal_writestring("dns cache entries=");
            term_write_u32(n);
            terminal_writestring("\n");
            for (uint32_t i = 0; i < n; ++i) {
                terminal_writestring("  ");
                terminal_writestring(entries[i].name);
                terminal_writestring(" -> ");
                if (entries[i].negative) {
                    terminal_writestring("<NEG>");
                } else {
                    term_write_ipv4(entries[i].ip);
                }
                terminal_writestring(" ttl=");
                term_write_u32(entries[i].ttl_left_ms);
                terminal_writestring("ms age=");
                term_write_u32(entries[i].age_ms);
                terminal_writestring("ms lru=");
                term_write_u32(entries[i].lru_rank);
                terminal_writestring("\n");
            }
            return;
        }
        copy_token(host_tok, host, sizeof(host));

        dns_tok = next_token(host_tok);
        if (dns_tok && *dns_tok) {
            if (parse_ipv4_token(dns_tok, dns_server) != 0) {
                terminal_writestring("usage: net dns <hostname> [dns_server_ip]\n");
                return;
            }
            explicit_dns = 1;
            rc = net_dns_query_a(host, dns_server, resolved_ip, 2000u);
        } else {
            rc = net_dns_query_a(host, NULL, resolved_ip, 2000u);
        }

        if (rc == NET_DNS_OK) {
            terminal_writestring("dns: ");
            terminal_writestring(host);
            terminal_writestring(" -> ");
            term_write_ipv4(resolved_ip);
            terminal_writestring("\n");
        } else {
            terminal_writestring("dns: lookup failed rc=");
            itoa(rc, num, 10);
            terminal_writestring(num);
            if (rc == NET_DNS_ERR_TIMEOUT) {
                terminal_writestring(" server=");
                if (explicit_dns) {
                    term_write_ipv4(dns_server);
                } else {
                    terminal_writestring("auto(10.0.2.3,10.0.2.2)");
                }
            }
            terminal_writestring("\n");
        }
        return;
    }

    if (!args || *args == '\0' || strcmp(args, "stats") == 0) {
        if (!net_is_ready()) {
            terminal_writestring("net: no initialized NIC driver\n");
            return;
        }

        uint8_t mac[6];
        net_stats_t st;
        char buf[24];

        net_get_mac(mac);
        net_get_stats(&st);

        terminal_writestring("net: driver=");
        terminal_writestring(net_driver_name());
        terminal_writestring(" state=");
        terminal_writestring(net_link_up() ? "link-up\n" : "link-down\n");

        terminal_writestring("mac: ");
        term_write_hex8(mac[0]); terminal_writestring(":");
        term_write_hex8(mac[1]); terminal_writestring(":");
        term_write_hex8(mac[2]); terminal_writestring(":");
        term_write_hex8(mac[3]); terminal_writestring(":");
        term_write_hex8(mac[4]); terminal_writestring(":");
        term_write_hex8(mac[5]); terminal_writestring("\n");

        terminal_writestring("irqs=");
        itoa((int)st.interrupts, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" rx=");
        itoa((int)st.rx_packets, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" tx=");
        itoa((int)st.tx_packets, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" rxirq=");
        itoa((int)st.rx_irqs, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" txirq=");
        itoa((int)st.tx_irqs, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" drops=");
        itoa((int)st.rx_drops, buf, 10);
        terminal_writestring(buf);
        terminal_writestring("\n");
        return;
    }

    if (strcmp(args, "tx") == 0) {
        if (!net_is_ready()) {
            terminal_writestring("net: no initialized NIC driver\n");
            return;
        }

        int rc = net_send_test_frame();
        if (rc == 0) {
            terminal_writestring("net: queued one test Ethernet frame\n");
        } else {
            terminal_writestring("net: tx failed\n");
        }
        return;
    }

    terminal_writestring("usage: net [stats] | net netstat | net rxdefer | net timers | net p2 | net test regress | net test fuzz [count] [seed] | net test stress [count] [seed] | net dhcp [timeout_ms] | net tx | net regs | net ip [a.b.c.d] | net mask [a.b.c.d] | net gw [a.b.c.d] | net arp | net arping <a.b.c.d> | net ping <host|a.b.c.d> [count] [-c count] [-W timeout_ms] [-U] | net udp self <port> <token> | net tcp connect <host|a.b.c.d> <port> [-W timeout_ms] | net tcp http <host|a.b.c.d> <port> <path> [-W timeout_ms] | net tcp stats | net dns <hostname> [dns_server_ip] | net dns cache\n");
}

/* ---- Filesystem commands ----------------------------------------------- */

/*
 * Resolve a path for shell commands.
 * Absolute paths ("/foo/bar") are used as-is.
 * Relative paths are joined with the current working directory.
 */
static const char* resolve_shell_path(const char* arg, char* buf, size_t bufsz) {
    if (!arg || *arg == '\0') return cwd;
    if (vfs_normalize_path(cwd, arg, buf, bufsz) < 0) return cwd;
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
static void ls_recursive(vfs_node_t* dir, const char* dir_path, int depth) {
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

            char child_path[VFS_PATH_MAX];
            if (strcmp(dir_path, "/") == 0) {
                strcpy(child_path, "/");
                strncat(child_path, ent.name, sizeof(child_path) - strlen(child_path) - 1);
            } else {
                strncpy(child_path, dir_path, sizeof(child_path) - 1);
                child_path[sizeof(child_path) - 1] = '\0';
                strncat(child_path, "/", sizeof(child_path) - strlen(child_path) - 1);
                strncat(child_path, ent.name, sizeof(child_path) - strlen(child_path) - 1);
            }

            /* Use path resolution so mount points traverse into mounted roots. */
            vfs_node_t* sub = vfs_namei(child_path);
            if (sub && (sub->type & VFS_DIRECTORY)) {
                ls_recursive(sub, child_path, depth + 1);
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

    ls_recursive(dir, path, 0);
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

    if (vfs_create_path(path, VFS_FILE) < 0) {
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

    vfs_node_t* existing = vfs_namei(path);
    if (existing && (existing->type & VFS_DIRECTORY)) {
        terminal_writestring("write: target is a directory: ");
        terminal_writestring(path);
        terminal_writestring("\n");
        terminal_writestring("hint: use a file path, e.g. /mnt/hd0p1/test.txt\n");
        return;
    }

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

    if (written < 0) {
        terminal_writestring("write: write failed: ");
        terminal_writestring(path);
        terminal_writestring("\n");
        return;
    }

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

    if (vfs_create_path(path, VFS_DIRECTORY) < 0) {
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

    if (strcmp(path, "/") == 0) {
        terminal_writestring("rm: cannot remove root\n");
        return;
    }

    if (vfs_unlink_path(path) < 0) {
        if (node->type & VFS_DIRECTORY) {
            terminal_writestring("rm: failed (directory not empty?)\n");
        } else {
            terminal_writestring("rm: failed\n");
        }
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
    terminal_writestring(" bytes\n  mode:  0");
    itoa((int)st.mode, buf, 8);
    terminal_writestring(buf);
    terminal_writestring("\n  uid:   ");
    itoa((int)st.uid, buf, 10);
    terminal_writestring(buf);
    terminal_writestring("\n  gid:   ");
    itoa((int)st.gid, buf, 10);
    terminal_writestring(buf);
    terminal_writestring("\n");
}

static void cmd_id(const char* args) {
    (void)args;
    char buf[16];
    terminal_writestring("uid=");
    itoa(getuid(), buf, 10);
    terminal_writestring(buf);
    terminal_writestring(" gid=");
    itoa(getgid(), buf, 10);
    terminal_writestring(buf);
    terminal_writestring("\n");
}

static void cmd_chmod(const char* args) {
    if (!args || *args == '\0') {
        terminal_writestring("usage: chmod <octal-mode> <path>\n");
        return;
    }

    char mode_tok[8];
    copy_token(args, mode_tok, sizeof(mode_tok));
    const char* p = next_token(args);
    if (!p || *p == '\0') {
        terminal_writestring("usage: chmod <octal-mode> <path>\n");
        return;
    }

    uint16_t mode = 0;
    if (parse_mode_octal(mode_tok, &mode) < 0) {
        terminal_writestring("chmod: invalid mode (use octal like 644 or 755)\n");
        return;
    }

    char pathbuf[VFS_PATH_MAX];
    const char* path = resolve_shell_path(p, pathbuf, sizeof(pathbuf));
    if (chmod(path, mode) < 0) {
        terminal_writestring("chmod: failed\n");
    }
}

static void cmd_chown(const char* args) {
    if (!args || *args == '\0') {
        terminal_writestring("usage: chown <uid> <gid> <path>\n");
        return;
    }

    char uid_tok[16];
    copy_token(args, uid_tok, sizeof(uid_tok));
    const char* p = next_token(args);
    if (!p || *p == '\0') {
        terminal_writestring("usage: chown <uid> <gid> <path>\n");
        return;
    }

    char gid_tok[16];
    copy_token(p, gid_tok, sizeof(gid_tok));
    p = next_token(p);
    if (!p || *p == '\0') {
        terminal_writestring("usage: chown <uid> <gid> <path>\n");
        return;
    }

    if (!is_decimal_token(uid_tok) || !is_decimal_token(gid_tok)) {
        terminal_writestring("chown: uid/gid must be decimal integers\n");
        return;
    }

    uint32_t uid = parse_uint(uid_tok);
    uint32_t gid = parse_uint(gid_tok);

    char pathbuf[VFS_PATH_MAX];
    const char* path = resolve_shell_path(p, pathbuf, sizeof(pathbuf));
    if (chown(path, uid, gid) < 0) {
        terminal_writestring("chown: failed\n");
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

    char resolved[VFS_PATH_MAX];
    if (vfs_normalize_path(cwd, path, resolved, sizeof(resolved)) < 0) {
        terminal_writestring("cd: invalid path\n");
        return;
    }

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

/*
 * VFS test suite: validates path normalization, mount points, and permissions
 */
static void cmd_vfstest(const char* args) {
    if (!args || *args == '\0') {
        terminal_writestring("usage: vfstest norm | vfstest mount | vfstest perm\n");
        return;
    }

    /* Extract subcommand */
    char subcmd[32];
    size_t i = 0;
    while (args[i] && args[i] != ' ' && i < sizeof(subcmd) - 1) {
        subcmd[i] = args[i];
        i++;
    }
    subcmd[i] = '\0';

    if (strcmp(subcmd, "norm") == 0) {
        /* Test 1: Normalize paths with //, ., .. */
        terminal_writestring("\n=== PATH NORMALIZATION TEST ===\n");

        struct {
            const char* input;
            const char* expected;
        } tests[] = {
            {"/a/b/c", "/a/b/c"},
            {"/a//b", "/a/b"},
            {"/a///b", "/a/b"},
            {"/./a/b", "/a/b"},
            {"/a/./b", "/a/b"},
            {"/a/b/.", "/a/b"},
            {"/a/../b", "/b"},
            {"/a/b/../c", "/a/c"},
            {"/a/./b/../c", "/a/c"},
            {"//a//b//", "/a/b"},
            {"/", "/"},
        };

        char normalized[VFS_PATH_MAX];
        int pass = 0, fail = 0;

        for (size_t t = 0; t < sizeof(tests) / sizeof(tests[0]); t++) {
            if (vfs_normalize_path("/", tests[t].input, normalized, sizeof(normalized)) == 0) {
                if (strcmp(normalized, tests[t].expected) == 0) {
                    terminal_writestring("[OK] ");
                    terminal_writestring(tests[t].input);
                    terminal_writestring(" -> ");
                    terminal_writestring(normalized);
                    terminal_writestring("\n");
                    pass++;
                } else {
                    terminal_writestring("[FAIL] ");
                    terminal_writestring(tests[t].input);
                    terminal_writestring(" got ");
                    terminal_writestring(normalized);
                    terminal_writestring(" expected ");
                    terminal_writestring(tests[t].expected);
                    terminal_writestring("\n");
                    fail++;
                }
            } else {
                terminal_writestring("[ERROR] Could not normalize ");
                terminal_writestring(tests[t].input);
                terminal_writestring("\n");
                fail++;
            }
        }

        terminal_writestring("\nPath normalization: ");
        char buf[16];
        itoa(pass, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" passed, ");
        itoa(fail, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" failed\n");
        return;
    }

    if (strcmp(subcmd, "mount") == 0) {
        /* Test 2: Mount point resolution */
        terminal_writestring("\n=== MOUNT POINT RESOLUTION TEST ===\n");

        /* Create test directory structure */
        terminal_writestring("Setting up test dirs...\n");
        if (vfs_create_path("/vfstest", VFS_DIRECTORY) < 0) {
            terminal_writestring("mkdir /vfstest failed (may already exist)\n");
        }
        if (vfs_create_path("/vfstest/file1", VFS_FILE) < 0) {
            terminal_writestring("touch /vfstest/file1 failed\n");
        }

        vfs_node_t* vfstest = vfs_namei("/vfstest");
        if (vfstest) {
            terminal_writestring("[OK] /vfstest resolved to ");
            char buf[16];
            itoa((int)vfstest->inode, buf, 10);
            terminal_writestring(buf);
            terminal_writestring("\n");
        } else {
            terminal_writestring("[FAIL] /vfstest not found\n");
        }

        vfs_node_t* file1 = vfs_namei("/vfstest/file1");
        if (file1) {
            terminal_writestring("[OK] /vfstest/file1 resolved\n");
        } else {
            terminal_writestring("[FAIL] /vfstest/file1 not found\n");
        }

        /* Test relative path with cwd */
        char old_cwd[VFS_PATH_MAX];
        strcpy(old_cwd, cwd);
        strcpy(cwd, "/vfstest");

        char normalized[VFS_PATH_MAX];
        if (vfs_normalize_path(cwd, "file1", normalized, sizeof(normalized)) == 0) {
            terminal_writestring("[OK] Relative path 'file1' normalized to ");
            terminal_writestring(normalized);
            terminal_writestring("\n");
        } else {
            terminal_writestring("[FAIL] Could not resolve relative path\n");
        }

        strcpy(cwd, old_cwd);
        terminal_writestring("Mount resolution tests completed\n");
        return;
    }

    if (strcmp(subcmd, "perm") == 0) {
        /* Test 3: Permission checks */
        terminal_writestring("\n=== PERMISSION CHECK TEST ===\n");

        vfs_node_t* root = vfs_get_root();
        if (!root) {
            terminal_writestring("[ERROR] No root filesystem\n");
            return;
        }

        terminal_writestring("Root inode: ");
        char buf[16];
        itoa((int)root->inode, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(", type: ");
        terminal_writestring((root->type & VFS_DIRECTORY) ? "DIR" : "FILE");
        terminal_writestring(", mode: 0");
        itoa((int)root->mode, buf, 8);
        terminal_writestring(buf);
        terminal_writestring("\n");

        /* Test permission bits */
        int can_read = vfs_node_allows(root, VFS_MODE_IROTH);
        int can_write = vfs_node_allows(root, VFS_MODE_IWOTH);
        int can_exec = vfs_node_allows(root, VFS_MODE_IXOTH);

        terminal_writestring("Permissions: read=");
        terminal_writestring(can_read ? "yes" : "no");
        terminal_writestring(" write=");
        terminal_writestring(can_write ? "yes" : "no");
        terminal_writestring(" exec=");
        terminal_writestring(can_exec ? "yes" : "no");
        terminal_writestring("\n");

        /* Create a test file and verify permissions */
        if (vfs_create_path("/perm_test", VFS_FILE) < 0) {
            terminal_writestring("[WARN] Could not create /perm_test (may already exist)\n");
        } else {
            terminal_writestring("[OK] Created /perm_test\n");
        }

        vfs_node_t* pfile = vfs_namei("/perm_test");
        if (pfile) {
            terminal_writestring("[OK] /perm_test has mode 0");
            itoa((int)pfile->mode, buf, 8);
            terminal_writestring(buf);
            terminal_writestring(" (should be writable by default)\n");
        }

        terminal_writestring("Permission tests completed\n");
        return;
    }

    terminal_writestring("Unknown vfstest subcommand: ");
    terminal_writestring(subcmd);
    terminal_writestring("\n");
}

static void cmd_exec(const char* args) {
    shell_jobctl_ensure();

    if (!args || *args == '\0') {
        terminal_writestring("usage: exec <path> [&]\n");
        return;
    }

    uint32_t wait = 1;
    char argbuf[VFS_PATH_MAX];
    strncpy(argbuf, args, sizeof(argbuf) - 1);
    argbuf[sizeof(argbuf) - 1] = '\0';

    size_t n = strlen(argbuf);
    while (n > 0 && argbuf[n - 1] == ' ') {
        argbuf[n - 1] = '\0';
        n--;
    }
    if (n > 0 && argbuf[n - 1] == '&') {
        wait = 0;
        argbuf[n - 1] = '\0';
        while (n > 1 && argbuf[n - 2] == ' ') {
            argbuf[n - 2] = '\0';
            n--;
        }
    }

    char pathbuf[VFS_PATH_MAX];
    const char* path = resolve_shell_path(argbuf, pathbuf, sizeof(pathbuf));

    terminal_writestring("Loading ELF: ");
    terminal_writestring(path);
    terminal_writestring("\n");

    int ret = elf_exec(path, 0);
    if (ret < 0) {
        terminal_writestring("exec: failed (error ");
        char buf[16];
        itoa(ret, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(")\n");
        return;
    }

    (void)task_setpgid(ret, ret);

    if (!wait) {
        shell_jobs_add(ret, ret, path);
        terminal_writestring("exec: started pid ");
        char buf[16];
        itoa(ret, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" pgid ");
        itoa(ret, buf, 10);
        terminal_writestring(buf);
        terminal_writestring("\n");
        return;
    }

    (void)terminal_set_foreground_pgid(ret);
    int status = 0;
    (void)task_waitpid(ret, &status, 0);
    if (shell_jobctl_ready && shell_pgid > 0) {
        (void)terminal_set_foreground_pgid(shell_pgid);
    }
}

static void cmd_jobs(const char* args) {
    (void)args;
    shell_jobs_reap();

    terminal_writestring("PID  PGID  CMD\n");
    terminal_writestring("---  ----  ---\n");
    for (int i = 0; i < SHELL_BG_JOBS_MAX; ++i) {
        if (!shell_bg_jobs[i].in_use) continue;

        char buf[16];
        itoa(shell_bg_jobs[i].pid, buf, 10);
        terminal_writestring(buf);
        terminal_writestring("   ");
        itoa(shell_bg_jobs[i].pgid, buf, 10);
        terminal_writestring(buf);
        terminal_writestring("    ");
        terminal_writestring(shell_bg_jobs[i].cmd[0] ? shell_bg_jobs[i].cmd : "(unknown)");
        terminal_writestring("\n");
    }
}

static void cmd_fg(const char* args) {
    shell_jobs_reap();
    if (!args || *args == '\0') {
        terminal_writestring("usage: fg <pid>\n");
        return;
    }

    int pid = (int)parse_uint(args);
    int idx = shell_jobs_find_by_pid(pid);
    if (idx < 0) {
        terminal_writestring("fg: job not found\n");
        return;
    }

    (void)terminal_set_foreground_pgid(shell_bg_jobs[idx].pgid);
    int status = 0;
    (void)task_waitpid(shell_bg_jobs[idx].pid, &status, 0);
    shell_jobs_remove_index(idx);

    if (shell_jobctl_ready && shell_pgid > 0) {
        (void)terminal_set_foreground_pgid(shell_pgid);
    }
}

static void cmd_bg(const char* args) {
    shell_jobs_reap();
    if (!args || *args == '\0') {
        terminal_writestring("usage: bg <pid>\n");
        return;
    }

    int pid = (int)parse_uint(args);
    int idx = shell_jobs_find_by_pid(pid);
    if (idx < 0) {
        terminal_writestring("bg: job not found\n");
        return;
    }

    terminal_writestring("bg: job running in background: ");
    char buf[16];
    itoa(shell_bg_jobs[idx].pid, buf, 10);
    terminal_writestring(buf);
    terminal_writestring("\n");
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

