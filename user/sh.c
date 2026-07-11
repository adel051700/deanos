/* /bin/sh — phase-1 minimal userspace REPL (see
 * docs/superpowers/specs/2026-07-10-userspace-shell-design.md). */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/wait.h>

#define SH_LINE_MAX 256
#define SH_PATH_MAX 256
#define SH_ARGV_MAX 16 /* kernel ELF_ARGV_MAX, argv[0] included */
#define SH_PIPE_MAX_STAGES 4
#define SH_JOBS_MAX 16

/* forward decl: defined in the path-resolution section below, used by
 * autocomplete()'s path completion above it. */
static int path_join(char* out, size_t outsz, const char* a, const char* b);

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

/* Insert `rest` at the cursor, skipping chars already present after it
 * (old shell's skip-suffix behavior). */
static void insert_completion(const char* rest) {
    size_t skip = 0;
    while (rest[skip] && (ed_cur + skip) < ed_len &&
           ed_buf[ed_cur + skip] == rest[skip]) {
        skip++;
    }
    rest += skip;
    while (*rest && ed_len < SH_LINE_MAX - 1) {
        insert_char_at_cursor(*rest++);
    }
}

/* Tab: complete the word containing the cursor. First word = command name
 * (builtins, /bin, /bin/test); later words = path via dir_read. First
 * prefix match wins; silent no-op otherwise. */
static void autocomplete(void) {
    static const char* const sh_builtins[] = { "cd", "exit", "help", "jobs", "fg", "bg", 0 };

    ed_buf[ed_len] = '\0';

    size_t word_start = 0;
    for (size_t i = 0; i < ed_cur; i++) {
        if (ed_buf[i] == ' ') word_start = i + 1;
    }
    size_t word_len = ed_cur - word_start;
    if (word_len == 0) return;
    const char* word = &ed_buf[word_start];

    if (word_start == 0) {
        /* ---- command-name completion ---- */
        for (int i = 0; sh_builtins[i]; i++) {
            if (strncmp(sh_builtins[i], word, word_len) == 0) {
                insert_completion(sh_builtins[i] + word_len);
                return;
            }
        }
        static const char* const cmd_dirs[] = { "/bin", "/bin/test", 0 };
        for (int d = 0; cmd_dirs[d]; d++) {
            struct dirent e;
            for (unsigned idx = 0; dir_read(cmd_dirs[d], idx, &e) == 0; idx++) {
                if (e.type & DT_DIR) continue; /* dirs (e.g. /bin/test) aren't commands */
                if (strncmp(e.name, word, word_len) == 0) {
                    insert_completion(e.name + word_len);
                    return;
                }
            }
        }
        return;
    }

    /* ---- path completion for argument words ---- */
    char partial[SH_PATH_MAX];
    if (word_len >= sizeof(partial)) return;
    memcpy(partial, word, word_len);
    partial[word_len] = '\0';

    char dirpath[SH_PATH_MAX];
    const char* prefix;
    char* last_slash = strrchr(partial, '/');
    if (last_slash) {
        size_t dlen = (size_t)(last_slash - partial);
        char dpart[SH_PATH_MAX];
        if (dlen == 0) {
            strcpy(dpart, "/");
        } else {
            memcpy(dpart, partial, dlen);
            dpart[dlen] = '\0';
        }
        if (dpart[0] == '/') {
            strcpy(dirpath, dpart);
        } else {
            char cwd[SH_PATH_MAX];
            if (!getcwd(cwd, sizeof(cwd))) return;
            if (path_join(dirpath, sizeof(dirpath), cwd, dpart) < 0) return;
        }
        prefix = last_slash + 1; /* may be empty: "dir/" completes first entry */
    } else {
        if (!getcwd(dirpath, sizeof(dirpath))) return;
        prefix = partial;
    }
    size_t prefix_len = strlen(prefix);

    struct dirent e;
    for (unsigned idx = 0; dir_read(dirpath, idx, &e) == 0; idx++) {
        if (strncmp(e.name, prefix, prefix_len) == 0) {
            insert_completion(e.name + prefix_len);
            if ((e.type & DT_DIR) && ed_len < SH_LINE_MAX - 1) {
                insert_char_at_cursor('/'); /* old shell: mark directories */
            }
            return;
        }
    }
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
    if (hist_pos == hist_len) return; /* not browsing: Down is a no-op */
    hist_pos++;
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
            if (c == 12) { /* ^L: clear screen, redraw prompt + line */
                write(1, "\x1b[2J", 4);
                print_prompt();
                write(1, ed_buf, ed_len);
                term_left(ed_len - ed_cur);
                continue;
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

/* ---- pipelines / redirection / background jobs ------------------------ */

struct sh_stage {
    char* argv[SH_ARGV_MAX]; /* NULL-terminated, points into ed_buf */
    int   argc;
    char  in_path[SH_PATH_MAX];  /* first stage only; empty = none */
    char  out_path[SH_PATH_MAX]; /* last stage only; empty = none */
    int   has_in, has_out, append_out;
};

struct sh_job {
    int  in_use;
    int  seq;                       /* monotonic; highest = most recent */
    int  pgid;
    int  pids[SH_PIPE_MAX_STAGES];  /* -1 = reaped */
    int  npids;
    char cmd[SH_LINE_MAX];
};
static struct sh_job sh_jobs[SH_JOBS_MAX];
static int sh_job_seq;

static int is_builtin_name(const char* name) {
    static const char* const names[] = { "cd", "exit", "help", "jobs", "fg", "bg", 0 };
    for (int i = 0; names[i]; i++)
        if (strcmp(names[i], name) == 0) return 1;
    return 0;
}

static int job_add(int pgid, const int* pids, int npids, const char* cmd) {
    for (int i = 0; i < SH_JOBS_MAX; i++) {
        if (sh_jobs[i].in_use) continue;
        sh_jobs[i].in_use = 1;
        sh_jobs[i].seq = ++sh_job_seq;
        sh_jobs[i].pgid = pgid;
        sh_jobs[i].npids = npids;
        for (int k = 0; k < npids; k++) sh_jobs[i].pids[k] = pids[k];
        strcpy(sh_jobs[i].cmd, cmd); /* cmd < SH_LINE_MAX by construction */
        return 0;
    }
    return -1;
}

/* Absolutize a path against the cwd (kernel open() resolves relative
 * paths against /, so sh must do this). Existence is NOT probed —
 * output redirects create their file. */
static int abs_path(const char* name, char* out, size_t outsz) {
    if (name[0] == '/') {
        if (strlen(name) + 1 > outsz) return -1;
        strcpy(out, name);
        return 0;
    }
    char cwd[SH_PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return -1;
    return path_join(out, outsz, cwd, name);
}

/* Split the line on '|' in place. Returns stage count, or -1 if more than
 * SH_PIPE_MAX_STAGES. Segments may be empty (caught by parse_stage). */
static int split_pipeline(char* line, char* segs[SH_PIPE_MAX_STAGES]) {
    int n = 0;
    char* p = line;
    segs[n++] = p;
    while (*p) {
        if (*p == '|') {
            *p++ = '\0';
            if (n >= SH_PIPE_MAX_STAGES) return -1;
            segs[n++] = p;
        } else {
            p++;
        }
    }
    return n;
}

/* Parse one stage segment in place: argv words plus <in / >out / >>out
 * (attached or space-separated, old-shell lexer rules: < and > always
 * break a token). Returns 0, -1 on syntax error. */
static int parse_stage(char* seg, struct sh_stage* st) {
    memset(st, 0, sizeof(*st));
    char* p = seg;

    for (;;) {
        while (*p == ' ') *p++ = '\0';
        if (*p == '\0') break;

        if (*p == '<' || *p == '>') {
            int is_in = (*p == '<');
            int append = 0;
            *p++ = '\0';
            if (!is_in && *p == '>') { append = 1; *p++ = '\0'; }
            while (*p == ' ') *p++ = '\0';
            if (*p == '\0' || *p == '<' || *p == '>') return -1; /* missing path */

            /* copy the path token out (it may be terminated by an operator
             * we must not consume yet) */
            char tok[SH_PATH_MAX];
            size_t tl = 0;
            while (*p && *p != ' ' && *p != '<' && *p != '>' && tl < sizeof(tok) - 1)
                tok[tl++] = *p++;
            tok[tl] = '\0';

            if (is_in) {
                if (st->has_in) return -1; /* duplicate < */
                strcpy(st->in_path, tok);
                st->has_in = 1;
            } else {
                if (st->has_out) return -1; /* duplicate > */
                strcpy(st->out_path, tok);
                st->has_out = 1;
                st->append_out = append;
            }
            continue;
        }

        /* argv word: runs until space or operator; the terminating NUL is
         * written when the following space/operator is consumed */
        if (st->argc >= SH_ARGV_MAX - 1) return -1;
        st->argv[st->argc++] = p;
        while (*p && *p != ' ' && *p != '<' && *p != '>') p++;
    }

    st->argv[st->argc] = 0;
    return (st->argc > 0) ? 0 : -1;
}


static void run_pipeline(struct sh_stage* st, int nstages, int background,
                         const char* cmdline) {
    static char paths[SH_PIPE_MAX_STAGES][SH_PATH_MAX];
    int in_fd = -1, out_fd = -1;
    int pipes[SH_PIPE_MAX_STAGES - 1][2];
    int pids[SH_PIPE_MAX_STAGES];
    int npipes = nstages - 1;
    int nforked = 0;

    for (int i = 0; i < npipes; i++) { pipes[i][0] = -1; pipes[i][1] = -1; }

    /* 0: a background job needs a free table slot — refuse up front
     * (spec: "table full → nothing runs") */
    if (background) {
        int free_slot = 0;
        for (int i = 0; i < SH_JOBS_MAX; i++) {
            if (!sh_jobs[i].in_use) { free_slot = 1; break; }
        }
        if (!free_slot) {
            printf("sh: job table full\n");
            return;
        }
    }

    /* 1: resolve every command before touching any fd */
    for (int i = 0; i < nstages; i++) {
        if (resolve_command(st[i].argv[0], paths[i], SH_PATH_MAX) < 0) {
            printf("sh: command not found: %s\n", st[i].argv[0]);
            return;
        }
    }

    /* 2: open all redirects before anything forks
     * Kernel fd_alloc never hands out fds 0-2, so pipe/redirect fds are
     * always >= 3 and can't collide with the dup2 targets. */
    if (st[0].has_in) {
        char rp[SH_PATH_MAX];
        if (abs_path(st[0].in_path, rp, sizeof(rp)) < 0 ||
            (in_fd = open(rp, O_RDONLY)) < 0) {
            printf("sh: cannot open input: %s\n", st[0].in_path);
            return;
        }
    }
    if (st[nstages - 1].has_out) {
        char rp[SH_PATH_MAX];
        int flags = O_WRONLY | O_CREAT |
                    (st[nstages - 1].append_out ? O_APPEND : O_TRUNC);
        if (abs_path(st[nstages - 1].out_path, rp, sizeof(rp)) < 0 ||
            (out_fd = open(rp, flags)) < 0) {
            printf("sh: cannot open output: %s\n", st[nstages - 1].out_path);
            if (in_fd >= 0) close(in_fd);
            return;
        }
    }

    /* 3: pipes */
    for (int i = 0; i < npipes; i++) {
        if (pipe(pipes[i]) < 0) {
            printf("sh: pipe failed\n");
            for (int j = 0; j < i; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            if (in_fd >= 0) close(in_fd);
            if (out_fd >= 0) close(out_fd);
            return;
        }
    }

    /* 4: fork each stage */
    for (int i = 0; i < nstages; i++) {
        int pid = fork();
        if (pid < 0) {
            printf("sh: fork failed\n");
            background = 0; /* spec: reap already-forked stages, don't track */
            break;
        }
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            setpgid(0, nforked ? pids[0] : 0);

            int child_in  = (i == 0) ? in_fd : pipes[i - 1][0];
            int child_out = (i == nstages - 1) ? out_fd : pipes[i][1];
            if (child_in  >= 0) dup2(child_in, 0);
            if (child_out >= 0) dup2(child_out, 1);

            for (int j = 0; j < npipes; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            if (in_fd >= 0) close(in_fd);
            if (out_fd >= 0) close(out_fd);

            execve(paths[i], st[i].argv);
            _exit(127);
        }
        setpgid(pid, nforked ? pids[0] : pid); /* race-guard mirror */
        pids[nforked++] = pid;
    }

    /* 5: parent drops every pipeline fd (a kept write end would block EOF) */
    for (int j = 0; j < npipes; j++) { close(pipes[j][0]); close(pipes[j][1]); }
    if (in_fd >= 0) close(in_fd);
    if (out_fd >= 0) close(out_fd);

    if (nforked == 0) return;

    /* 6: wait or background */
    if (!background) {
        tcsetpgrp(0, pids[0]);
        for (int i = 0; i < nforked; i++) {
            int status = 0;
            waitpid(pids[i], &status, 0);
        }
        tcsetpgrp(0, getpid());
    } else {
        if (job_add(pids[0], pids, nforked, cmdline) < 0) {
            /* table full: job already runs untracked; warn (spec: refuse
               happens at launch decision — see main; this is the fork-raced
               fallback) */
            printf("sh: job table full; job %d untracked\n", pids[0]);
        } else {
            printf("[%d] %s\n", pids[0], cmdline);
        }
    }
}

/* Poll every live pid; print and free jobs whose pids have all exited.
 * Called before each prompt and by the jobs builtin. */
static void jobs_reap(void) {
    for (int i = 0; i < SH_JOBS_MAX; i++) {
        if (!sh_jobs[i].in_use) continue;

        int alive = 0;
        for (int k = 0; k < sh_jobs[i].npids; k++) {
            if (sh_jobs[i].pids[k] < 0) continue;
            int status = 0;
            int rc = waitpid(sh_jobs[i].pids[k], &status, WNOHANG);
            if (rc == sh_jobs[i].pids[k] || rc == -3) {
                sh_jobs[i].pids[k] = -1;
            } else {
                alive = 1;
            }
        }

        if (!alive) {
            printf("[%d] done: %s\n", sh_jobs[i].pgid, sh_jobs[i].cmd);
            sh_jobs[i].in_use = 0;
        }
    }
}

static struct sh_job* job_find(int pgid) {
    for (int i = 0; i < SH_JOBS_MAX; i++) {
        if (sh_jobs[i].in_use && sh_jobs[i].pgid == pgid) return &sh_jobs[i];
    }
    return 0;
}

static struct sh_job* job_most_recent(void) {
    struct sh_job* best = 0;
    for (int i = 0; i < SH_JOBS_MAX; i++) {
        if (!sh_jobs[i].in_use) continue;
        if (!best || sh_jobs[i].seq > best->seq) best = &sh_jobs[i];
    }
    return best;
}

static void builtin_jobs(void) {
    jobs_reap();
    for (int i = 0; i < SH_JOBS_MAX; i++) {
        if (!sh_jobs[i].in_use) continue;
        printf("[%d] running: %s\n", sh_jobs[i].pgid, sh_jobs[i].cmd);
    }
}

static void builtin_fg(const char* arg) {
    jobs_reap();
    struct sh_job* j = (arg && *arg) ? job_find(atoi(arg)) : job_most_recent();
    if (!j) {
        printf("fg: no such job\n");
        return;
    }

    tcsetpgrp(0, j->pgid);
    for (int k = 0; k < j->npids; k++) {
        if (j->pids[k] < 0) continue;
        int status = 0;
        waitpid(j->pids[k], &status, 0);
    }
    tcsetpgrp(0, getpid());
    j->in_use = 0;
}

static void builtin_bg(const char* arg) {
    jobs_reap();
    struct sh_job* j = (arg && *arg) ? job_find(atoi(arg)) : job_most_recent();
    if (!j) {
        printf("bg: no such job\n");
        return;
    }
    /* No SIGTSTP in this kernel: jobs are always already running. */
    printf("bg: job %d already running in background\n", j->pgid);
}

static void builtin_cd(const char* arg) {
    const char* target = (arg && *arg) ? arg : "/";
    if (chdir(target) < 0) printf("cd: %s: no such directory\n", target);
}

static void builtin_help(void) {
    printf("builtins: cd [path], exit, help, jobs, fg [pgid], bg [pgid]\n");
    printf("pipelines: a | b | c (max %d stages), < in, > out, >> append, & background\n",
           SH_PIPE_MAX_STAGES);
    printf("everything else runs from /bin (then /bin/test)\n");
}

int main(void) {
    setsid();
    signal(SIGINT, SIG_IGN);
    tcsetpgrp(0, getpid());

    char* argv[SH_ARGV_MAX];

    for (;;) {
        jobs_reap(); /* announce finished background jobs before the prompt */
        print_prompt();
        if (read_line() < 0) { hist_pos = hist_len; continue; } /* ^C */
        history_add(ed_buf); /* before split_args mutates ed_buf */

        char cmdline[SH_LINE_MAX];
        strcpy(cmdline, ed_buf); /* display copy; parsing mutates ed_buf */

        /* trailing '&' → background */
        int background = 0;
        {
            size_t n = strlen(ed_buf);
            while (n > 0 && ed_buf[n - 1] == ' ') ed_buf[--n] = '\0';
            if (n > 0 && ed_buf[n - 1] == '&') {
                background = 1;
                ed_buf[--n] = '\0';
            }
        }

        if (!background && !strchr(ed_buf, '|') && !strchr(ed_buf, '<') &&
            !strchr(ed_buf, '>')) {
            /* plain single command: the existing fast path */
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
            if (strcmp(argv[0], "jobs") == 0) {
                builtin_jobs();
                continue;
            }
            if (strcmp(argv[0], "fg") == 0) {
                builtin_fg(argc > 1 ? argv[1] : 0);
                continue;
            }
            if (strcmp(argv[0], "bg") == 0) {
                builtin_bg(argc > 1 ? argv[1] : 0);
                continue;
            }

            run_command(argv);
            continue;
        }

        /* pipeline / redirection / background path */
        char* segs[SH_PIPE_MAX_STAGES];
        int nstages = split_pipeline(ed_buf, segs);
        if (nstages < 0) {
            printf("sh: too many pipeline stages (max %d)\n", SH_PIPE_MAX_STAGES);
            continue;
        }

        static struct sh_stage stages[SH_PIPE_MAX_STAGES];
        int bad = 0;
        for (int i = 0; i < nstages; i++) {
            if (parse_stage(segs[i], &stages[i]) < 0) {
                printf("sh: syntax error\n");
                bad = 1;
                break;
            }
            if (is_builtin_name(stages[i].argv[0])) {
                printf("sh: %s is a builtin, not runnable in a pipeline or background\n",
                       stages[i].argv[0]);
                bad = 1;
                break;
            }
            if (i > 0 && stages[i].has_in) {
                printf("sh: '<' only allowed on the first stage\n");
                bad = 1;
                break;
            }
            if (i < nstages - 1 && stages[i].has_out) {
                printf("sh: '>' only allowed on the last stage\n");
                bad = 1;
                break;
            }
        }
        if (bad) continue;

        run_pipeline(stages, nstages, background, cmdline);
    }
}
