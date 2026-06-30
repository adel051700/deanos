#ifndef _KERNEL_NET_LWIP_H
#define _KERNEL_NET_LWIP_H
#include <stdint.h>

int  net_lwip_start(void);
void net_service_tick(void);
int  net_lwip_is_ready(void);
const char* net_lwip_driver_name(void);

void net_lwip_get_mac(uint8_t out_mac[6]);
void net_lwip_get_ipv4(uint8_t out_ip[4]);
void net_lwip_get_ipv4_netmask(uint8_t out_mask[4]);
void net_lwip_get_ipv4_gateway(uint8_t out_gw[4]);

#endif
