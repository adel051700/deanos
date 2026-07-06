#ifndef _SYS_NET_H
#define _SYS_NET_H 1
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

struct net_info {
    uint8_t  mac[6];
    uint8_t  ip[4];
    uint8_t  netmask[4];
    uint8_t  gateway[4];
    char     driver_name[16];
    int32_t  ready;
};

int net_info(struct net_info* out);
int net_ping(const uint8_t ip[4], unsigned short seq, unsigned timeout_ms);
int net_dhcp_renew(void);

#ifdef __cplusplus
}
#endif
#endif /* _SYS_NET_H */
