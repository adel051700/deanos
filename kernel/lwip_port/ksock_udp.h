#ifndef KSOCK_UDP_H
#define KSOCK_UDP_H
#include <stdint.h>

typedef struct ksock_udp ksock_udp_t;

ksock_udp_t* ksock_udp_open(void);
int  ksock_udp_bind(ksock_udp_t*, uint16_t local_port);
int  ksock_udp_sendto(ksock_udp_t*, const uint8_t ip[4], uint16_t port,
                      const void* buf, uint16_t len);
int  ksock_udp_recvfrom(ksock_udp_t*, void* buf, uint16_t cap, uint16_t* out_len,
                        uint8_t out_ip[4], uint16_t* out_port,
                        uint32_t timeout_ms, int nonblock);
int  ksock_udp_readable(ksock_udp_t*);
void ksock_udp_close(ksock_udp_t*);

#endif
