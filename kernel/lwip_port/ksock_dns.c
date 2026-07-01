#include "ksock_dns.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "kernel/net_lwip.h"
#include "kernel/task.h"
#include "kernel/pit.h"
#include <string.h>

typedef struct { volatile int done; volatile int ok; ip_addr_t addr; } dns_wait_t;

static void dns_cb(const char* name, const ip_addr_t* ipaddr, void* arg) {
    dns_wait_t* w = (dns_wait_t*)arg;
    (void)name;
    if (ipaddr) { w->addr = *ipaddr; w->ok = 1; }
    w->done = 1;
}

int ksock_dns_query_a(const char* host, uint8_t out_ip[4], uint32_t timeout_ms) {
    dns_wait_t w; ip_addr_t resolved; err_t e; uint32_t f;
    uint32_t start = (uint32_t)pit_get_uptime_ms();
    memset(&w, 0, sizeof(w));

    f = net_lock();
    e = dns_gethostbyname(host, &resolved, dns_cb, &w);
    net_unlock(f);

    if (e == ERR_OK) {                 /* cached */
        uint32_t v = ip4_addr_get_u32(ip_2_ip4(&resolved));
        out_ip[0]=(uint8_t)v; out_ip[1]=(uint8_t)(v>>8); out_ip[2]=(uint8_t)(v>>16); out_ip[3]=(uint8_t)(v>>24);
        return 0;
    }
    if (e != ERR_INPROGRESS) return -1;

    for (;;) {
        int done;
        f = net_lock();
        done = w.done;
        net_unlock(f);
        if (done) break;
        if (timeout_ms && (uint32_t)pit_get_uptime_ms() - start >= timeout_ms) return -1;
        net_service_tick();
        task_yield();
    }

    f = net_lock();
    {
        int ok = w.ok;
        uint32_t v = ip4_addr_get_u32(ip_2_ip4(&w.addr));
        net_unlock(f);
        if (!ok) return -1;
        out_ip[0]=(uint8_t)v; out_ip[1]=(uint8_t)(v>>8); out_ip[2]=(uint8_t)(v>>16); out_ip[3]=(uint8_t)(v>>24);
    }
    return 0;
}
