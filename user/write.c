#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: write <path> <text>\n");
        return 1;
    }

    const char* path = argv[1];

    char text[512];
    text[0] = '\0';
    for (int i = 2; i < argc; i++) {
        if (i > 2) strncat(text, " ", sizeof(text) - strlen(text) - 1);
        strncat(text, argv[i], sizeof(text) - strlen(text) - 1);
    }

    struct stat st;
    if (stat(path, &st) == 0 && (st.type & DT_DIR)) {
        printf("write: target is a directory: %s\n", path);
        printf("hint: use a file path, e.g. /mnt/hd0p1/test.txt\n");
        return 1;
    }

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC);
    if (fd < 0) {
        printf("write: cannot open: %s\n", path);
        return 1;
    }

    size_t tlen = strlen(text);
    ssize_t written = write(fd, text, tlen);
    close(fd);

    if (written < 0) {
        printf("write: write failed: %s\n", path);
        return 1;
    }

    printf("Wrote %d bytes to %s\n", (int)written, path);
    return 0;
}
