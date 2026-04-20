#ifndef _KERNEL_NET_DNS_H
#define _KERNEL_NET_DNS_H

#include <stdint.h>

int net_dns_encode_qname(const char* hostname, uint8_t* out, uint16_t out_capacity, uint16_t* out_len);
int net_dns_skip_name(const uint8_t* msg, uint16_t msg_len, uint16_t start_off, uint16_t* out_next);

#endif

