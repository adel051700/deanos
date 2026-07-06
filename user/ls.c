#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define LS_MAX_DEPTH 3
#define LS_PATH_MAX 256

static void join_path(const char* dir_path, const char* name, char* out, size_t out_sz) {
    if (strcmp(dir_path, "/") == 0) {
        strcpy(out, "/");
        strncat(out, name, out_sz - strlen(out) - 1);
    } else {
        strncpy(out, dir_path, out_sz - 1);
        out[out_sz - 1] = '\0';
        strncat(out, "/", out_sz - strlen(out) - 1);
        strncat(out, name, out_sz - strlen(out) - 1);
    }
}

static uint32_t count_children(const char* path) {
    struct dirent tmp;
    uint32_t n = 0;
    while (dir_read(path, n, &tmp) == 0) n++;
    return n;
}

static void print_file_info(const char* dir_path, const char* name) {
    printf("[FILE] %s", name);
    char child_path[LS_PATH_MAX];
    join_path(dir_path, name, child_path, sizeof(child_path));
    struct stat st;
    if (stat(child_path, &st) == 0) {
        printf("  (%d bytes)", (int)st.size);
    }
    printf("\n");
}

static void print_prefix(int depth, int is_last) {
    for (int i = 0; i < depth; i++) {
        if (i == 0) printf("  ");
        else printf("     ");
    }
    if (depth > 0) {
        if (is_last) printf("|_ ");
        else printf("|  ");
    }
}

static void ls_recursive(const char* dir_path, int depth) {
    if (depth > LS_MAX_DEPTH) return;

    uint32_t total = count_children(dir_path);
    if (total == 0) {
        if (depth == 0) printf("  (empty)\n");
        return;
    }

    struct dirent ent;
    uint32_t idx = 0;
    while (dir_read(dir_path, idx, &ent) == 0) {
        int is_last = (idx == total - 1);
        int is_dir  = (ent.type & DT_DIR);

        print_prefix(depth, is_last);

        if (is_dir) {
            printf("[DIR] %s\n", ent.name);

            char child_path[LS_PATH_MAX];
            join_path(dir_path, ent.name, child_path, sizeof(child_path));
            ls_recursive(child_path, depth + 1);
        } else {
            print_file_info(dir_path, ent.name);
        }
        idx++;
    }
}

int main(int argc, char** argv) {
    /* path is echoed raw (unnormalized) in the error messages below, unlike
     * the retired cmd_ls builtin which echoed vfs_normalize_path's output.
     * No realpath()-equivalent syscall is exposed to userspace, so this
     * only differs from the builtin for non-canonical input (e.g. a
     * trailing slash or "a/../b") — the file's actual resolution is still
     * correct either way, since dir_read()/stat() normalize server-side. */
    const char* path = (argc > 1) ? argv[1] : "";

    struct stat st;
    if (stat(path, &st) != 0) {
        printf("ls: no such directory: %s\n", path);
        return 1;
    }
    if (!(st.type & DT_DIR)) {
        printf("ls: not a directory: %s\n", path);
        return 1;
    }

    ls_recursive(path, 0);
    return 0;
}
