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
#define SH_VAR_MAX      32
#define SH_VAR_NAME_MAX 32
#define SH_VAR_VAL_MAX  128

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

/* ---- shell variables (local to this sh process; not exported to child
 * process environments — see the design doc's Non-goals) ---------------- */

struct sh_var { char name[SH_VAR_NAME_MAX]; char val[SH_VAR_VAL_MAX]; int used; };
static struct sh_var sh_vars[SH_VAR_MAX];

static struct sh_var* var_find(const char* name) {
    for (int i = 0; i < SH_VAR_MAX; i++)
        if (sh_vars[i].used && strcmp(sh_vars[i].name, name) == 0) return &sh_vars[i];
    return 0;
}

/* Unset variables read as "" (shell convention) — never NULL. */
static const char* get_var(const char* name) {
    struct sh_var* v = var_find(name);
    return v ? v->val : "";
}

/* Reuses an existing slot for `name`, else the first free slot; silently
 * no-ops if the table is full (32 variables is generous for a script —
 * running out just drops the assignment rather than crashing). */
static void set_var(const char* name, const char* val) {
    struct sh_var* v = var_find(name);
    if (!v) {
        for (int i = 0; i < SH_VAR_MAX; i++) {
            if (!sh_vars[i].used) { v = &sh_vars[i]; break; }
        }
        if (!v) return;
        strncpy(v->name, name, SH_VAR_NAME_MAX - 1);
        v->name[SH_VAR_NAME_MAX - 1] = '\0';
        v->used = 1;
    }
    strncpy(v->val, val, SH_VAR_VAL_MAX - 1);
    v->val[SH_VAR_VAL_MAX - 1] = '\0';
}

/* $? — set after every simple command, pipeline, and condition. */
static void set_status_var(int status) {
    char buf[12];
    itoa(status, buf, 10);
    set_var("?", buf);
}

/* ---- word/name helpers -------------------------------------------------- */

static int is_ident_start(char c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_ident_char(char c)  { return is_ident_start(c) || (c >= '0' && c <= '9'); }

/* ---- integer arithmetic: $((expr)) — + - * / %, parens, unary minus,
 * bare identifiers resolved as variables (get_var + atoi; unset or
 * non-numeric reads as 0). Division/modulo by zero reads as 0 rather than
 * faulting. ---------------------------------------------------------- */

static int ae_expr(const char** p);

static void ae_skip_ws(const char** p) { while (**p == ' ') (*p)++; }

static int ae_factor(const char** p) {
    ae_skip_ws(p);
    if (**p == '(') {
        (*p)++;
        int v = ae_expr(p);
        ae_skip_ws(p);
        if (**p == ')') (*p)++;
        return v;
    }
    if (**p == '-') { (*p)++; return -ae_factor(p); }
    if (**p >= '0' && **p <= '9') {
        int v = 0;
        while (**p >= '0' && **p <= '9') v = v * 10 + (*(*p)++ - '0');
        return v;
    }
    if (is_ident_start(**p)) {
        char name[SH_VAR_NAME_MAX];
        size_t n = 0;
        while (is_ident_char(**p) && n < sizeof(name) - 1) name[n++] = *(*p)++;
        name[n] = '\0';
        return atoi(get_var(name));
    }
    return 0; /* malformed input: 0 rather than crashing */
}

static int ae_term(const char** p) {
    int v = ae_factor(p);
    for (;;) {
        ae_skip_ws(p);
        if (**p == '*') { (*p)++; v *= ae_factor(p); }
        else if (**p == '/') { (*p)++; int d = ae_factor(p); v = d ? v / d : 0; }
        else if (**p == '%') { (*p)++; int d = ae_factor(p); v = d ? v % d : 0; }
        else break;
    }
    return v;
}

static int ae_expr(const char** p) {
    int v = ae_term(p);
    for (;;) {
        ae_skip_ws(p);
        if (**p == '+') { (*p)++; v += ae_term(p); }
        else if (**p == '-') { (*p)++; v -= ae_term(p); }
        else break;
    }
    return v;
}

static int arith_eval(const char* expr) {
    const char* p = expr;
    return ae_expr(&p);
}

/* Scans forward from `p` (already past the "$((" prefix) tracking paren
 * depth opened *within* the expression. Returns a pointer to the first
 * ')' that closes the wrapper (i.e. found at depth 0), or NULL if the
 * string ends first. The caller must still confirm the very next char
 * is also ')' to accept the "))" wrapper close. */
static const char* find_arith_close(const char* p) {
    int depth = 0;
    while (*p) {
        if (*p == '(') { depth++; p++; }
        else if (*p == ')') {
            if (depth == 0) return p;
            depth--; p++;
        } else {
            p++;
        }
    }
    return 0;
}

/* Expands one already-whitespace-split token: $((expr)) must start the
 * token (whole-token match, no embedded whitespace — see design doc);
 * $name / ${name} / $? may appear anywhere in the token and are
 * substituted in place. Everything else copies through literally. */
static void expand_word(const char* in, char* out, size_t outsz) {
    if (in[0] == '$' && in[1] == '(' && in[2] == '(') {
        const char* close = find_arith_close(in + 3);
        if (close && close[1] == ')') {
            char expr[SH_LINE_MAX];
            size_t elen = (size_t)(close - (in + 3));
            if (elen >= sizeof(expr)) elen = sizeof(expr) - 1;
            memcpy(expr, in + 3, elen);
            expr[elen] = '\0';
            char numbuf[12];
            itoa(arith_eval(expr), numbuf, 10);
            size_t oi = 0;
            for (const char* s = numbuf; *s && oi < outsz - 1; s++) out[oi++] = *s;
            out[oi] = '\0';
            return;
        }
    }

    size_t oi = 0;
    const char* p = in;
    while (*p && oi < outsz - 1) {
        if (*p != '$') { out[oi++] = *p++; continue; }

        if (p[1] == '?') {
            const char* val = get_var("?");
            for (const char* s = val; *s && oi < outsz - 1; s++) out[oi++] = *s;
            p += 2;
            continue;
        }

        int braced = (p[1] == '{');
        const char* np = p + 1 + braced;
        if (!is_ident_start(*np)) { out[oi++] = *p++; continue; } /* lone '$': literal */

        char name[SH_VAR_NAME_MAX];
        size_t n = 0;
        while (is_ident_char(*np) && n < sizeof(name) - 1) name[n++] = *np++;
        name[n] = '\0';
        if (braced && *np == '}') np++;

        const char* val = get_var(name);
        for (const char* s = val; *s && oi < outsz - 1; s++) out[oi++] = *s;
        p = np;
    }
    out[oi] = '\0';
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
    static const char* const sh_builtins[] = { "cd", "exit", "help", "jobs", "fg", "bg", "test", 0 };

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

/* ---- history (old kernel shell semantics, persisted to a file) ------- */

#define SH_HISTORY_SIZE 32
#define SH_HISTORY_FILE "/.sh_history"
static char history[SH_HISTORY_SIZE][SH_LINE_MAX];
static int  hist_len;
static int  hist_pos; /* == hist_len when not browsing */
static char edit_backup[SH_LINE_MAX];

typedef enum { HIST_SKIPPED = 0, HIST_APPENDED = 1, HIST_EVICTED = 2 } hist_add_result_t;

/* Memory-only add: today's exact history semantics (dedup consecutive
 * duplicates, cap at SH_HISTORY_SIZE by shifting out the oldest). Used
 * directly by history_load() so loading never re-persists what it read. */
static hist_add_result_t history_add_mem(const char* cmd) {
    if (!cmd || !*cmd) { hist_pos = hist_len; return HIST_SKIPPED; }
    if (hist_len > 0 && strcmp(history[hist_len - 1], cmd) == 0) {
        hist_pos = hist_len; /* consecutive duplicate: skip */
        return HIST_SKIPPED;
    }
    if (hist_len < SH_HISTORY_SIZE) {
        strcpy(history[hist_len], cmd); /* cmd is < SH_LINE_MAX by construction */
        hist_len++;
        hist_pos = hist_len;
        return HIST_APPENDED;
    }
    for (int i = 1; i < SH_HISTORY_SIZE; i++)
        strcpy(history[i - 1], history[i]);
    strcpy(history[SH_HISTORY_SIZE - 1], cmd);
    hist_pos = hist_len;
    return HIST_EVICTED;
}

/* Appends one line to SH_HISTORY_FILE. Used while the ring hasn't wrapped
 * yet, so the file is always exactly today's history[] on disk. */
static void history_persist_append(const char* cmd) {
    int fd = open(SH_HISTORY_FILE, O_WRONLY | O_CREAT | O_APPEND);
    if (fd < 0) return; /* read-only root, no disk, etc: history still works in-memory */
    write(fd, cmd, strlen(cmd));
    write(fd, "\n", 1);
    close(fd);
}

/* Rewrites SH_HISTORY_FILE from history[] in full. Used once the ring
 * wraps, since history[] is then the sole authoritative view — this keeps
 * the file bounded at SH_HISTORY_SIZE lines forever, so history_load()
 * never needs more than a single bounded sequential read. */
static void history_persist_rewrite(void) {
    int fd = open(SH_HISTORY_FILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    for (int i = 0; i < hist_len; i++) {
        write(fd, history[i], strlen(history[i]));
        write(fd, "\n", 1);
    }
    close(fd);
}

static void history_add(const char* cmd) {
    hist_add_result_t r = history_add_mem(cmd);
    if (r == HIST_APPENDED) history_persist_append(cmd);
    else if (r == HIST_EVICTED) history_persist_rewrite();
}

/* Loads prior sessions' history from SH_HISTORY_FILE at shell startup.
 * The file is always <= SH_HISTORY_SIZE lines (history_persist_rewrite
 * keeps it that way), so a single bounded read covers it. Missing file is
 * a silent no-op (cold start, matches today's empty-history behavior). */
static void history_load(void) {
    int fd = open(SH_HISTORY_FILE, O_RDONLY);
    if (fd < 0) return;

    static char buf[SH_HISTORY_SIZE * SH_LINE_MAX];
    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf + total, sizeof(buf) - total);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= sizeof(buf)) break;
    }
    close(fd);

    size_t line_start = 0;
    for (size_t i = 0; i < total; i++) {
        if (buf[i] == '\n') {
            buf[i] = '\0';
            if (i > line_start) history_add_mem(&buf[line_start]);
            line_start = i + 1;
        }
    }
}

/* Erase `len` characters currently on screen, cursor currently `cur` chars
 * from the start of that displayed text (moves right to the end first,
 * then destructive-\b erases — the old shell's exact erase method). */
static void erase_line_display(size_t len, size_t cur) {
    term_right(len - cur);
    for (size_t i = 0; i < len; i++) write(1, "\b", 1);
}

/* Replace the visible line: erase, print new. */
static void set_line(const char* s) {
    erase_line_display(ed_len, ed_cur);
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

/* ---- reverse search (Ctrl-R), bash-style incremental ------------------ */

static int    search_active = 0;
static char   search_term[SH_LINE_MAX];
static size_t search_len = 0;
static int    search_match_idx = -1;      /* index into history[], or >= hist_len = no match */
static char   search_saved_line[SH_LINE_MAX]; /* ed_buf snapshot, restored on cancel */
static size_t search_disp_len = 0;        /* length of the search-prompt text on screen */

static void search_redraw(void) {
    erase_line_display(search_disp_len, search_disp_len);
    search_term[search_len] = '\0';
    const char* match = (search_match_idx >= 0 && search_match_idx < hist_len)
                         ? history[search_match_idx] : "";
    search_disp_len = (size_t)printf("(reverse-i-search)`%s': %s", search_term, match);
}

/* Searches history[] from `start` down to 0 for the first entry
 * containing search_term as a substring. Leaves search_match_idx
 * unchanged on a miss (keeps showing the last good match, bash's
 * behavior), then redraws either way. */
static void search_find_from(int start) {
    search_term[search_len] = '\0';
    for (int i = start; i >= 0; i--) {
        if (strstr(history[i], search_term)) { search_match_idx = i; break; }
    }
    search_redraw();
}

static void search_enter(void) {
    erase_line_display(ed_len, ed_cur);
    size_t i = 0;
    while (i < ed_len) { search_saved_line[i] = ed_buf[i]; i++; }
    search_saved_line[i] = '\0';
    search_active = 1;
    search_len = 0;
    search_term[0] = '\0';
    search_match_idx = hist_len;
    search_disp_len = 0;
    search_find_from(hist_len - 1);
}

static void search_add_char(char c) {
    if (search_len >= SH_LINE_MAX - 1) return;
    search_term[search_len++] = c;
    int start = (search_match_idx < hist_len) ? search_match_idx : hist_len - 1;
    search_find_from(start);
}

static void search_backspace(void) {
    if (search_len > 0) search_len--;
    search_match_idx = hist_len;
    search_find_from(hist_len - 1);
}

static void search_step(void) {
    int start = (search_match_idx < hist_len) ? (search_match_idx - 1) : (hist_len - 1);
    search_find_from(start);
}

/* Shared by cancel/accept: erase the search prompt, write `s` as the new
 * line. Does not touch history state. */
static void restore_line_display(const char* s) {
    erase_line_display(search_disp_len, search_disp_len);
    size_t i = 0;
    while (s[i] && i < SH_LINE_MAX - 1) { ed_buf[i] = s[i]; i++; }
    ed_buf[i] = '\0';
    ed_len = i;
    ed_cur = i;
    write(1, ed_buf, ed_len);
}

static void search_cancel(void) {
    restore_line_display(search_saved_line);
    search_active = 0;
}

static void search_accept_and_exit(void) {
    const char* s = (search_match_idx >= 0 && search_match_idx < hist_len)
                     ? history[search_match_idx] : search_saved_line;
    restore_line_display(s);
    search_active = 0;
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
    search_active = 0;

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
                    if (search_active) search_accept_and_exit();
                    handle_csi(c, csi_params, csi_len);
                    esc = 0;
                } else if (csi_len < sizeof(csi_params)) {
                    csi_params[csi_len++] = c;
                }
                continue;
            }
            if (c == 27) {
                /* A real arrow/function key sends ESC '[' <byte> together
                 * from the keyboard driver, landing in the same read()
                 * burst — check the next buffered byte before deciding
                 * this is a standalone Escape (which cancels search) vs.
                 * the start of a sequence (deferred to the CSI-final
                 * branch below, which accepts the match first). */
                if (search_active && !(i + 1 < n && in[i + 1] == '[')) {
                    search_cancel();
                }
                esc = 1;
                continue;
            }

            if (search_active) {
                if (c == 18) { search_step(); continue; }  /* ^R again: next older match */
                if (c == '\n' || c == '\r') {
                    search_accept_and_exit();
                    write(1, "\n", 1);
                    ed_buf[ed_len] = '\0';
                    return 0;
                }
                if (c == 7) { search_cancel(); continue; } /* ^G: cancel */
                if (c == '\b' || c == 127) { search_backspace(); continue; }
                if ((unsigned char)c >= ' ' && (unsigned char)c != 0x7F) {
                    search_add_char(c);
                    continue;
                }
                /* any other control byte: accept the match, then let it
                 * fall through to the normal dispatch below for this
                 * same byte (e.g. ^C aborts, ^L clears, Tab completes,
                 * on the now-accepted line). */
                search_accept_and_exit();
            }

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
            if (c == 18) { search_enter(); continue; } /* ^R: start reverse search */
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

static int run_command(char** argv) {
    char path[SH_PATH_MAX];
    if (resolve_command(argv[0], path, sizeof(path)) < 0) {
        printf("sh: command not found: %s\n", argv[0]);
        return 127;
    }

    int pid = fork();
    if (pid < 0) {
        printf("sh: fork failed\n");
        return 1;
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
    return status;
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
    static const char* const names[] = { "cd", "exit", "help", "jobs", "fg", "bg", "test", "[", 0 };
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


static int run_pipeline(struct sh_stage* st, int nstages, int background,
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
            return 1;
        }
    }

    /* 1: resolve every command before touching any fd */
    for (int i = 0; i < nstages; i++) {
        if (resolve_command(st[i].argv[0], paths[i], SH_PATH_MAX) < 0) {
            printf("sh: command not found: %s\n", st[i].argv[0]);
            return 1;
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
            return 1;
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
            return 1;
        }
    }

    /* 3: pipes */
    for (int i = 0; i < npipes; i++) {
        if (pipe(pipes[i]) < 0) {
            printf("sh: pipe failed\n");
            for (int j = 0; j < i; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            if (in_fd >= 0) close(in_fd);
            if (out_fd >= 0) close(out_fd);
            return 1;
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

    if (nforked == 0) return 1;

    /* 6: wait or background */
    int status = 0;
    if (!background) {
        tcsetpgrp(0, pids[0]);
        for (int i = 0; i < nforked; i++) {
            int st = 0;
            waitpid(pids[i], &st, 0);
            if (i == nforked - 1) status = st;
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
    return status;
}

/* forward decls: builtins are defined below main(), exec_line calls them */
static void builtin_cd(const char* arg);
static void builtin_help(void);
static void builtin_jobs(void);
static void builtin_fg(const char* arg);
static void builtin_bg(const char* arg);
static int builtin_test(int argc, char** argv);

/* ---- exec_line: assignment / plain command / pipeline dispatch -------- */

/* Recognizes an entire line as a bare `name=value` assignment: identifier,
 * '=', and no whitespace anywhere in the line (design doc: no
 * prefix-assignment-then-command, no word-splitting of the value — the
 * whole line must be exactly the assignment). */
static int is_assignment(const char* line, char* name, size_t namesz, const char** rawval) {
    if (!is_ident_start(line[0])) return 0;
    const char* p = line;
    while (is_ident_char(*p)) p++;
    size_t n = (size_t)(p - line);
    if (n == 0 || n >= namesz) return 0;
    if (*p != '=') return 0;
    for (const char* q = line; *q; q++) if (*q == ' ') return 0;
    memcpy(name, line, n);
    name[n] = '\0';
    *rawval = p + 1;
    return 1;
}

/* Expands each of argv[0..argc) into its own row of `scratch` and
 * repoints argv[i] there — never overwrites the original token in place
 * (an expansion can be longer than the token it replaces). Caller owns
 * `scratch` so a multi-stage pipeline can give each stage a distinct row. */
static void expand_argv(char** argv, int argc, char scratch[][SH_LINE_MAX]) {
    for (int i = 0; i < argc; i++) {
        expand_word(argv[i], scratch[i], SH_LINE_MAX);
        argv[i] = scratch[i];
    }
}

static int exec_line(char* line) {
    char display[SH_LINE_MAX];
    strcpy(display, line);

    char aname[SH_VAR_NAME_MAX];
    const char* rawval;
    if (is_assignment(line, aname, sizeof(aname), &rawval)) {
        char val[SH_VAR_VAL_MAX];
        expand_word(rawval, val, sizeof(val));
        set_var(aname, val);
        set_status_var(0);
        return 0;
    }

    /* trailing '&' -> background */
    int background = 0;
    {
        size_t n = strlen(line);
        while (n > 0 && line[n - 1] == ' ') line[--n] = '\0';
        if (n > 0 && line[n - 1] == '&') {
            background = 1;
            line[--n] = '\0';
        }
    }

    int status;
    char* argv[SH_ARGV_MAX];

    if (!background && !strchr(line, '|') && !strchr(line, '<') && !strchr(line, '>')) {
        int argc = split_args(line, argv);
        if (argc < 0) {
            printf("sh: too many arguments (max %d)\n", SH_ARGV_MAX - 1);
            status = 1;
            set_status_var(status);
            return status;
        }
        if (argc == 0) return atoi(get_var("?")); /* blank line: $? unchanged */

        static char fastbufs[SH_ARGV_MAX][SH_LINE_MAX];
        expand_argv(argv, argc, fastbufs);

        if (strcmp(argv[0], "exit") == 0) exit(argc > 1 ? atoi(argv[1]) : 0);
        if (strcmp(argv[0], "cd") == 0) { builtin_cd(argc > 1 ? argv[1] : 0); status = 0; }
        else if (strcmp(argv[0], "help") == 0) { builtin_help(); status = 0; }
        else if (strcmp(argv[0], "jobs") == 0) { builtin_jobs(); status = 0; }
        else if (strcmp(argv[0], "fg") == 0) { builtin_fg(argc > 1 ? argv[1] : 0); status = 0; }
        else if (strcmp(argv[0], "bg") == 0) { builtin_bg(argc > 1 ? argv[1] : 0); status = 0; }
        else if (strcmp(argv[0], "test") == 0 || strcmp(argv[0], "[") == 0) status = builtin_test(argc, argv);
        else status = run_command(argv);

        set_status_var(status);
        return status;
    }

    char* segs[SH_PIPE_MAX_STAGES];
    int nstages = split_pipeline(line, segs);
    if (nstages < 0) {
        printf("sh: too many pipeline stages (max %d)\n", SH_PIPE_MAX_STAGES);
        set_status_var(1);
        return 1;
    }

    static struct sh_stage stages[SH_PIPE_MAX_STAGES];
    static char pipebufs[SH_PIPE_MAX_STAGES][SH_ARGV_MAX][SH_LINE_MAX];
    int bad = 0;
    for (int i = 0; i < nstages; i++) {
        if (parse_stage(segs[i], &stages[i]) < 0) {
            printf("sh: syntax error\n");
            bad = 1;
            break;
        }
        expand_argv(stages[i].argv, stages[i].argc, pipebufs[i]);
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
    if (bad) { set_status_var(1); return 1; }

    status = run_pipeline(stages, nstages, background, display);
    set_status_var(status);
    return status;
}

#define SH_BLOCK_LINES_MAX 64
#define SH_NEST_MAX        8

/* A source of raw lines for the block interpreter: either the interactive
 * editor (with a "> " continuation prompt) or a pre-split array — a
 * captured block body and a script file are both just arrays. */
typedef struct {
    int    interactive;
    char** lines;   /* unused when interactive */
    int    nlines, idx;
} line_src_t;

/* Returns 0 with `out` filled, -1 at EOF (array sources) or ^C
 * (interactive — read_line()'s existing -1-on-cancel return propagates
 * the same way it does at the top-level prompt). */
static int src_next(line_src_t* s, char* out, size_t outsz) {
    if (s->interactive) {
        write(1, "> ", 2);
        if (read_line() < 0) return -1;
        strncpy(out, ed_buf, outsz - 1);
        out[outsz - 1] = '\0';
        return 0;
    }
    if (s->idx >= s->nlines) return -1;
    strncpy(out, s->lines[s->idx++], outsz - 1);
    out[outsz - 1] = '\0';
    return 0;
}

static void src_init_array(line_src_t* s, char** lines, int nlines) {
    s->interactive = 0;
    s->lines = lines;
    s->nlines = nlines;
    s->idx = 0;
}

static void src_init_interactive(line_src_t* s) {
    s->interactive = 1;
    s->lines = 0;
    s->nlines = 0;
    s->idx = 0;
}

static const char* const SH_TERM_FI[]   = { "fi" };
static const char* const SH_TERM_DONE[] = { "done" };

/* nesting-depth-indexed scratch: capture_until's callers pass a row of
 * this so a recursive control-flow parse never puts a 64*256-byte buffer
 * on the 8KB user stack. Declared here since both this file's capture
 * call sites (Task 6/7) and this definition need the same storage. */
static char block_scratch[SH_NEST_MAX][SH_BLOCK_LINES_MAX][SH_LINE_MAX];
static int  block_depth;

static int opens_block(const char* word) {
    return strcmp(word, "if") == 0 || strcmp(word, "while") == 0 || strcmp(word, "for") == 0;
}

/* Reads raw lines from `src` into `stored` (capped at SH_BLOCK_LINES_MAX),
 * tracking nesting depth by counting first-word if/while/for opens against
 * standalone fi/done closes. Stops once depth is back to 0 and the line's
 * first word exactly matches one of `terms[nterms]`; that terminator line
 * itself is written to `matched_line` (pass 0/0 if the caller doesn't need
 * it) and its index into `terms[]` is returned. Returns -1 on EOF or if
 * the block overflows SH_BLOCK_LINES_MAX (both are "missing fi/done" —
 * caller reports the error). The matched line is NOT included in `stored`. */
static int capture_until(line_src_t* src, const char* const terms[], int nterms,
                          char stored[][SH_LINE_MAX], int* n_stored,
                          char* matched_line, size_t matched_line_sz) {
    int depth = 0;
    *n_stored = 0;
    char line[SH_LINE_MAX];

    for (;;) {
        if (src_next(src, line, sizeof(line)) < 0) return -1;

        const char* p = line;
        while (*p == ' ') p++;
        char word[16];
        size_t wn = 0;
        while (*p && *p != ' ' && wn < sizeof(word) - 1) word[wn++] = *p++;
        word[wn] = '\0';

        if (depth == 0) {
            for (int t = 0; t < nterms; t++) {
                if (strcmp(word, terms[t]) == 0) {
                    if (matched_line) {
                        strncpy(matched_line, line, matched_line_sz - 1);
                        matched_line[matched_line_sz - 1] = '\0';
                    }
                    return t;
                }
            }
        }

        if (opens_block(word)) depth++;
        else if (strcmp(word, "fi") == 0 || strcmp(word, "done") == 0) depth--;

        if (*n_stored >= SH_BLOCK_LINES_MAX) return -1;
        strncpy(stored[*n_stored], line, SH_LINE_MAX - 1);
        stored[*n_stored][SH_LINE_MAX - 1] = '\0';
        (*n_stored)++;
    }
}

/* ---- if/elif/else/fi control flow --------------------------------------- */

typedef enum { STMT_NORMAL, STMT_BREAK, STMT_CONTINUE } stmt_sig_t;

/* Finds "; <tail>" in `line` starting at `line + skip`, copies everything
 * between them (trimmed) into `out`. Returns 0 on success, -1 if the
 * separator isn't found (caller reports "expected '; then'" etc). */
static int extract_header(const char* line, size_t skip, const char* tail,
                           char* out, size_t outsz) {
    const char* p = line + skip;
    while (*p == ' ') p++;

    char needle[16];
    needle[0] = ';';
    needle[1] = ' ';
    strcpy(needle + 2, tail);
    const char* sep = strstr(p, needle);
    if (!sep) return -1;

    size_t n = (size_t)(sep - p);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    while (n > 0 && out[n - 1] == ' ') out[--n] = '\0';
    return 0;
}

static stmt_sig_t exec_block(line_src_t* src); /* forward decl: run_if calls it */

static stmt_sig_t run_if(line_src_t* src, const char* header) {
    size_t skip = (strncmp(header, "elif", 4) == 0) ? 4 : 2; /* "elif" vs "if" */
    char cond[SH_LINE_MAX];
    if (extract_header(header, skip, "then", cond, sizeof(cond)) < 0) {
        printf("sh: if: expected '; then'\n");
        return STMT_NORMAL;
    }

    char linebuf[SH_LINE_MAX];
    strcpy(linebuf, cond);
    int status = exec_line(linebuf);

    static const char* const terms[] = { "elif", "else", "fi" };
    int n_stored = 0;
    char matched[SH_LINE_MAX];
    int which = capture_until(src, terms, 3, block_scratch[block_depth], &n_stored,
                               matched, sizeof(matched));
    if (which < 0) { printf("sh: if: missing fi\n"); return STMT_NORMAL; }

    if (status == 0) {
        stmt_sig_t sig = STMT_NORMAL;
        if (block_depth + 1 >= SH_NEST_MAX) {
            printf("sh: too deeply nested\n");
        } else {
            char* ptrs[SH_BLOCK_LINES_MAX];
            for (int i = 0; i < n_stored; i++) ptrs[i] = block_scratch[block_depth][i];
            line_src_t body;
            src_init_array(&body, ptrs, n_stored);
            block_depth++;
            sig = exec_block(&body);
            block_depth--;
        }
        if (which != 2) { /* not already at fi: discard the unused elif/else..fi */
            int dn = 0;
            capture_until(src, SH_TERM_FI, 1, block_scratch[block_depth], &dn, 0, 0);
        }
        return sig;
    }

    if (which == 2) return STMT_NORMAL; /* condition false, no elif/else */
    if (which == 0) return run_if(src, matched); /* elif: recurse on its own condition */

    /* which == 1: else */
    if (block_depth + 1 >= SH_NEST_MAX) { printf("sh: too deeply nested\n"); return STMT_NORMAL; }
    int en = 0;
    int w2 = capture_until(src, SH_TERM_FI, 1, block_scratch[block_depth], &en, 0, 0);
    if (w2 < 0) { printf("sh: if: missing fi\n"); return STMT_NORMAL; }
    char* ptrs2[SH_BLOCK_LINES_MAX];
    for (int i = 0; i < en; i++) ptrs2[i] = block_scratch[block_depth][i];
    line_src_t body2;
    src_init_array(&body2, ptrs2, en);
    block_depth++;
    stmt_sig_t sig2 = exec_block(&body2);
    block_depth--;
    return sig2;
}

static stmt_sig_t exec_block(line_src_t* src) {
    for (;;) {
        char line[SH_LINE_MAX];
        if (src_next(src, line, sizeof(line)) < 0) return STMT_NORMAL; /* EOF: end of block */

        char* p = line;
        while (*p == ' ') p++;
        if (*p == '\0' || *p == '#') continue; /* blank line / comment */

        char word[16];
        size_t wn = 0;
        const char* q = p;
        while (*q && *q != ' ' && wn < sizeof(word) - 1) word[wn++] = *q++;
        word[wn] = '\0';

        if (strcmp(word, "if") == 0) {
            stmt_sig_t s = run_if(src, p);
            if (s != STMT_NORMAL) return s;
            continue;
        }

        exec_line(p);
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
    printf("builtins: cd [path], exit, help, jobs, fg [pgid], bg [pgid], test/[ ...\n");
    printf("pipelines: a | b | c (max %d stages), < in, > out, >> append, & background\n",
           SH_PIPE_MAX_STAGES);
    printf("everything else runs from /bin (then /bin/test)\n");
}

/* test / [ — numeric (-eq -ne -lt -le -gt -ge), string (= !=), and file
 * (-f -d -e) checks. When invoked as `[`, the last argument must be `]`. */
static int builtin_test(int argc, char** argv) {
    if (strcmp(argv[0], "[") == 0) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            printf("sh: [: missing closing ]\n");
            return 1;
        }
        argc--;
    }

    int n = argc - 1; /* operand count, argv[0] is "test" or "[" */

    if (n == 1) return (*argv[1] != '\0') ? 0 : 1;

    if (n == 2) {
        struct stat st;
        int found = (stat(argv[2], &st) == 0);
        if (strcmp(argv[1], "-f") == 0) return (found && !(st.type & DT_DIR)) ? 0 : 1;
        if (strcmp(argv[1], "-d") == 0) return (found && (st.type & DT_DIR)) ? 0 : 1;
        if (strcmp(argv[1], "-e") == 0) return found ? 0 : 1;
        printf("sh: test: unknown unary operator: %s\n", argv[1]);
        return 1;
    }

    if (n == 3) {
        const char* lhs = argv[1];
        const char* op  = argv[2];
        const char* rhs = argv[3];
        if (strcmp(op, "=") == 0)  return (strcmp(lhs, rhs) == 0) ? 0 : 1;
        if (strcmp(op, "!=") == 0) return (strcmp(lhs, rhs) != 0) ? 0 : 1;
        int l = atoi(lhs), r = atoi(rhs);
        if (strcmp(op, "-eq") == 0) return (l == r) ? 0 : 1;
        if (strcmp(op, "-ne") == 0) return (l != r) ? 0 : 1;
        if (strcmp(op, "-lt") == 0) return (l <  r) ? 0 : 1;
        if (strcmp(op, "-le") == 0) return (l <= r) ? 0 : 1;
        if (strcmp(op, "-gt") == 0) return (l >  r) ? 0 : 1;
        if (strcmp(op, "-ge") == 0) return (l >= r) ? 0 : 1;
        printf("sh: test: unknown binary operator: %s\n", op);
        return 1;
    }

    printf("sh: test: bad number of arguments\n");
    return 1;
}

int main(void) {
    setsid();
    signal(SIGINT, SIG_IGN);
    tcsetpgrp(0, getpid());
    tty_set_canonical(0, 0); /* kernel default is canonical; sh does its own line editing */
    history_load();

    for (;;) {
        jobs_reap();
        print_prompt();
        if (read_line() < 0) { hist_pos = hist_len; continue; }
        history_add(ed_buf);

        char line[SH_LINE_MAX];
        strcpy(line, ed_buf);

        char* p = line;
        while (*p == ' ') p++;
        char word[16];
        size_t wn = 0;
        const char* q = p;
        while (*q && *q != ' ' && wn < sizeof(word) - 1) word[wn++] = *q++;
        word[wn] = '\0';

        if (strcmp(word, "if") == 0) {
            line_src_t top;
            src_init_interactive(&top);
            run_if(&top, p);
        } else {
            exec_line(line);
        }
    }
}
