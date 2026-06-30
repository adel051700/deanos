#include "deanos_netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "lwip/ethip6.h"
#include "netif/ethernet.h"
#include "kernel/e1000.h"     /* found via -Ikernel/include */
#include "kernel/rtl8139.h"   /* found via -Ikernel/include */
#include <string.h>
#include <stdint.h>

#define RX_QUEUE_LEN   64u
#define RX_FRAME_MAX   1600u

typedef struct rx_slot { uint16_t len; uint8_t data[RX_FRAME_MAX]; } rx_slot_t;

static rx_slot_t   g_rx_ring[RX_QUEUE_LEN];
static volatile uint32_t g_rx_head = 0, g_rx_tail = 0;   /* head=produce, tail=consume */
static struct netif g_netif;

static int (*g_tx)(const void*, uint16_t) = 0;
static void (*g_set_rx_cb)(void(*)(const uint8_t*, uint16_t)) = 0;
static void (*g_get_mac)(uint8_t[6]) = 0;
static int (*g_link_up)(void) = 0;
static const char* g_drv_name = "none";

/* IRQ context: copy frame into ring (drop if full).
 *
 * Safety: single-producer (IRQ) / single-consumer (net_lwip_rx_pump, non-IRQ).
 * On a uniprocessor, the consumer cannot run while the IRQ handler is active,
 * so no lock is needed.  head/tail are volatile to prevent the compiler from
 * caching them across the critical memcpy.  We write the data before updating
 * g_rx_head so the consumer never sees an uninitialised slot. */
static void rx_isr_cb(const uint8_t* frame, uint16_t len) {
    uint32_t next;
    if (len > RX_FRAME_MAX) return;
    next = (g_rx_head + 1u) % RX_QUEUE_LEN;
    if (next == g_rx_tail) return;            /* full: drop */
    g_rx_ring[g_rx_head].len = len;
    memcpy(g_rx_ring[g_rx_head].data, frame, len);
    g_rx_head = next;                         /* publish last */
}

/* lwIP -> driver TX: flatten pbuf chain, send raw frame. */
static err_t netif_linkoutput(struct netif* netif, struct pbuf* p) {
    static uint8_t txbuf[RX_FRAME_MAX];
    uint16_t off = 0;
    struct pbuf* q;
    (void)netif;
    if (p->tot_len > RX_FRAME_MAX) return ERR_BUF;
    for (q = p; q != NULL; q = q->next) {
        memcpy(txbuf + off, q->payload, q->len);
        off += q->len;
    }
    if (g_tx && g_tx(txbuf, off) == 0) return ERR_OK;
    return ERR_IF;
}

err_t deanos_netif_init(struct netif* netif) {
    netif->name[0] = 'e'; netif->name[1] = 'n';
    netif->output = etharp_output;          /* ARP for IPv4 */
    netif->linkoutput = netif_linkoutput;
    netif->mtu = 1500;
    netif->hwaddr_len = 6;
    if (g_get_mac) g_get_mac(netif->hwaddr);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

void net_lwip_rx_pump(void) {
    while (g_rx_tail != g_rx_head) {
        rx_slot_t* s = &g_rx_ring[g_rx_tail];
        struct pbuf* p = pbuf_alloc(PBUF_RAW, s->len, PBUF_POOL);
        if (p) {
            pbuf_take(p, s->data, s->len);
            if (g_netif.input(p, &g_netif) != ERR_OK) pbuf_free(p);
        }
        g_rx_tail = (g_rx_tail + 1u) % RX_QUEUE_LEN;
    }
}

int deanos_netif_bind_driver(void) {
    /* Mirror the old net_initialize probe order: e1000 first, then rtl8139. */
    if (e1000_is_ready()) {
        g_tx = e1000_send_raw; g_set_rx_cb = e1000_set_rx_callback;
        g_get_mac = e1000_get_mac; g_link_up = e1000_link_up; g_drv_name = "e1000";
    } else if (rtl8139_is_ready()) {
        g_tx = rtl8139_send_raw; g_set_rx_cb = rtl8139_set_rx_callback;
        g_get_mac = rtl8139_get_mac; g_link_up = rtl8139_link_up; g_drv_name = "rtl8139";
    } else {
        return -1;
    }
    g_set_rx_cb(rx_isr_cb);
    return 0;
}

struct netif* deanos_netif_default(void) { return &g_netif; }
const char* deanos_netif_driver_name(void) { return g_drv_name; }
