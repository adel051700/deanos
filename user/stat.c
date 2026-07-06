#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: stat <path>\n");
        return 1;
    }

    const char* path = argv[1];

    struct stat st;
    if (stat(path, &st) != 0) {
        printf("stat: not found: %s\n", path);
        return 1;
    }

    char modebuf[16];
    itoa((int)st.mode, modebuf, 8);

    printf("  path:  %s\n", path);
    printf("  inode: %d\n", (int)st.inode);
    printf("  type:  %s\n", (st.type & DT_DIR) ? "directory" : "file");
    printf("  size:  %d bytes\n", (int)st.size);
    printf("  mode:  0%s\n", modebuf);
    printf("  uid:   %d\n", (int)st.uid);
    printf("  gid:   %d\n", (int)st.gid);
    return 0;
}
