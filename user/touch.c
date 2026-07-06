#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: touch <path>\n");
        return 1;
    }

    const char* path = argv[1];

    struct stat st;
    if (stat(path, &st) == 0) {
        printf("touch: already exists: %s\n", path);
        return 1;
    }

    int fd = open(path, O_CREAT | O_WRONLY);
    if (fd < 0) {
        printf("touch: failed to create file\n");
        return 1;
    }
    close(fd);
    return 0;
}
