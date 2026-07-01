#ifndef _KERNEL_NET_LWIP_H
#define _KERNEL_NET_LWIP_H
#include <stdint.h>

int  net_lwip_start(void);
void net_service_tick(void);
int  net_lwip_is_ready(void);
const char* net_lwip_driver_name(void);

/* net_lock()/net_unlock(): the single "big net lock" for this uniprocessor,
 * preemptive kernel. lwIP's raw API is not reentrant, and the PIT IRQ
 * (100 Hz, scheduler_tick -> net_service_tick) pumps RX + timeouts on top
 * of whatever a task is doing. Disabling local interrupts is a complete
 * critical section here: no IRQ (so no pump) and no task switch can occur
 * while held. EVERY direct lwIP raw-API call, and every read/write of
 * per-socket state shared with a pump-context callback, must be wrapped
 * with net_lock()/net_unlock(). NEVER hold this lock across task_yield()
 * or across a call to net_service_tick() -- release it first. */
uint32_t net_lock(void);
void net_unlock(uint32_t flags);

void net_lwip_get_mac(uint8_t out_mac[6]);
void net_lwip_get_ipv4(uint8_t out_ip[4]);
void net_lwip_get_ipv4_netmask(uint8_t out_mask[4]);
void net_lwip_get_ipv4_gateway(uint8_t out_gw[4]);

/* Minimal blocking ICMP echo (ping) over lwIP's raw API.
 * ip: destination IPv4 address (host byte order octets).
 * seq: echo sequence number to embed in the request (host byte order;
 *      converted to network order internally).
 * timeout_ms: 0 == wait forever; otherwise give up after this many ms.
 * Returns 0 if a matching echo reply was received, <0 on timeout/error. */
int net_lwip_ping(const uint8_t ip[4], uint16_t seq, uint32_t timeout_ms);

#endif
