#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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

static void ls_dir(const char* dir_path) {
    struct dirent ent;
    uint32_t idx = 0;
    while (dir_read(dir_path, idx, &ent) == 0) {
        if (ent.type & DT_DIR) {
            printf("[DIR] %s\n", ent.name);
        } else {
            print_file_info(dir_path, ent.name);
        }
        idx++;
    }
    if (idx == 0) printf("  (empty)\n");
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

    ls_dir(path);
    return 0;
}
