/* /bin/net — network status/ping/dns/http tool, ported from the
 * kernel-shell builtin (see docs/superpowers/specs/2026-07-14-kernel-shell-retirement-design.md). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/net.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

static int is_decimal(const char* s) {
    if (!s || !*s) return 0;
    for (const char* p = s; *p; ++p) if (*p < '0' || *p > '9') return 0;
    return 1;
}

static void print_hex_byte(unsigned v) {
    static const char* hex = "0123456789ABCDEF";
    char buf[3];
    buf[0] = hex[(v >> 4) & 0xF];
    buf[1] = hex[v & 0xF];
    buf[2] = '\0';
    printf("%s", buf);
}

static void print_ipv4(const uint8_t ip[4]) {
    printf("%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
}

static void print_info(void) {
    struct net_info info;
    if (net_info(&info) != 0) {
        printf("net: info query failed\n");
        return;
    }

    printf("net: driver=%s link=%s\n", info.driver_name, info.ready ? "up" : "down");

    printf("mac: ");
    for (int i = 0; i < 6; ++i) {
        print_hex_byte(info.mac[i]);
        if (i < 5) printf(":");
    }
    printf("\n");

    printf("inet=");
    print_ipv4(info.ip);
    printf(" mask=");
    print_ipv4(info.netmask);
    printf(" gw=");
    print_ipv4(info.gateway);
    printf("\n");
}

static void cmd_ping(int argc, char** argv) {
    /* argv[0]=="ping", argv[1]=host, then [count] [-c count] [-W timeout] */
    if (argc < 2) {
        printf("usage: net ping <host|a.b.c.d> [count] [-c count] [-W timeout_ms]\n");
        return;
    }
    const char* host = argv[1];
    unsigned count = 1u;
    unsigned timeout_ms = 1000u;
    int saw_pos_count = 0;

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc || !is_decimal(argv[i + 1])) {
                printf("usage: net ping <host|a.b.c.d> [count] [-c count] [-W timeout_ms]\n");
                return;
            }
            count = (unsigned)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-W") == 0) {
            if (i + 1 >= argc || !is_decimal(argv[i + 1])) {
                printf("usage: net ping <host|a.b.c.d> [count] [-c count] [-W timeout_ms]\n");
                return;
            }
            timeout_ms = (unsigned)atoi(argv[++i]);
        } else if (!saw_pos_count && is_decimal(argv[i])) {
            count = (unsigned)atoi(argv[i]);
            saw_pos_count = 1;
        } else {
            printf("usage: net ping <host|a.b.c.d> [count] [-c count] [-W timeout_ms]\n");
            return;
        }
    }

    if (count == 0u) count = 1u;
    if (count > 32u) count = 32u;
    if (timeout_ms == 0u) timeout_ms = 1u;
    if (timeout_ms > 10000u) timeout_ms = 10000u;

    struct in_addr addr;
    if (resolve(host, &addr, 5000u) != 0) {
        printf("ping: dns lookup failed host=%s\n", host);
        return;
    }
    uint8_t ip[4] = { addr.s_addr[0], addr.s_addr[1], addr.s_addr[2], addr.s_addr[3] };

    printf("PING %s (", host);
    print_ipv4(ip);
    printf("): %d probe(s), timeout=%dms\n", (int)count, (int)timeout_ms);

    unsigned sent = 0, recv_ok = 0;

    /* net_ping() only reports success/failure, not elapsed time (the
     * builtin timed the call itself with pit_get_uptime_ms() around the
     * kernel-internal call — no equivalent is exposed to userspace, and
     * adding one is out of scope per the spec's "no new syscalls"
     * constraint). Every successful reply is reported as 0ms; accepted
     * parity gap, not a bug. */
    for (unsigned i = 0; i < count; ++i) {
        int rc = net_ping(ip, (unsigned short)(i + 1u), timeout_ms);
        sent++;
        if (rc == 0) {
            recv_ok++;
            printf("reply from ");
            print_ipv4(ip);
            printf(": seq=%d time=0ms\n", (int)(i + 1u));
        } else {
            printf("timeout: seq=%d\n", (int)(i + 1u));
        }
    }

    unsigned loss = (sent > recv_ok) ? (unsigned)(((sent - recv_ok) * 100u) / sent) : 0u;
    printf("ping stats: sent=%d recv=%d loss=%d%%\n", (int)sent, (int)recv_ok, (int)loss);
    if (recv_ok > 0u) {
        printf("rtt min/avg/max=0/0/0 ms\n");
    }
}

static void cmd_dns(int argc, char** argv) {
    if (argc < 2) { printf("usage: net dns <hostname>\n"); return; }
    struct in_addr addr;
    if (resolve(argv[1], &addr, 5000u) == 0) {
        printf("dns: %s -> ", argv[1]);
        uint8_t ip[4] = { addr.s_addr[0], addr.s_addr[1], addr.s_addr[2], addr.s_addr[3] };
        print_ipv4(ip);
        printf("\n");
    } else {
        printf("dns: lookup failed host=%s\n", argv[1]);
    }
}

static void cmd_tcp_http(int argc, char** argv) {
    /* argv[0]=="tcp", argv[1]=="http", argv[2]=host, argv[3]=port, argv[4]=path */
    if (argc < 5 || strcmp(argv[1], "http") != 0) {
        printf("usage: net tcp http <host|a.b.c.d> <port> <path>\n");
        return;
    }
    const char* host = argv[2];
    const char* port_tok = argv[3];
    const char* path = argv[4];

    if (!is_decimal(port_tok)) {
        printf("usage: net tcp http <host|a.b.c.d> <port> <path>\n");
        return;
    }
    int port = atoi(port_tok);
    if (port <= 0 || port > 65535) {
        printf("net tcp http: invalid port\n");
        return;
    }

    struct in_addr addr;
    if (resolve(host, &addr, 5000u) != 0) {
        printf("net tcp http: dns lookup failed host=%s\n", host);
        return;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { printf("net tcp http: connect failed\n"); return; }

    struct sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port = (uint16_t)port;
    sa.sin_addr = addr;
    if (connect(sock, &sa, sizeof(sa)) != 0) {
        printf("net tcp http: connect failed\n");
        close(sock);
        return;
    }

    char req[320];
    strcpy(req, "GET ");
    if (path[0] != '/') strcat(req, "/");
    strncat(req, path, sizeof(req) - strlen(req) - 1u);
    strncat(req, " HTTP/1.0\r\nHost: ", sizeof(req) - strlen(req) - 1u);
    strncat(req, host, sizeof(req) - strlen(req) - 1u);
    strncat(req, "\r\n\r\n", sizeof(req) - strlen(req) - 1u);

    if (send(sock, req, strlen(req), 0) < 0) {
        printf("net tcp http: send failed\n");
        close(sock);
        return;
    }

    printf("net tcp http: response begin\n");
    unsigned total = 0;
    for (;;) {
        char rx[256];
        int rc = recv(sock, rx, sizeof(rx), 0);
        if (rc > 0) {
            write(1, rx, (size_t)rc);
            total += (unsigned)rc;
            continue;
        }
        if (rc == 0) break;      /* peer closed */
        if (rc == -8) break;     /* NET_TCP_ERR_WOULD_BLOCK: idle timeout */
        printf("\nnet tcp http: recv failed\n");
        break;
    }
    close(sock);
    printf("\nnet tcp http: done bytes=%d\n", (int)total);
}

static void cmd_dhcp(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "renew") == 0) {
        if (net_dhcp_renew() != 0) { printf("net dhcp renew: failed to start\n"); return; }
        printf("net dhcp renew: requested\n");
        return;
    }

    struct net_info info;
    if (net_info(&info) != 0 || !info.ready) {
        printf("net dhcp: NIC not ready\n");
        return;
    }
    printf("net dhcp: lease ip=");
    print_ipv4(info.ip);
    printf(" mask=");
    print_ipv4(info.netmask);
    printf(" gw=");
    print_ipv4(info.gateway);
    printf("\n");
}

int main(int argc, char** argv) {
    if (argc < 2 || strcmp(argv[1], "ip") == 0) { print_info(); return 0; }
    if (strcmp(argv[1], "ping") == 0) { cmd_ping(argc - 1, argv + 1); return 0; }
    if (strcmp(argv[1], "dns") == 0) { cmd_dns(argc - 1, argv + 1); return 0; }
    if (strcmp(argv[1], "tcp") == 0) { cmd_tcp_http(argc - 1, argv + 1); return 0; }
    if (strcmp(argv[1], "dhcp") == 0) { cmd_dhcp(argc - 1, argv + 1); return 0; }

    printf("usage: net [ip] | net ping <host|a.b.c.d> [count] [-c count] [-W timeout_ms] | "
           "net dns <hostname> | net tcp http <host|a.b.c.d> <port> <path> | net dhcp [renew]\n");
    return 1;
}
