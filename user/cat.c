#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: cat <path>\n");
        return 1;
    }

    const char* path = argv[1];

    struct stat st;
    if (stat(path, &st) != 0) {
        printf("cat: no such file: %s\n", path);
        return 1;
    }
    if (!(st.type & DT_FILE)) {
        printf("cat: not a file: %s\n", path);
        return 1;
    }
    if (st.size == 0) {
        printf("(empty file)\n");
        return 0;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("cat: no such file: %s\n", path);
        return 1;
    }

    char buf[256];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(1, buf, (size_t)n);
    }
    close(fd);

    printf("\n");
    return 0;
}
