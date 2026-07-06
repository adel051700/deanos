#include <fcntl.h>
#include <unistd.h>
#include <sys/net.h>

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

static void write_field_int(int fd, const char* label, int v) {
    write_str(fd, label);
    write_int(fd, v);
    write_str(fd, "\n");
}

int main(void) {
    int fd = open("/nettest.out", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 1;

    struct net_info info;
    int info_rc = net_info(&info);
    write_field_int(fd, "info_rc=", info_rc);
    write_field_int(fd, "ready=", info.ready);

    write_str(fd, "driver_name=");
    write_str(fd, info.driver_name);
    write_str(fd, "\n");

    int gw_nonzero = (info.gateway[0] || info.gateway[1] || info.gateway[2] || info.gateway[3]);

    if (info.ready && gw_nonzero) {
        int ping_rc = net_ping(info.gateway, 1, 2000u);
        write_field_int(fd, "ping_rc=", ping_rc);
    } else {
        write_str(fd, "ping_test_skipped=1\n");
    }

    int dhcp_rc = net_dhcp_renew();
    write_field_int(fd, "dhcp_renew_rc=", dhcp_rc);

    close(fd);
    return 0;
}
