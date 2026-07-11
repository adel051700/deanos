/* dmesg — dump or clear the kernel log. Usage: dmesg [clear|-c] */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/klog.h>

int main(int argc, char** argv) {
    if (argc > 1 && (strcmp(argv[1], "clear") == 0 || strcmp(argv[1], "-c") == 0)) {
        dmesg_clear();
        printf("dmesg: log buffer cleared\n");
        return 0;
    }
    if (argc > 1) {
        printf("usage: dmesg [clear|-c]\n");
        return 1;
    }

    static char buf[KLOG_BUF_SZ];
    int n = dmesg_read(buf, sizeof(buf));
    if (n < 0) {
        printf("dmesg: read failed\n");
        return 1;
    }
    write(1, buf, (size_t)n);
    return 0;
}
