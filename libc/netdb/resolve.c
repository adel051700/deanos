#include <kernel/syscall.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline long __nd_syscall1(unsigned num, unsigned a1) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a1) : "memory");
    return ret;
}

static int parse_ipv4(const char* s, uint8_t out[4]) {
    uint32_t parts[4] = {0, 0, 0, 0};
    int idx = 0;
    int digits = 0;
    if (!s) return -1;
    for (const char* p = s;; ++p) {
        if (*p >= '0' && *p <= '9') {
            parts[idx] = parts[idx] * 10u + (uint32_t)(*p - '0');
            if (parts[idx] > 255u) return -1;
            digits++;
        } else if (*p == '.') {
            if (!digits || idx == 3) return -1;
            idx++;
            digits = 0;
        } else if (*p == '\0') {
            if (!digits || idx != 3) return -1;
            break;
        } else {
            return -1;
        }
    }
    for (int i = 0; i < 4; ++i) out[i] = (uint8_t)parts[i];
    return 0;
}

int resolve(const char* hostname, struct in_addr* out, unsigned timeout_ms) {
    syscall_resolve_args_t args;
    uint8_t tmp[4];
    if (!hostname || !out) return -1;

    if (parse_ipv4(hostname, tmp) == 0) {
        out->s_addr[0] = tmp[0];
        out->s_addr[1] = tmp[1];
        out->s_addr[2] = tmp[2];
        out->s_addr[3] = tmp[3];
        return 0;
    }

    args.hostname = hostname;
    args.out_ip = tmp;
    args.dns_server_ip = 0;
    args.timeout_ms = timeout_ms;
    long rc = __nd_syscall1(SYS_resolve, (unsigned)&args);
    if (rc != 0) return (int)rc;
    out->s_addr[0] = tmp[0];
    out->s_addr[1] = tmp[1];
    out->s_addr[2] = tmp[2];
    out->s_addr[3] = tmp[3];
    return 0;
}

static char __h_name_buf[256];
static uint8_t __h_addr_buf[4];
static char* __h_addr_list[2];
static char* __h_aliases[1] = {0};
static struct hostent __h_entry;

struct hostent* gethostbyname(const char* name) {
    struct in_addr addr;
    size_t nl;
    if (!name) return 0;
    if (resolve(name, &addr, 0u) != 0) return 0;

    nl = strlen(name);
    if (nl >= sizeof(__h_name_buf)) nl = sizeof(__h_name_buf) - 1u;
    memcpy(__h_name_buf, name, nl);
    __h_name_buf[nl] = '\0';

    __h_addr_buf[0] = addr.s_addr[0];
    __h_addr_buf[1] = addr.s_addr[1];
    __h_addr_buf[2] = addr.s_addr[2];
    __h_addr_buf[3] = addr.s_addr[3];
    __h_addr_list[0] = (char*)__h_addr_buf;
    __h_addr_list[1] = 0;

    __h_entry.h_name = __h_name_buf;
    __h_entry.h_aliases = __h_aliases;
    __h_entry.h_addrtype = AF_INET;
    __h_entry.h_length = 4;
    __h_entry.h_addr_list = __h_addr_list;
    return &__h_entry;
}

static int parse_port(const char* s, uint16_t* out) {
    uint32_t v = 0;
    if (!s || !out) return -1;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10u + (uint32_t)(*p - '0');
        if (v > 65535u) return -1;
    }
    *out = (uint16_t)v;
    return 0;
}

int getaddrinfo(const char* node,
                const char* service,
                const struct addrinfo* hints,
                struct addrinfo** res) {
    struct in_addr addr;
    uint16_t port = 0;
    struct addrinfo* ai;
    struct sockaddr_in* sa;

    if (!res) return EAI_FAIL;
    *res = 0;
    if (!node && !service) return EAI_NONAME;
    if (hints && hints->ai_family != 0 && hints->ai_family != AF_INET) return EAI_BADFLAGS;

    if (service && parse_port(service, &port) != 0) return EAI_NONAME;

    if (node) {
        if (resolve(node, &addr, 0u) != 0) return EAI_NONAME;
    } else {
        addr.s_addr[0] = 0;
        addr.s_addr[1] = 0;
        addr.s_addr[2] = 0;
        addr.s_addr[3] = 0;
    }

    ai = (struct addrinfo*)malloc(sizeof(struct addrinfo));
    if (!ai) return EAI_MEMORY;
    sa = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
    if (!sa) {
        free(ai);
        return EAI_MEMORY;
    }

    memset(ai, 0, sizeof(*ai));
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = port;
    sa->sin_addr = addr;

    ai->ai_family = AF_INET;
    ai->ai_socktype = hints ? hints->ai_socktype : 0;
    ai->ai_protocol = hints ? hints->ai_protocol : 0;
    ai->ai_addrlen = sizeof(struct sockaddr_in);
    ai->ai_addr = sa;
    ai->ai_next = 0;
    ai->ai_canonname = 0;
    *res = ai;
    return EAI_OK;
}

void freeaddrinfo(struct addrinfo* res) {
    struct addrinfo* cur = res;
    while (cur) {
        struct addrinfo* next = cur->ai_next;
        if (cur->ai_addr) free(cur->ai_addr);
        if (cur->ai_canonname) free(cur->ai_canonname);
        free(cur);
        cur = next;
    }
}
