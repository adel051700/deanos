/* /bin/sh — phase-1 minimal userspace REPL (see
 * docs/superpowers/specs/2026-07-10-userspace-shell-design.md). */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#define SH_LINE_MAX 256
#define SH_PATH_MAX 256
#define SH_ARGV_MAX 16 /* kernel ELF_ARGV_MAX, argv[0] included */

static void print_prompt(void) {
    char cwd[SH_PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        cwd[0] = '/';
        cwd[1] = '\0';
    }
    printf("DeanOS %s $ ", cwd);
}

/* ---- line editor (ported from the retired kernel shell) ------------- */

static char   ed_buf[SH_LINE_MAX];
static size_t ed_len;
static size_t ed_cur;

/* Non-destructive cursor moves via the tty's ESC[C / ESC[D. */
static void term_left(size_t n)  { for (size_t i = 0; i < n; i++) write(1, "\x1b[D", 3); }
static void term_right(size_t n) { for (size_t i = 0; i < n; i++) write(1, "\x1b[C", 3); }

static void insert_char_at_cursor(char c) {
    if (ed_len >= SH_LINE_MAX - 1) return; /* silently drop past the cap */
    memmove(&ed_buf[ed_cur + 1], &ed_buf[ed_cur], ed_len - ed_cur);
    ed_buf[ed_cur] = c;
    ed_len++;
    ed_buf[ed_len] = '\0';
    write(1, &ed_buf[ed_cur], ed_len - ed_cur); /* char + shifted tail */
    term_left(ed_len - ed_cur - 1);
    ed_cur++;
}

static void do_backspace(void) {
    if (ed_cur == 0) return;
    size_t chars_after = ed_len - ed_cur;
    term_left(1);
    memmove(&ed_buf[ed_cur - 1], &ed_buf[ed_cur], chars_after);
    ed_cur--;
    ed_len--;
    ed_buf[ed_len] = '\0';
    write(1, &ed_buf[ed_cur], chars_after); /* redraw tail */
    write(1, " ", 1);                       /* erase the stale last cell */
    term_left(chars_after + 1);
}

static void do_delete(void) {
    if (ed_cur >= ed_len) return;
    size_t chars_after = ed_len - ed_cur - 1;
    memmove(&ed_buf[ed_cur], &ed_buf[ed_cur + 1], chars_after);
    ed_len--;
    ed_buf[ed_len] = '\0';
    write(1, &ed_buf[ed_cur], chars_after);
    write(1, " ", 1);
    term_left(chars_after + 1);
}

static void autocomplete(void) {
    /* Filled in by the tab-completion task; Tab is a no-op until then. */
}

/* ---- history (old kernel shell semantics, in-memory only) ------------ */

#define SH_HISTORY_SIZE 32
static char history[SH_HISTORY_SIZE][SH_LINE_MAX];
static int  hist_len;
static int  hist_pos; /* == hist_len when not browsing */
static char edit_backup[SH_LINE_MAX];

static void history_add(const char* cmd) {
    if (!cmd || !*cmd) { hist_pos = hist_len; return; }
    if (hist_len > 0 && strcmp(history[hist_len - 1], cmd) == 0) {
        hist_pos = hist_len; /* consecutive duplicate: skip */
        return;
    }
    if (hist_len < SH_HISTORY_SIZE) {
        strcpy(history[hist_len], cmd); /* cmd is < SH_LINE_MAX by construction */
        hist_len++;
    } else {
        for (int i = 1; i < SH_HISTORY_SIZE; i++)
            strcpy(history[i - 1], history[i]);
        strcpy(history[SH_HISTORY_SIZE - 1], cmd);
    }
    hist_pos = hist_len;
}

/* Replace the visible line: cursor to end, destructive-\b erase, print new.
 * (The tty's \b erases the cell — the old shell's exact erase method.) */
static void set_line(const char* s) {
    term_right(ed_len - ed_cur);
    for (size_t i = 0; i < ed_len; i++) write(1, "\b", 1);
    ed_len = 0;
    ed_cur = 0;
    if (s && *s) {
        size_t i = 0;
        while (s[i] && i < SH_LINE_MAX - 1) { ed_buf[i] = s[i]; i++; }
        ed_buf[i] = '\0';
        ed_len = i;
        write(1, ed_buf, ed_len);
    } else {
        ed_buf[0] = '\0';
    }
    ed_cur = ed_len;
}

static void history_prev(void) {
    if (hist_len == 0) return;
    if (hist_pos == hist_len) {
        strcpy(edit_backup, ed_buf); /* stash the in-progress line */
    }
    if (hist_pos > 0) hist_pos--;
    set_line(history[hist_pos]);
}

static void history_next(void) {
    if (hist_len == 0) return;
    if (hist_pos < hist_len) hist_pos++;
    if (hist_pos == hist_len) set_line(edit_backup);
    else set_line(history[hist_pos]);
}

static void handle_csi(char final, const char* params, size_t plen) {
    if (final == 'A' && plen == 0) {        /* up arrow */
        history_prev();
    } else if (final == 'B' && plen == 0) { /* down arrow */
        history_next();
    } else if (final == 'D' && plen == 0) {        /* left arrow */
        if (ed_cur > 0) { ed_cur--; term_left(1); }
    } else if (final == 'C' && plen == 0) { /* right arrow */
        if (ed_cur < ed_len) { ed_cur++; term_right(1); }
    } else if (final == '~' && plen == 1 && params[0] == '3') { /* delete */
        do_delete();
    }
}

/* Read one line with editing. Returns 0 when a line is submitted (in
 * ed_buf), -1 on ^C (caller reprints the prompt). */
static int read_line(void) {
    ed_len = 0;
    ed_cur = 0;
    ed_buf[0] = '\0';

    int esc = 0; /* 0 idle, 1 got ESC, 2 in CSI */
    char csi_params[8];
    size_t csi_len = 0;

    for (;;) {
        char in[16];
        ssize_t n = read(0, in, sizeof(in));
        if (n < 0) {
            sleep_ms(100); /* not foreground / EINTR: retry, never exit */
            continue;
        }
        for (ssize_t i = 0; i < n; i++) {
            char c = in[i];

            if (esc == 1) {
                if (c == '[') { esc = 2; csi_len = 0; }
                else esc = 0; /* unknown ESC pair: swallow */
                continue;
            }
            if (esc == 2) {
                if (c >= 0x40 && c <= 0x7E) { /* final byte */
                    handle_csi(c, csi_params, csi_len);
                    esc = 0;
                } else if (csi_len < sizeof(csi_params)) {
                    csi_params[csi_len++] = c;
                }
                continue;
            }
            if (c == 27) { esc = 1; continue; }

            if (c == '\n' || c == '\r') {
                write(1, "\n", 1);
                ed_buf[ed_len] = '\0';
                return 0;
            }
            if (c == 3) { /* ^C delivered in-band by the keyboard driver */
                write(1, "^C\n", 3);
                return -1;
            }
            if (c == '\t') { autocomplete(); continue; }
            if (c == '\b' || c == 127) { do_backspace(); continue; }
            if ((unsigned char)c >= ' ' && (unsigned char)c != 0x7F) {
                insert_char_at_cursor(c); /* old-shell printable filter */
            }
        }
    }
}

/* Split on spaces in place. Returns argc, or -1 if there are too many
 * words. argv[argc] is set to NULL for execve. */
static int split_args(char* line, char** argv) {
    int argc = 0;
    char* p = line;
    for (;;) {
        while (*p == ' ') *p++ = '\0';
        if (*p == '\0') break;
        if (argc >= SH_ARGV_MAX - 1) return -1;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    argv[argc] = 0;
    return argc;
}

/* Join a + "/" + b into out. Returns 0 if it fits. */
static int path_join(char* out, size_t outsz, const char* a, const char* b) {
    size_t la = strlen(a);
    if (la + 1 + strlen(b) + 1 > outsz) return -1;
    strcpy(out, a);
    if (la > 0 && out[la - 1] != '/') strcat(out, "/");
    strcat(out, b);
    return 0;
}

/* Resolve a command name to an existing executable path in out.
 * Bare names try /bin then /bin/test; names containing '/' are used as
 * given (absolute) or joined with the cwd (relative). Returns 0 if a
 * candidate exists (probed with stat), -1 otherwise. */
static int resolve_command(const char* name, char* out, size_t outsz) {
    struct stat st;

    if (strchr(name, '/')) {
        if (name[0] == '/') {
            if (strlen(name) + 1 > outsz) return -1;
            strcpy(out, name);
        } else {
            char cwd[SH_PATH_MAX];
            if (!getcwd(cwd, sizeof(cwd))) return -1;
            if (path_join(out, outsz, cwd, name) < 0) return -1;
        }
        return (stat(out, &st) == 0) ? 0 : -1;
    }

    if (path_join(out, outsz, "/bin", name) == 0 && stat(out, &st) == 0)
        return 0;
    if (path_join(out, outsz, "/bin/test", name) == 0 && stat(out, &st) == 0)
        return 0;
    return -1;
}

static void run_command(char** argv) {
    char path[SH_PATH_MAX];
    if (resolve_command(argv[0], path, sizeof(path)) < 0) {
        printf("sh: command not found: %s\n", argv[0]);
        return;
    }

    int pid = fork();
    if (pid < 0) {
        printf("sh: fork failed\n");
        return;
    }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        setpgid(0, 0);
        execve(path, argv);
        _exit(127);
    }

    setpgid(pid, pid); /* both sides set it — standard race guard */
    tcsetpgrp(0, pid);
    int status = 0;
    waitpid(pid, &status, 0);
    tcsetpgrp(0, getpid());
}

static void builtin_cd(const char* arg) {
    const char* target = (arg && *arg) ? arg : "/";
    if (chdir(target) < 0) printf("cd: %s: no such directory\n", target);
}

static void builtin_help(void) {
    printf("builtins: cd [path], exit, help\n");
    printf("everything else runs from /bin (then /bin/test)\n");
}

int main(void) {
    setsid();
    signal(SIGINT, SIG_IGN);
    tcsetpgrp(0, getpid());

    char* argv[SH_ARGV_MAX];

    for (;;) {
        print_prompt();
        if (read_line() < 0) { hist_pos = hist_len; continue; } /* ^C */
        history_add(ed_buf); /* before split_args mutates ed_buf */

        if (strchr(ed_buf, '|') || strchr(ed_buf, '<') || strchr(ed_buf, '>')) {
            printf("sh: pipes/redirection not supported yet\n");
            continue;
        }

        int argc = split_args(ed_buf, argv);
        if (argc < 0) {
            printf("sh: too many arguments (max %d)\n", SH_ARGV_MAX - 1);
            continue;
        }
        if (argc == 0) continue;

        if (strcmp(argv[0], "exit") == 0) return 0; /* init respawns */
        if (strcmp(argv[0], "cd") == 0) {
            builtin_cd(argc > 1 ? argv[1] : 0);
            continue;
        }
        if (strcmp(argv[0], "help") == 0) {
            builtin_help();
            continue;
        }

        run_command(argv);
    }
}
