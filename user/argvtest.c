#include <fcntl.h>
#include <unistd.h>

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
    unsigned uv = neg ? (unsigned)(-v) : (unsigned)v;
    do {
        buf[--i] = (char)('0' + (uv % 10u));
        uv /= 10u;
    } while (uv != 0u);
    if (neg) buf[--i] = '-';
    write_str(fd, &buf[i]);
}

int main(int argc, char** argv) {
    int fd = open("/argvtest.out", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 1;

    write_str(fd, "argc=");
    write_int(fd, argc);
    write_str(fd, "\n");

    for (int i = 0; i < argc; i++) {
        write_str(fd, "argv[");
        write_int(fd, i);
        write_str(fd, "]=");
        write_str(fd, argv[i]);
        write_str(fd, "\n");
    }

    close(fd);
    return 0;
}
