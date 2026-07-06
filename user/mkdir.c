#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: mkdir <path>\n");
        return 1;
    }

    const char* path = argv[1];

    struct stat st;
    if (stat(path, &st) == 0) {
        printf("mkdir: already exists: %s\n", path);
        return 1;
    }

    if (mkdir(path) < 0) {
        printf("mkdir: failed to create directory\n");
        return 1;
    }
    return 0;
}
