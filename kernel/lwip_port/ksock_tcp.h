#ifndef KSOCK_TCP_H
#define KSOCK_TCP_H
#include <stdint.h>

#define KSOCK_TCP_WOULDBLOCK (-11)

typedef struct ksock_tcp ksock_tcp_t;

ksock_tcp_t* ksock_tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms);
int  ksock_tcp_send(ksock_tcp_t*, const void* buf, uint16_t len, uint32_t timeout_ms, int nonblock);
int  ksock_tcp_recv(ksock_tcp_t*, void* buf, uint16_t cap, uint16_t* out_len, uint32_t timeout_ms, int nonblock);
int  ksock_tcp_readable(ksock_tcp_t*);
int  ksock_tcp_writable(ksock_tcp_t*);
void ksock_tcp_set_nodelay(ksock_tcp_t*, int on);
ksock_tcp_t* ksock_tcp_listen(uint16_t port, int backlog);
ksock_tcp_t* ksock_tcp_accept(ksock_tcp_t* listener, uint32_t timeout_ms, int nonblock);
int  ksock_tcp_peer(ksock_tcp_t*, uint8_t out_ip[4], uint16_t* out_port);
void ksock_tcp_close(ksock_tcp_t*);

#endif
