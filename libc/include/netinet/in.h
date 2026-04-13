#ifndef _NETINET_IN_H
#define _NETINET_IN_H 1

#include <stdint.h>

#define AF_INET 2
#define IPPROTO_UDP 17

struct in_addr {
    uint8_t s_addr[4];
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
};

#endif /* _NETINET_IN_H */

