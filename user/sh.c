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

/* Read one line with echo. Returns 0 on a completed line, -1 on ^C
 * (caller reprints the prompt). Arrow keys and other escape sequences
 * are swallowed; input beyond the buffer cap is silently dropped. */
static int read_line(char* line) {
    size_t len = 0;
    int esc = 0; /* 0 idle, 1 saw ESC, 2 inside CSI */

    for (;;) {
        char buf[16];
        ssize_t n = read(0, buf, sizeof(buf));
        if (n < 0) {
            /* Unexpectedly not foreground (or EINTR): retry, don't exit —
             * exiting here would make init respawn-storm. */
            sleep_ms(100);
            continue;
        }
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];

            if (esc == 1) {
                esc = (c == '[') ? 2 : 0;
                continue;
            }
            if (esc == 2) {
                if (c >= 0x40 && c <= 0x7E) esc = 0; /* CSI final byte */
                continue;
            }
            if (c == 27) {
                esc = 1;
                continue;
            }

            if (c == '\n' || c == '\r') {
                write(1, "\n", 1);
                line[len] = '\0';
                return 0;
            }
            if (c == 3) { /* ^C delivered in-band by the keyboard driver */
                write(1, "^C\n", 3);
                return -1;
            }
            if (c == '\b' || c == 127) {
                if (len > 0) {
                    len--;
                    write(1, "\b \b", 3);
                }
                continue;
            }
            if (c < 32 || c > 126) continue; /* other non-printables */
            if (len < SH_LINE_MAX - 1) {
                line[len++] = c;
                write(1, &c, 1);
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

    char line[SH_LINE_MAX];
    char* argv[SH_ARGV_MAX];

    for (;;) {
        print_prompt();
        if (read_line(line) < 0) continue; /* ^C: fresh prompt */

        if (strchr(line, '|') || strchr(line, '<') || strchr(line, '>')) {
            printf("sh: pipes/redirection not supported yet\n");
            continue;
        }

        int argc = split_args(line, argv);
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
