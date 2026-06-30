#ifndef DEANOS_NETIF_H
#define DEANOS_NETIF_H

#include "lwip/netif.h"
#include "lwip/err.h"

err_t deanos_netif_init(struct netif* netif);
int   deanos_netif_bind_driver(void);   /* 0 ok, <0 no NIC */
void  net_lwip_rx_pump(void);
struct netif* deanos_netif_default(void);
const char* deanos_netif_driver_name(void);

#endif
