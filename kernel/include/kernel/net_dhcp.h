#ifndef _KERNEL_NET_DHCP_H
#define _KERNEL_NET_DHCP_H

#include <stdint.h>

int net_dhcp_parse_options(const uint8_t* pkt,
                           uint16_t len,
                           uint8_t* out_msg_type,
                           uint8_t out_server_ip[4],
                           uint8_t out_mask[4],
                           uint8_t out_gw[4],
                           uint32_t* out_lease_s,
                           uint32_t* out_t1_s,
                           uint32_t* out_t2_s);

#endif

