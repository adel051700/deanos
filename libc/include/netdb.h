#ifndef _NETDB_H
#define _NETDB_H 1

#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EAI_OK          0
#define EAI_FAIL       -1
#define EAI_NONAME     -2
#define EAI_AGAIN      -3
#define EAI_MEMORY     -4
#define EAI_BADFLAGS   -5

struct hostent {
    char* h_name;
    char** h_aliases;
    int h_addrtype;
    int h_length;
    char** h_addr_list;
};

#define h_addr h_addr_list[0]

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    size_t ai_addrlen;
    struct sockaddr_in* ai_addr;
    char* ai_canonname;
    struct addrinfo* ai_next;
};

int resolve(const char* hostname, struct in_addr* out, unsigned timeout_ms);
struct hostent* gethostbyname(const char* name);
int getaddrinfo(const char* node,
                const char* service,
                const struct addrinfo* hints,
                struct addrinfo** res);
void freeaddrinfo(struct addrinfo* res);

#ifdef __cplusplus
}
#endif

#endif /* _NETDB_H */
