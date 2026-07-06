#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: rm <path>\n");
        return 1;
    }

    const char* path = argv[1];

    struct stat st;
    if (stat(path, &st) != 0) {
        printf("rm: no such file: %s\n", path);
        return 1;
    }

    if (strcmp(path, "/") == 0) {
        printf("rm: cannot remove root\n");
        return 1;
    }

    if (unlink(path) < 0) {
        if (st.type & DT_DIR) {
            printf("rm: failed (directory not empty?)\n");
        } else {
            printf("rm: failed\n");
        }
        return 1;
    }
    return 0;
}
