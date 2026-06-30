#include "ksock_udp.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "kernel/net_lwip.h"
#include "kernel/kheap.h"
#include "kernel/task.h"
#include "kernel/pit.h"
#include <string.h>

#define UDP_RXQ 16
typedef struct { uint8_t ip[4]; uint16_t port, len; uint8_t data[1472]; } udp_dgram_t;

struct ksock_udp {
    struct udp_pcb* pcb;
    udp_dgram_t q[UDP_RXQ];
    uint32_t head, tail;   /* head=produce (callback), tail=consume */
};

static void udp_rx_cb(void* arg, struct udp_pcb* pcb, struct pbuf* p,
                      const ip_addr_t* addr, u16_t port) {
    struct ksock_udp* s = (struct ksock_udp*)arg;
    uint32_t next = (s->head + 1u) % UDP_RXQ;
    (void)pcb;
    if (p && next != s->tail && p->tot_len <= sizeof(s->q[0].data)) {
        udp_dgram_t* d = &s->q[s->head];
        uint32_t v = ip4_addr_get_u32(ip_2_ip4(addr));
        d->ip[0]=(uint8_t)v; d->ip[1]=(uint8_t)(v>>8); d->ip[2]=(uint8_t)(v>>16); d->ip[3]=(uint8_t)(v>>24);
        d->port = port; d->len = p->tot_len;
        pbuf_copy_partial(p, d->data, p->tot_len, 0);
        s->head = next;
    }
    if (p) pbuf_free(p);
}

ksock_udp_t* ksock_udp_open(void) {
    struct ksock_udp* s = (struct ksock_udp*)kmalloc(sizeof(*s));
    if (!s) return 0;
    memset(s, 0, sizeof(*s));
    s->pcb = udp_new();
    if (!s->pcb) { kfree(s); return 0; }
    udp_recv(s->pcb, udp_rx_cb, s);
    return s;
}

int ksock_udp_bind(ksock_udp_t* s, uint16_t port) {
    if (!s) return -1;
    return udp_bind(s->pcb, IP_ANY_TYPE, port) == ERR_OK ? 0 : -1;
}

int ksock_udp_sendto(ksock_udp_t* s, const uint8_t ip[4], uint16_t port,
                     const void* buf, uint16_t len) {
    ip_addr_t dst; struct pbuf* p; err_t e;
    if (!s) return -1;
    IP_ADDR4(&dst, ip[0], ip[1], ip[2], ip[3]);
    p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (!p) return -1;
    pbuf_take(p, buf, len);
    e = udp_sendto(s->pcb, p, &dst, port);
    pbuf_free(p);
    return e == ERR_OK ? (int)len : -1;
}

int ksock_udp_readable(ksock_udp_t* s) { return s && s->head != s->tail; }

int ksock_udp_recvfrom(ksock_udp_t* s, void* buf, uint16_t cap, uint16_t* out_len,
                       uint8_t out_ip[4], uint16_t* out_port,
                       uint32_t timeout_ms, int nonblock) {
    uint32_t start = (uint32_t)pit_get_uptime_ms();
    if (!s) return -1;
    for (;;) {
        if (s->head != s->tail) {
            udp_dgram_t* d = &s->q[s->tail];
            uint16_t n = d->len < cap ? d->len : cap;
            memcpy(buf, d->data, n);
            if (out_len) *out_len = n;
            if (out_ip) memcpy(out_ip, d->ip, 4);
            if (out_port) *out_port = d->port;
            s->tail = (s->tail + 1u) % UDP_RXQ;
            return (int)n;
        }
        if (nonblock) return 0;
        if (timeout_ms && (uint32_t)pit_get_uptime_ms() - start >= timeout_ms) return 0;
        net_service_tick();
        task_yield();
    }
}

void ksock_udp_close(ksock_udp_t* s) {
    if (!s) return;
    if (s->pcb) udp_remove(s->pcb);
    kfree(s);
}
