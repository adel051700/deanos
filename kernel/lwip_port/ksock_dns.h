#ifndef KSOCK_DNS_H
#define KSOCK_DNS_H
#include <stdint.h>
int ksock_dns_query_a(const char* host, uint8_t out_ip[4], uint32_t timeout_ms);
#endif
