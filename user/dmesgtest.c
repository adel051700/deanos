#include <fcntl.h>
#include <unistd.h>
#include <sys/klog.h>

static void write_str(int fd, const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    write(fd, s, len);
}

static void write_int(int fd, int v) {
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    int neg = v < 0;
    unsigned uv = neg ? (unsigned)(-(v + 1)) + 1u : (unsigned)v;
    do {
        buf[--i] = (char)('0' + (uv % 10u));
        uv /= 10u;
    } while (uv != 0u);
    if (neg) buf[--i] = '-';
    write_str(fd, &buf[i]);
}

int main(void) {
    int fd = open("/dmesgtest.out", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 1;

    static char klogbuf[KLOG_BUF_SZ];
    int n1 = dmesg_read(klogbuf, sizeof(klogbuf));
    write_str(fd, "dmesg_read_n1_positive=");
    write_int(fd, (n1 > 0) ? 1 : 0);
    write_str(fd, "\n");

    int clear_rc = dmesg_clear();
    write_str(fd, "dmesg_clear_rc=");
    write_int(fd, clear_rc);
    write_str(fd, "\n");

    int n2 = dmesg_read(klogbuf, sizeof(klogbuf));
    write_str(fd, "dmesg_read_n2=");
    write_int(fd, n2);
    write_str(fd, "\n");

    close(fd);
    return 0;
}
