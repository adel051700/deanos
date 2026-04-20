#include "include/kernel/net.h"

#include "include/kernel/e1000.h"
#include "include/kernel/idt.h"
#include "include/kernel/log.h"
#include "include/kernel/net_dhcp.h"
#include "include/kernel/net_dns.h"
#include "include/kernel/pci.h"
#include "include/kernel/pit.h"
#include "include/kernel/rtl8139.h"

#include <string.h>

typedef enum net_driver_kind {
    NET_DRIVER_NONE = 0,
    NET_DRIVER_E1000,
    NET_DRIVER_RTL8139,
} net_driver_kind_t;

#define NET_ETH_TYPE_ARP  0x0806u
#define NET_ETH_TYPE_IPV4 0x0800u

#define NET_NETIF_FLAG_UP       0x01u
#define NET_NETIF_FLAG_LINK_UP  0x02u

#define NET_ARP_HTYPE_ETHERNET 0x0001u
#define NET_ARP_OP_REQUEST     0x0001u
#define NET_ARP_OP_REPLY       0x0002u

#define NET_ARP_CACHE_SIZE 16u
#define NET_ARP_TTL_MS     120000u

#define NET_IP_PROTO_ICMP  1u
#define NET_IP_PROTO_TCP   6u
#define NET_IP_PROTO_UDP  17u
#define NET_ICMP_ECHO_REPLY 0u
#define NET_ICMP_DEST_UNREACH 3u
#define NET_ICMP_ECHO_REQUEST 8u
#define NET_ICMP_TIME_EXCEEDED 11u

#define NET_UDP_MAX_SOCKETS      16u
#define NET_UDP_QUEUE_DEPTH       8u
#define NET_UDP_PAYLOAD_MAX     512u
#define NET_UDP_EPHEMERAL_START 49152u
#define NET_UDP_EPHEMERAL_END   65535u

#define NET_TCP_MAX_SOCKETS      8u
#define NET_TCP_MAX_LISTENERS    8u
#define NET_TCP_ACCEPTQ_MAX      8u
#define NET_TCP_EPHEMERAL_START 40000u
#define NET_TCP_EPHEMERAL_END   49999u
#define NET_TCP_RX_BUFFER_SIZE   2048u

#define NET_TCP_FLAG_FIN 0x01u
#define NET_TCP_FLAG_SYN 0x02u
#define NET_TCP_FLAG_RST 0x04u
#define NET_TCP_FLAG_ACK 0x10u

#define NET_TCP_RTX_PAYLOAD_MAX 1200u
#define NET_TCP_RTX_RTO_MS      500u
#define NET_TCP_RTX_MAX_RETRIES   6u
#define NET_TCP_TIME_WAIT_MS    2000u
#define NET_TCP_OOO_MAX_SEGS       4u
#define NET_TCP_OOO_SEG_MAX      384u

#define NET_TCP_MSS                 1460u
#define NET_TCP_SEG_MAX              1200u
#define NET_TCP_INITIAL_CWND_SEGS    3u
#define NET_TCP_INITIAL_SSTHRESH    65535u
#define NET_TCP_MIN_SSTHRESH_SEGS    2u

#define NET_TCP_DELACK_MS                40u
#define NET_TCP_DELACK_MAX_SEGS_PENDING   2u

#define NET_TCP_KEEPALIVE_IDLE_MS    60000u
#define NET_TCP_KEEPALIVE_PROBE_MS    5000u
#define NET_TCP_KEEPALIVE_MAX_PROBES     5u

#define NET_IPV4_TX_MAX_PAYLOAD   65515u
#define NET_IPV4_FRAG_PAYLOAD      1480u

#define NET_TIMER_LINK_REFRESH_MS 1000u
#define NET_DHCP_RETRY_BASE_MS   2000u
#define NET_DHCP_RETRY_MAX_MS   30000u
#define NET_DHCP_ACK_WAIT_MS     3000u
#define NET_DHCP_MAX_RETRIES        6u

#define NET_MAX_NETIFS             4u
#define NET_IPV4_REASM_CTX_MAX     4u
#define NET_IPV4_REASM_MAX_DATA 1480u
#define NET_IPV4_REASM_TIMEOUT_MS 1500u
#define NET_DNS_CACHE_SIZE        16u
#define NET_DNS_CACHE_NAME_MAX    63u
#define NET_DNS_TTL_MIN_S         10u
#define NET_DNS_TTL_MAX_S       3600u
#define NET_DNS_NEG_TTL_S         30u

typedef enum net_tcp_state {
    NET_TCP_STATE_CLOSED = 0,
    NET_TCP_STATE_SYN_RECEIVED,
    NET_TCP_STATE_SYN_SENT,
    NET_TCP_STATE_ESTABLISHED,
    NET_TCP_STATE_CLOSE_WAIT,
    NET_TCP_STATE_FIN_WAIT_1,
    NET_TCP_STATE_FIN_WAIT_2,
    NET_TCP_STATE_CLOSING,
    NET_TCP_STATE_TIME_WAIT,
    NET_TCP_STATE_LAST_ACK,
    NET_TCP_STATE_RESET,
} net_tcp_state_t;

typedef struct net_tcp_ooo_seg {
    uint8_t used;
    uint16_t len;
    uint32_t seq;
    uint8_t data[NET_TCP_OOO_SEG_MAX];
} net_tcp_ooo_seg_t;

typedef struct net_tcp_socket_state {
    volatile uint8_t in_use;
    volatile uint8_t state;
    uint16_t local_port;
    uint16_t remote_port;
    uint8_t remote_ip[4];
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t snd_wnd;
    uint32_t rcv_nxt;
    uint32_t last_ack_seen;
    uint8_t dup_ack_count;
    volatile uint8_t peer_closed;
    volatile uint8_t rx_overflow;
    volatile uint16_t rx_len;
    uint8_t rx_buf[NET_TCP_RX_BUFFER_SIZE];
    volatile int connect_result;
    uint8_t rtx_valid;
    uint8_t rtx_flags;
    uint8_t rtx_due;
    uint8_t rtx_retries;
    uint16_t rtx_payload_len;
    uint32_t rtx_seq;
    uint32_t rtx_ack;
    uint32_t rtx_elapsed_ms;
    uint32_t rtx_rto_ms;
    uint32_t rtx_send_ts_ms;
    uint8_t rtx_payload[NET_TCP_RTX_PAYLOAD_MAX];
    uint32_t srtt_ms;
    uint32_t rttvar_ms;
    uint32_t rto_ms;
    uint32_t time_wait_ms;
    int8_t listener_id;
    uint8_t accepted;
    net_tcp_ooo_seg_t ooo[NET_TCP_OOO_MAX_SEGS];
    uint32_t cwnd;
    uint32_t ssthresh;
    uint16_t mss;
    uint8_t nodelay;
    uint8_t delack_pending;
    uint8_t delack_segs_pending;
    uint16_t delack_ms;
    uint8_t keepalive_enabled;
    uint8_t keepalive_probes;
    uint16_t keepalive_probe_ms;
    uint32_t idle_ms;
} net_tcp_socket_state_t;

typedef struct net_tcp_listener_state {
    uint8_t in_use;
    uint8_t bound;
    uint16_t local_port;
    uint16_t backlog;
    uint16_t qhead;
    uint16_t qtail;
    uint16_t qcount;
    int16_t pending[NET_TCP_ACCEPTQ_MAX];
} net_tcp_listener_state_t;

typedef struct net_udp_datagram {
    uint8_t src_ip[4];
    uint16_t src_port;
    uint16_t len;
    uint16_t stored_len;
    uint8_t data[NET_UDP_PAYLOAD_MAX];
} net_udp_datagram_t;

typedef struct net_udp_socket_state {
    uint8_t in_use;
    uint8_t bound;
    uint16_t local_port;
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t count;
    net_udp_datagram_t queue[NET_UDP_QUEUE_DEPTH];
} net_udp_socket_state_t;

typedef struct net_pbuf {
    uint16_t len;
    uint16_t tot_len;
    uint16_t refcount;
    int16_t chain_next;
    int16_t next_free;
    uint8_t data[NET_RX_PBUF_DATA_SIZE];
} net_pbuf_t;

typedef struct net_rx_queue {
    uint16_t head;
    uint16_t tail;
    uint16_t count;
    uint16_t slots[NET_RX_DEFER_QUEUE_LEN];
} net_rx_queue_t;

typedef struct net_dhcp_timer_state {
    uint32_t lease_remaining_ms;
    uint32_t t1_remaining_ms;
    uint32_t t2_remaining_ms;
    uint32_t retry_remaining_ms;
    uint32_t retry_count;
    uint32_t retry_due_count;
} net_dhcp_timer_state_t;

typedef enum net_dhcp_client_state {
    NET_DHCP_CLIENT_DISABLED = 0,
    NET_DHCP_CLIENT_BOUND,
    NET_DHCP_CLIENT_RENEWING,
    NET_DHCP_CLIENT_REBINDING,
    NET_DHCP_CLIENT_FAILED,
} net_dhcp_client_state_t;

typedef struct net_dhcp_client {
    uint8_t enabled;
    uint8_t state;
    uint8_t waiting_ack;
    uint8_t have_fallback;
    int32_t socket_id;
    uint32_t xid;
    uint32_t ack_wait_ms;
    uint32_t retries;
    uint32_t seen_retry_due;
    uint8_t server_ip[4];
    uint8_t lease_ip[4];
    uint8_t netmask[4];
    uint8_t gateway[4];
    uint8_t fallback_ip[4];
    uint8_t fallback_mask[4];
    uint8_t fallback_gw[4];
} net_dhcp_client_t;

typedef struct net_timer_state {
    uint32_t arp_accum_ms;
    uint32_t link_refresh_accum_ms;
    uint8_t link_refresh_due;
    uint32_t link_refresh_count;
    uint32_t link_state_changes;
    uint8_t last_link_up;
    uint32_t tcp_rtx_due_count;
    uint32_t tcp_rtx_sent_count;
    uint32_t tcp_rtx_timeout_count;
    uint32_t tcp_rtx_scan_count;
} net_timer_state_t;

typedef struct net_ipv4_reasm_ctx {
    uint8_t used;
    uint8_t src_ip[4];
    uint8_t dst_ip[4];
    uint16_t id;
    uint8_t proto;
    uint16_t ihl;
    uint8_t header[60];
    uint8_t have_first;
    uint8_t have_last;
    uint16_t total_payload_len;
    uint8_t data[NET_IPV4_REASM_MAX_DATA];
    uint8_t present[NET_IPV4_REASM_MAX_DATA];
    uint32_t last_update_ms;
} net_ipv4_reasm_ctx_t;

typedef struct net_dns_cache_entry {
    uint8_t valid;
    uint8_t negative;
    char name[NET_DNS_CACHE_NAME_MAX + 1u];
    uint8_t ip[4];
    uint32_t expires_at_ms;
    uint32_t last_used_ms;
} net_dns_cache_entry_t;

static net_driver_kind_t g_driver = NET_DRIVER_NONE;
#define g_ipv4_addr g_default_netif.ipv4_addr
#define g_ipv4_netmask g_default_netif.ipv4_netmask
#define g_ipv4_gateway g_default_netif.ipv4_gateway
static net_arp_entry_t g_arp_cache[NET_ARP_CACHE_SIZE] = {0};
static uint32_t g_arp_tick = 1u;
static net_arp_stats_t g_arp_stats = {0};
static volatile uint8_t g_ping_waiting = 0;
static volatile uint16_t g_ping_expect_id = 0;
static volatile uint16_t g_ping_expect_seq = 0;
static uint8_t g_ping_expect_ip[4] = {0};
static volatile int g_ping_result = NET_PING_ERR_TIMEOUT;
static net_udp_socket_state_t g_udp_sockets[NET_UDP_MAX_SOCKETS] = {0};
static uint16_t g_udp_next_ephemeral = NET_UDP_EPHEMERAL_START;
static net_tcp_socket_state_t g_tcp_sockets[NET_TCP_MAX_SOCKETS] = {0};
static net_tcp_listener_state_t g_tcp_listeners[NET_TCP_MAX_LISTENERS] = {0};
static uint16_t g_tcp_next_ephemeral = NET_TCP_EPHEMERAL_START;
static uint16_t g_dns_next_id = 1u;
static net_tcp_debug_stats_t g_tcp_dbg = {0};
static net_pbuf_t g_rx_pbuf_pool[NET_RX_PBUF_POOL_SIZE] = {0};
static net_rx_queue_t g_rx_defer_queue = {0};
static int16_t g_rx_pbuf_free_head = -1;
static net_rx_defer_stats_t g_rx_defer_stats = {0};
static uint32_t g_net_timer_accum_ms = 0u;
static net_dhcp_timer_state_t g_dhcp_timer = {0};
static net_timer_state_t g_timer_state = {0};
static net_dhcp_client_t g_dhcp_client = {0};
static netif_t* g_netifs[NET_MAX_NETIFS] = {0};
static uint32_t g_netif_count = 0u;
static net_ipv4_reasm_ctx_t g_ipv4_reasm[NET_IPV4_REASM_CTX_MAX] = {0};
static net_dns_cache_entry_t g_dns_cache[NET_DNS_CACHE_SIZE] = {0};
static net_p2_stats_t g_p2_stats = {0};

static int netif_e1000_linkoutput(const void* data, uint16_t len) { return e1000_send_raw(data, len); }
static int netif_e1000_is_ready(void) { return e1000_is_ready(); }
static int netif_e1000_link_up(void) { return e1000_link_up(); }
static int netif_e1000_send_test_frame(void) { return e1000_send_test_frame(); }
static void netif_e1000_get_mac(uint8_t out_mac[6]) { e1000_get_mac(out_mac); }

static int netif_rtl8139_linkoutput(const void* data, uint16_t len) { return rtl8139_send_raw(data, len); }
static int netif_rtl8139_is_ready(void) { return rtl8139_is_ready(); }
static int netif_rtl8139_link_up(void) { return rtl8139_link_up(); }
static int netif_rtl8139_send_test_frame(void) { return rtl8139_send_test_frame(); }
static void netif_rtl8139_get_mac(uint8_t out_mac[6]) { rtl8139_get_mac(out_mac); }

static const netif_driver_ops_t g_e1000_ops = {
    .linkoutput = netif_e1000_linkoutput,
    .is_ready = netif_e1000_is_ready,
    .link_up = netif_e1000_link_up,
    .send_test_frame = netif_e1000_send_test_frame,
    .get_mac = netif_e1000_get_mac,
};

static const netif_driver_ops_t g_rtl8139_ops = {
    .linkoutput = netif_rtl8139_linkoutput,
    .is_ready = netif_rtl8139_is_ready,
    .link_up = netif_rtl8139_link_up,
    .send_test_frame = netif_rtl8139_send_test_frame,
    .get_mac = netif_rtl8139_get_mac,
};

static netif_t g_default_netif = {
    .name = "none",
    .mtu = 1500u,
    .flags = 0u,
    .hwaddr = {0},
    .ipv4_addr = {10u, 0u, 2u, 15u},
    .ipv4_netmask = {255u, 255u, 255u, 0u},
    .ipv4_gateway = {10u, 0u, 2u, 2u},
    .rx_frames = 0u,
    .tx_frames = 0u,
    .rx_drops = 0u,
    .link_changes = 0u,
    .driver_ops = NULL,
};

static netif_t g_loopback_netif = {
    .name = "lo",
    .mtu = 1500u,
    .flags = NET_NETIF_FLAG_UP | NET_NETIF_FLAG_LINK_UP,
    .hwaddr = {0},
    .ipv4_addr = {127u, 0u, 0u, 1u},
    .ipv4_netmask = {255u, 0u, 0u, 0u},
    .ipv4_gateway = {0u, 0u, 0u, 0u},
    .rx_frames = 0u,
    .tx_frames = 0u,
    .rx_drops = 0u,
    .link_changes = 0u,
    .driver_ops = NULL,
};

static int net_ipv4_select_next_hop(const uint8_t dst_ip[4], uint8_t out_next_hop[4]);
static int net_send_ipv4(const uint8_t* dst_mac,
                         const uint8_t dst_ip[4],
                         uint8_t proto,
                         const uint8_t* l4_payload,
                         uint16_t l4_len);
static uint32_t net_now_ms32(void);
static void net_ethernet_input(const uint8_t* frame, uint16_t len);
static void net_ipv4_input(const uint8_t* frame, uint16_t len);
static void etharp_input(const uint8_t* frame, uint16_t len);
static void ip4_input(const uint8_t* frame, uint16_t len);
static void udp_input(const uint8_t src_ip[4], const uint8_t* udp, uint16_t udp_len);
static void tcp_input(const uint8_t src_ip[4], const uint8_t dst_ip[4], const uint8_t* tcp, uint16_t tcp_len);
static void net_icmp_input(const uint8_t* frame,
                           uint16_t len,
                           const uint8_t* ip,
                           uint16_t ihl,
                           uint16_t total_len,
                           const uint8_t src_ip[4],
                           const uint8_t dst_ip[4]);
static void netif_sync_link_state(void);
static int netif_register(netif_t* nif);
static netif_t* netif_pick_egress(const uint8_t dst_ip[4]);
static int net_ipv4_is_loopback(const uint8_t ip[4]);
static int net_ipv4_is_self(const uint8_t ip[4]);
static int net_ipv4_is_local(const uint8_t ip[4]);
static void net_timers_run_deferred(void);
static void net_tcp_rtx_arm(net_tcp_socket_state_t* s,
                            uint32_t seq,
                            uint32_t ack,
                            uint8_t flags,
                            const uint8_t* payload,
                            uint16_t payload_len);
static void net_tcp_rtx_on_ack(net_tcp_socket_state_t* s, uint32_t ack);
static uint8_t net_tcp_rtx_consumes_seq(uint8_t flags);
static int net_tcp_listener_find_by_port(uint16_t port);
static int net_tcp_listener_enqueue(int listener_id, int socket_id);
static uint16_t net_tcp_advertised_window(const net_tcp_socket_state_t* s);
static int net_tcp_send_segment_window(const uint8_t dst_ip[4],
                                       uint16_t src_port,
                                       uint16_t dst_port,
                                       uint32_t seq,
                                       uint32_t ack,
                                       uint8_t flags,
                                       uint16_t window,
                                       const uint8_t* payload,
                                       uint16_t payload_len);
static int net_tcp_seq_payload_fits(uint32_t seq, uint16_t len, uint32_t expect, uint16_t window);
static int net_tcp_ooo_insert(net_tcp_socket_state_t* s, uint32_t seq, const uint8_t* payload, uint16_t len);
static void net_tcp_try_merge_ooo(net_tcp_socket_state_t* s);
static void net_rx_defer_init(void);
static void net_rx_enqueue_frame(const uint8_t* frame, uint16_t len);
static int16_t net_pbuf_alloc(void);
static void net_pbuf_free_chain(int16_t head_idx);
static void net_dhcp_client_step(void);
static void net_dhcp_client_on_ack(const uint8_t* pkt, uint16_t len);
static int net_ipv4_reassemble(const uint8_t* frame, uint16_t len, uint8_t* out_pkt, uint16_t* out_len);
static int net_dns_cache_lookup(const char* hostname, uint8_t out_ip[4], int* out_negative);
static void net_dns_cache_store(const char* hostname, const uint8_t ip[4], uint32_t ttl_seconds, int negative);

static uint32_t net_irq_save(void) {
    uint32_t flags;
    __asm__ volatile ("pushf; pop %0" : "=r"(flags));
    interrupts_disable();
    return flags;
}

static void net_irq_restore(uint32_t flags) {
    if ((flags & (1u << 9)) != 0u) {
        interrupts_enable();
    }
}

static uint16_t net_read_be16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static void net_write_be16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

static uint32_t net_read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void net_write_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static int net_udp_socket_id_valid(int socket_id) {
    return socket_id >= 0 && socket_id < (int)NET_UDP_MAX_SOCKETS;
}

static uint16_t net_tcp_checksum_ipv4(const uint8_t src_ip[4],
                                      const uint8_t dst_ip[4],
                                      const uint8_t* tcp,
                                      uint16_t tcp_len) {
    uint32_t sum = 0;

    if (!src_ip || !dst_ip || !tcp || tcp_len < 20u) return 0u;

    sum += ((uint16_t)src_ip[0] << 8) | src_ip[1];
    sum += ((uint16_t)src_ip[2] << 8) | src_ip[3];
    sum += ((uint16_t)dst_ip[0] << 8) | dst_ip[1];
    sum += ((uint16_t)dst_ip[2] << 8) | dst_ip[3];
    sum += NET_IP_PROTO_TCP;
    sum += tcp_len;

    for (uint16_t i = 0; i + 1u < tcp_len; i = (uint16_t)(i + 2u)) {
        sum += ((uint16_t)tcp[i] << 8) | tcp[i + 1u];
    }
    if ((tcp_len & 1u) != 0u) {
        sum += ((uint16_t)tcp[tcp_len - 1u] << 8);
    }

    while ((sum >> 16) != 0u) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }

    return (uint16_t)(~sum);
}

static uint8_t net_tcp_rtx_consumes_seq(uint8_t flags) {
    uint8_t consume = 0u;
    if ((flags & NET_TCP_FLAG_SYN) != 0u) consume++;
    if ((flags & NET_TCP_FLAG_FIN) != 0u) consume++;
    return consume;
}

static void net_tcp_rtx_arm(net_tcp_socket_state_t* s,
                            uint32_t seq,
                            uint32_t ack,
                            uint8_t flags,
                            const uint8_t* payload,
                            uint16_t payload_len) {
    if (!s) return;
    if (payload_len > NET_TCP_RTX_PAYLOAD_MAX) payload_len = NET_TCP_RTX_PAYLOAD_MAX;

    s->rtx_valid = 1u;
    s->rtx_due = 0u;
    s->rtx_retries = 0u;
    s->rtx_seq = seq;
    s->rtx_ack = ack;
    s->rtx_flags = flags;
    s->rtx_payload_len = payload_len;
    s->rtx_elapsed_ms = 0u;
    s->rtx_rto_ms = s->rto_ms != 0u ? s->rto_ms : NET_TCP_RTX_RTO_MS;
    s->rtx_send_ts_ms = net_now_ms32();
    if (payload_len > 0u && payload) {
        memcpy(s->rtx_payload, payload, payload_len);
    }
}

static int net_tcp_send_pure_ack(net_tcp_socket_state_t* s) {
    int rc;
    if (!s) return -1;
    rc = net_tcp_send_segment_window(s->remote_ip,
                                     s->local_port,
                                     s->remote_port,
                                     s->snd_nxt,
                                     s->rcv_nxt,
                                     NET_TCP_FLAG_ACK,
                                     net_tcp_advertised_window(s),
                                     NULL,
                                     0u);
    if (rc == 0) {
        s->delack_pending = 0u;
        s->delack_segs_pending = 0u;
        s->delack_ms = 0u;
        s->idle_ms = 0u;
    }
    return rc;
}

static void net_tcp_schedule_ack(net_tcp_socket_state_t* s) {
    if (!s) return;
    if (s->delack_segs_pending < 0xFFu) s->delack_segs_pending++;
    if (s->delack_segs_pending >= NET_TCP_DELACK_MAX_SEGS_PENDING) {
        (void)net_tcp_send_pure_ack(s);
        return;
    }
    s->delack_pending = 1u;
    s->delack_ms = NET_TCP_DELACK_MS;
}

static void net_tcp_note_tx(net_tcp_socket_state_t* s) {
    if (!s) return;
    s->delack_pending = 0u;
    s->delack_segs_pending = 0u;
    s->delack_ms = 0u;
    s->idle_ms = 0u;
    s->keepalive_probes = 0u;
    s->keepalive_probe_ms = 0u;
}

static void net_tcp_cc_on_ack(net_tcp_socket_state_t* s, uint32_t bytes_acked) {
    uint32_t grow;
    if (!s || bytes_acked == 0u) return;
    if (s->cwnd < s->ssthresh) {
        grow = bytes_acked;
        if (grow > s->mss) grow = s->mss;
        s->cwnd += grow;
    } else {
        grow = ((uint32_t)s->mss * s->mss) / (s->cwnd == 0u ? 1u : s->cwnd);
        if (grow == 0u) grow = 1u;
        s->cwnd += grow;
    }
    if (s->cwnd > 65535u) s->cwnd = 65535u;
}

static void net_tcp_cc_on_timeout(net_tcp_socket_state_t* s) {
    uint32_t flight;
    uint32_t min_ssthresh;
    if (!s) return;
    flight = s->snd_nxt - s->snd_una;
    min_ssthresh = (uint32_t)NET_TCP_MIN_SSTHRESH_SEGS * s->mss;
    s->ssthresh = flight / 2u;
    if (s->ssthresh < min_ssthresh) s->ssthresh = min_ssthresh;
    s->cwnd = s->mss;
}

static void net_tcp_rtx_on_ack(net_tcp_socket_state_t* s, uint32_t ack) {
    uint32_t end_seq;

    if (!s || !s->rtx_valid) return;

    end_seq = s->rtx_seq + s->rtx_payload_len + net_tcp_rtx_consumes_seq(s->rtx_flags);
    if (ack >= end_seq) {
        if (s->rtx_retries == 0u && s->rtx_send_ts_ms != 0u) {
            uint32_t sample = net_now_ms32() - s->rtx_send_ts_ms;
            if (sample == 0u) sample = 1u;
            if (s->srtt_ms == 0u) {
                s->srtt_ms = sample;
                s->rttvar_ms = sample / 2u;
            } else {
                uint32_t abs_err = (s->srtt_ms > sample) ? (s->srtt_ms - sample) : (sample - s->srtt_ms);
                s->rttvar_ms = (uint32_t)((3u * s->rttvar_ms + abs_err) / 4u);
                s->srtt_ms = (uint32_t)((7u * s->srtt_ms + sample) / 8u);
            }
            s->rtx_rto_ms = s->srtt_ms + (4u * s->rttvar_ms);
            if (s->rtx_rto_ms < 200u) s->rtx_rto_ms = 200u;
            if (s->rtx_rto_ms > 4000u) s->rtx_rto_ms = 4000u;
        }
        s->rtx_valid = 0u;
        s->rtx_due = 0u;
        s->rtx_retries = 0u;
        s->rtx_elapsed_ms = 0u;
        s->rtx_send_ts_ms = 0u;
    }
}

static int net_tcp_listener_find_by_port(uint16_t port) {
    for (uint32_t i = 0; i < NET_TCP_MAX_LISTENERS; ++i) {
        if (g_tcp_listeners[i].in_use && g_tcp_listeners[i].bound && g_tcp_listeners[i].local_port == port) {
            return (int)i;
        }
    }
    return -1;
}

static int net_tcp_listener_enqueue(int listener_id, int socket_id) {
    net_tcp_listener_state_t* l;
    if (listener_id < 0 || listener_id >= (int)NET_TCP_MAX_LISTENERS) return 0;
    if (socket_id < 0 || socket_id >= (int)NET_TCP_MAX_SOCKETS) return 0;
    l = &g_tcp_listeners[listener_id];
    if (!l->in_use || l->backlog == 0u) return 0;
    if (l->qcount >= l->backlog || l->qcount >= NET_TCP_ACCEPTQ_MAX) return 0;
    l->pending[l->qtail] = (int16_t)socket_id;
    l->qtail = (uint16_t)((l->qtail + 1u) % NET_TCP_ACCEPTQ_MAX);
    l->qcount++;
    return 1;
}

static int net_tcp_seq_payload_fits(uint32_t seq, uint16_t len, uint32_t expect, uint16_t window) {
    uint32_t seg_start = seq;
    uint32_t seg_end = len == 0u ? seq : (seq + (uint32_t)len - 1u);
    uint32_t win_start = expect;
    uint32_t win_end;

    if (window == 0u) {
        return len == 0u && seq == expect;
    }

    win_end = win_start + (uint32_t)window - 1u;
    if (len == 0u) {
        return seg_start >= win_start && seg_start <= win_end;
    }

    if (seg_end < win_start) return 0;
    if (seg_start > win_end) return 0;
    return 1;
}

static int net_tcp_ooo_insert(net_tcp_socket_state_t* s, uint32_t seq, const uint8_t* payload, uint16_t len) {
    int free_idx = -1;
    if (!s || !payload || len == 0u) return 0;
    if (len > NET_TCP_OOO_SEG_MAX) len = NET_TCP_OOO_SEG_MAX;

    for (uint32_t i = 0; i < NET_TCP_OOO_MAX_SEGS; ++i) {
        if (s->ooo[i].used && s->ooo[i].seq == seq) {
            if (len > s->ooo[i].len) {
                memcpy(s->ooo[i].data, payload, len);
                s->ooo[i].len = len;
            }
            return 1;
        }
        if (!s->ooo[i].used && free_idx < 0) free_idx = (int)i;
    }
    if (free_idx < 0) return 0;

    s->ooo[free_idx].used = 1u;
    s->ooo[free_idx].seq = seq;
    s->ooo[free_idx].len = len;
    memcpy(s->ooo[free_idx].data, payload, len);
    return 1;
}

static void net_tcp_try_merge_ooo(net_tcp_socket_state_t* s) {
    int progress = 1;
    if (!s) return;

    while (progress) {
        progress = 0;
        for (uint32_t i = 0; i < NET_TCP_OOO_MAX_SEGS; ++i) {
            if (!s->ooo[i].used || s->ooo[i].seq != s->rcv_nxt) continue;
            if (s->rx_len >= NET_TCP_RX_BUFFER_SIZE) return;
            if (s->ooo[i].len > (uint16_t)(NET_TCP_RX_BUFFER_SIZE - s->rx_len)) return;

            memcpy(s->rx_buf + s->rx_len, s->ooo[i].data, s->ooo[i].len);
            s->rx_len = (uint16_t)(s->rx_len + s->ooo[i].len);
            s->rcv_nxt += s->ooo[i].len;
            s->ooo[i].used = 0u;
            s->ooo[i].len = 0u;
            s->ooo[i].seq = 0u;
            progress = 1;
            break;
        }
    }
}

static int net_tcp_socket_alloc(void) {
    for (uint32_t i = 0; i < NET_TCP_MAX_SOCKETS; ++i) {
        if (!g_tcp_sockets[i].in_use) {
            memset(&g_tcp_sockets[i], 0, sizeof(g_tcp_sockets[i]));
            g_tcp_sockets[i].in_use = 1;
            g_tcp_sockets[i].state = NET_TCP_STATE_CLOSED;
            g_tcp_sockets[i].connect_result = NET_TCP_ERR_TIMEOUT;
            g_tcp_sockets[i].snd_wnd = NET_TCP_RX_BUFFER_SIZE;
            g_tcp_sockets[i].rto_ms = NET_TCP_RTX_RTO_MS;
            g_tcp_sockets[i].listener_id = -1;
            g_tcp_sockets[i].mss = NET_TCP_SEG_MAX;
            g_tcp_sockets[i].cwnd = (uint32_t)NET_TCP_INITIAL_CWND_SEGS * NET_TCP_SEG_MAX;
            g_tcp_sockets[i].ssthresh = NET_TCP_INITIAL_SSTHRESH;
            return (int)i;
        }
    }
    return -1;
}

static void net_tcp_socket_release(int socket_id) {
    if (socket_id < 0 || socket_id >= (int)NET_TCP_MAX_SOCKETS) return;
    memset(&g_tcp_sockets[socket_id], 0, sizeof(g_tcp_sockets[socket_id]));
}

static int net_tcp_port_in_use(uint16_t port) {
    for (uint32_t i = 0; i < NET_TCP_MAX_SOCKETS; ++i) {
        if (g_tcp_sockets[i].in_use && g_tcp_sockets[i].local_port == port) return 1;
    }
    for (uint32_t i = 0; i < NET_TCP_MAX_LISTENERS; ++i) {
        if (g_tcp_listeners[i].in_use && g_tcp_listeners[i].bound && g_tcp_listeners[i].local_port == port) return 1;
    }
    return 0;
}

static uint16_t net_tcp_allocate_ephemeral_port(void) {
    uint32_t attempts = (uint32_t)(NET_TCP_EPHEMERAL_END - NET_TCP_EPHEMERAL_START + 1u);
    uint16_t candidate = g_tcp_next_ephemeral;

    for (uint32_t i = 0; i < attempts; ++i) {
        if (!net_tcp_port_in_use(candidate)) {
            g_tcp_next_ephemeral = (candidate == NET_TCP_EPHEMERAL_END)
                                       ? NET_TCP_EPHEMERAL_START
                                       : (uint16_t)(candidate + 1u);
            return candidate;
        }
        candidate = (candidate == NET_TCP_EPHEMERAL_END)
                        ? NET_TCP_EPHEMERAL_START
                        : (uint16_t)(candidate + 1u);
    }
    return 0u;
}

static int net_tcp_socket_id_valid(int socket_id) {
    return socket_id >= 0 && socket_id < (int)NET_TCP_MAX_SOCKETS;
}

static int net_tcp_socket_is_active_state(uint8_t state) {
    return state == NET_TCP_STATE_SYN_SENT ||
           state == NET_TCP_STATE_SYN_RECEIVED ||
           state == NET_TCP_STATE_ESTABLISHED ||
           state == NET_TCP_STATE_CLOSE_WAIT ||
           state == NET_TCP_STATE_FIN_WAIT_1 ||
           state == NET_TCP_STATE_FIN_WAIT_2 ||
           state == NET_TCP_STATE_CLOSING ||
           state == NET_TCP_STATE_LAST_ACK;
}

static uint16_t net_tcp_advertised_window(const net_tcp_socket_state_t* s) {
    uint32_t free_space;
    if (!s) return 0x4000u;
    if (s->rx_len >= NET_TCP_RX_BUFFER_SIZE) return 0u;
    free_space = NET_TCP_RX_BUFFER_SIZE - s->rx_len;
    if (free_space > 0xFFFFu) free_space = 0xFFFFu;
    return (uint16_t)free_space;
}

static int net_tcp_send_segment_window(const uint8_t dst_ip[4],
                                       uint16_t src_port,
                                       uint16_t dst_port,
                                       uint32_t seq,
                                       uint32_t ack,
                                       uint8_t flags,
                                       uint16_t window,
                                       const uint8_t* payload,
                                       uint16_t payload_len) {
    uint8_t tcp[24 + 1460];
    uint8_t next_hop[4];
    uint8_t dst_mac[6];
    uint16_t tcp_len;
    uint16_t hdr_len;
    uint8_t data_off_words;
    int include_mss = (flags & NET_TCP_FLAG_SYN) != 0u;

    if (!dst_ip || (!payload && payload_len > 0u) || payload_len > 1460u) return -1;
    if (src_port == 0u || dst_port == 0u) return -1;

    hdr_len = include_mss ? 24u : 20u;
    data_off_words = include_mss ? 6u : 5u;
    tcp_len = (uint16_t)(hdr_len + payload_len);

    memset(tcp, 0, hdr_len);
    net_write_be16(tcp + 0, src_port);
    net_write_be16(tcp + 2, dst_port);
    net_write_be32(tcp + 4, seq);
    net_write_be32(tcp + 8, ack);
    tcp[12] = (uint8_t)(data_off_words << 4);
    tcp[13] = flags;
    net_write_be16(tcp + 14, window);
    net_write_be16(tcp + 16, 0u);
    net_write_be16(tcp + 18, 0u);
    if (include_mss) {
        tcp[20] = 2u;
        tcp[21] = 4u;
        net_write_be16(tcp + 22, 1460u);
    }
    if (payload_len > 0u) memcpy(tcp + hdr_len, payload, payload_len);
    net_write_be16(tcp + 16, net_tcp_checksum_ipv4(g_ipv4_addr, dst_ip, tcp, tcp_len));

    if (net_ipv4_select_next_hop(dst_ip, next_hop) != 0) return -1;
    if (net_arp_resolve_retry(next_hop, dst_mac, 3u, 150u) != 0) return -1;
    return net_send_ipv4(dst_mac, dst_ip, NET_IP_PROTO_TCP, tcp, tcp_len);
}

static int net_tcp_send_segment(const uint8_t dst_ip[4],
                                uint16_t src_port,
                                uint16_t dst_port,
                                uint32_t seq,
                                uint32_t ack,
                                uint8_t flags,
                                const uint8_t* payload,
                                uint16_t payload_len) {
    return net_tcp_send_segment_window(dst_ip,
                                       src_port,
                                       dst_port,
                                       seq,
                                       ack,
                                       flags,
                                       0x4000u,
                                       payload,
                                       payload_len);
}

static int net_udp_port_in_use(uint16_t port, int ignore_socket_id) {
    if (port == 0u) return 1;
    for (uint32_t i = 0; i < NET_UDP_MAX_SOCKETS; ++i) {
        if ((int)i == ignore_socket_id) continue;
        if (g_udp_sockets[i].in_use && g_udp_sockets[i].bound && g_udp_sockets[i].local_port == port) {
            return 1;
        }
    }
    return 0;
}

static uint16_t net_udp_allocate_ephemeral_port(void) {
    uint32_t attempts = (uint32_t)(NET_UDP_EPHEMERAL_END - NET_UDP_EPHEMERAL_START + 1u);
    uint16_t candidate = g_udp_next_ephemeral;

    for (uint32_t i = 0; i < attempts; ++i) {
        if (!net_udp_port_in_use(candidate, -1)) {
            g_udp_next_ephemeral = (candidate == NET_UDP_EPHEMERAL_END)
                                       ? NET_UDP_EPHEMERAL_START
                                       : (uint16_t)(candidate + 1u);
            return candidate;
        }
        candidate = (candidate == NET_UDP_EPHEMERAL_END)
                        ? NET_UDP_EPHEMERAL_START
                        : (uint16_t)(candidate + 1u);
    }

    return 0u;
}

static int net_udp_enqueue_to_socket(net_udp_socket_state_t* sock,
                                     const uint8_t src_ip[4],
                                     uint16_t src_port,
                                     const uint8_t* payload,
                                     uint16_t payload_len) {
    uint32_t slot;
    net_udp_datagram_t* dgram;
    uint16_t stored_len;

    if (!sock || !src_ip || (!payload && payload_len > 0u)) return 0;
    if (!sock->in_use || !sock->bound) return 0;
    if (sock->count >= NET_UDP_QUEUE_DEPTH) return 0;

    slot = sock->tail;
    dgram = &sock->queue[slot];
    memcpy(dgram->src_ip, src_ip, 4);
    dgram->src_port = src_port;
    dgram->len = payload_len;
    stored_len = payload_len > NET_UDP_PAYLOAD_MAX ? NET_UDP_PAYLOAD_MAX : payload_len;
    dgram->stored_len = stored_len;
    if (stored_len > 0u) {
        memcpy(dgram->data, payload, stored_len);
    }

    sock->tail = (sock->tail + 1u) % NET_UDP_QUEUE_DEPTH;
    sock->count++;
    return 1;
}

static int net_udp_deliver_datagram(uint16_t dst_port,
                                    const uint8_t src_ip[4],
                                    uint16_t src_port,
                                    const uint8_t* payload,
                                    uint16_t payload_len) {
    int delivered = 0;
    for (uint32_t i = 0; i < NET_UDP_MAX_SOCKETS; ++i) {
        net_udp_socket_state_t* sock = &g_udp_sockets[i];
        if (!sock->in_use || !sock->bound || sock->local_port != dst_port) continue;
        if (net_udp_enqueue_to_socket(sock, src_ip, src_port, payload, payload_len)) {
            delivered = 1;
        }
    }
    return delivered;
}

static void net_handle_udp_ipv4(const uint8_t src_ip[4], const uint8_t* udp, uint16_t udp_len) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t wire_len;

    if (!src_ip || !udp || udp_len < 8u) {
        g_arp_stats.dropped_frames++;
        return;
    }

    src_port = net_read_be16(udp + 0);
    dst_port = net_read_be16(udp + 2);
    wire_len = net_read_be16(udp + 4);
    if (wire_len < 8u || wire_len > udp_len) {
        g_arp_stats.dropped_frames++;
        return;
    }

    (void)net_udp_deliver_datagram(dst_port, src_ip, src_port, udp + 8, (uint16_t)(wire_len - 8u));
}

static uint16_t net_checksum16(const uint8_t* data, uint16_t len) {
    uint32_t sum = 0;

    while (len > 1u) {
        sum += ((uint16_t)data[0] << 8) | (uint16_t)data[1];
        data += 2;
        len = (uint16_t)(len - 2u);
    }
    if (len > 0u) {
        sum += ((uint16_t)data[0] << 8);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

static uint16_t net_udp_checksum_ipv4(const uint8_t src_ip[4],
                                      const uint8_t dst_ip[4],
                                      const uint8_t* udp,
                                      uint16_t udp_len) {
    uint32_t sum = 0;
    uint16_t i;

    if (!src_ip || !dst_ip || !udp || udp_len < 8u) return 0u;

    /* IPv4 pseudo header */
    sum += ((uint16_t)src_ip[0] << 8) | src_ip[1];
    sum += ((uint16_t)src_ip[2] << 8) | src_ip[3];
    sum += ((uint16_t)dst_ip[0] << 8) | dst_ip[1];
    sum += ((uint16_t)dst_ip[2] << 8) | dst_ip[3];
    sum += NET_IP_PROTO_UDP;
    sum += udp_len;

    for (i = 0; i + 1u < udp_len; i = (uint16_t)(i + 2u)) {
        sum += ((uint16_t)udp[i] << 8) | udp[i + 1u];
    }
    if ((udp_len & 1u) != 0u) {
        sum += ((uint16_t)udp[udp_len - 1u] << 8);
    }

    while ((sum >> 16) != 0u) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }

    {
        uint16_t csum = (uint16_t)(~sum);
        return csum == 0u ? 0xFFFFu : csum;
    }
}

static int net_tcp_find_socket(const uint8_t src_ip[4], uint16_t src_port, uint16_t dst_port) {
    for (uint32_t i = 0; i < NET_TCP_MAX_SOCKETS; ++i) {
        net_tcp_socket_state_t* s = &g_tcp_sockets[i];
        if (!s->in_use) continue;
        if (s->local_port != dst_port || s->remote_port != src_port) continue;
        if (memcmp(s->remote_ip, src_ip, 4) != 0) continue;
        return (int)i;
    }
    return -1;
}

static void net_handle_tcp_ipv4(const uint8_t src_ip[4], const uint8_t dst_ip[4], const uint8_t* tcp, uint16_t tcp_len) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t data_off;
    uint8_t flags;
    uint16_t hdr_len;
    uint16_t peer_window;
    const uint8_t* payload;
    uint16_t payload_len;
    int idx;
    net_tcp_socket_state_t* s;

    if (!src_ip || !dst_ip || !tcp || tcp_len < 20u) {
        g_arp_stats.dropped_frames++;
        return;
    }

    data_off = (uint8_t)(tcp[12] >> 4);
    hdr_len = (uint16_t)(data_off * 4u);
    if (hdr_len < 20u || hdr_len > tcp_len) {
        g_arp_stats.dropped_frames++;
        return;
    }

    /*
     * Some virtualized paths can expose packets with checksum-offload artifacts.
     * Track checksum mismatches for diagnostics, but keep processing for now.
     */
    if (net_read_be16(tcp + 16) != 0u && net_tcp_checksum_ipv4(src_ip, dst_ip, tcp, tcp_len) != 0u) {
        g_tcp_dbg.checksum_drop++;
    }

    src_port = net_read_be16(tcp + 0);
    dst_port = net_read_be16(tcp + 2);
    seq = net_read_be32(tcp + 4);
    ack = net_read_be32(tcp + 8);
    flags = tcp[13];
    peer_window = net_read_be16(tcp + 14);
    payload = tcp + hdr_len;
    payload_len = (uint16_t)(tcp_len - hdr_len);

    idx = net_tcp_find_socket(src_ip, src_port, dst_port);
    if (idx < 0) {
        if ((flags & NET_TCP_FLAG_SYN) != 0u && (flags & NET_TCP_FLAG_ACK) == 0u) {
            int lid = net_tcp_listener_find_by_port(dst_port);
            if (lid >= 0) {
                int sid = net_tcp_socket_alloc();
                if (sid >= 0) {
                    net_tcp_socket_state_t* c = &g_tcp_sockets[sid];
                    uint32_t isn = (uint32_t)pit_get_uptime_ms() ^ ((uint32_t)dst_port << 16) ^ seq;

                    memcpy(c->remote_ip, src_ip, 4);
                    c->remote_port = src_port;
                    c->local_port = dst_port;
                    c->snd_una = isn;
                    c->snd_nxt = isn + 1u;
                    c->rcv_nxt = seq + 1u;
                    c->state = NET_TCP_STATE_SYN_RECEIVED;
                    c->listener_id = (int8_t)lid;

                    if (net_tcp_send_segment(c->remote_ip,
                                             c->local_port,
                                             c->remote_port,
                                             isn,
                                             c->rcv_nxt,
                                             NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK,
                                             NULL,
                                             0u) == 0) {
                        net_tcp_rtx_arm(c, isn, c->rcv_nxt, NET_TCP_FLAG_SYN | NET_TCP_FLAG_ACK, NULL, 0u);
                    } else {
                        net_tcp_socket_release(sid);
                    }
                }
            }
        }
        g_tcp_dbg.tuple_miss++;
        memcpy(g_tcp_dbg.last_miss_src_ip, src_ip, 4);
        g_tcp_dbg.last_miss_src_port = src_port;
        g_tcp_dbg.last_miss_dst_port = dst_port;
        g_tcp_dbg.last_miss_flags = flags;
        g_tcp_dbg.last_miss_seq = seq;
        g_tcp_dbg.last_miss_ack = ack;
        g_tcp_dbg.last_miss_arrival_ms = (uint32_t)pit_get_uptime_ms();

        /* RST unsolicited segments destined for a local address so peers fail fast
         * instead of timing out. Only respond to non-RST segments. */
        if ((flags & NET_TCP_FLAG_RST) == 0u && net_ipv4_is_local(dst_ip)) {
            uint32_t rst_seq;
            uint32_t rst_ack;
            uint8_t rst_flags;
            if ((flags & NET_TCP_FLAG_ACK) != 0u) {
                rst_seq = ack;
                rst_ack = 0u;
                rst_flags = NET_TCP_FLAG_RST;
            } else {
                uint32_t seg_len = payload_len;
                if ((flags & NET_TCP_FLAG_SYN) != 0u) seg_len += 1u;
                if ((flags & NET_TCP_FLAG_FIN) != 0u) seg_len += 1u;
                rst_seq = 0u;
                rst_ack = seq + seg_len;
                rst_flags = NET_TCP_FLAG_RST | NET_TCP_FLAG_ACK;
            }
            (void)net_tcp_send_segment(src_ip, dst_port, src_port, rst_seq, rst_ack, rst_flags, NULL, 0u);
        }
        return;
    }

    s = &g_tcp_sockets[idx];
    s->snd_wnd = peer_window;
    s->idle_ms = 0u;
    s->keepalive_probes = 0u;
    s->keepalive_probe_ms = 0u;

    if ((flags & NET_TCP_FLAG_ACK) != 0u) {
        if (ack > s->snd_una) {
            uint32_t bytes_acked = ack - s->snd_una;
            s->snd_una = ack;
            s->dup_ack_count = 0u;
            s->last_ack_seen = ack;
            net_tcp_cc_on_ack(s, bytes_acked);
            net_tcp_rtx_on_ack(s, ack);
        } else if (ack == s->snd_una && s->state == NET_TCP_STATE_ESTABLISHED) {
            if (s->last_ack_seen == ack) {
                if (s->dup_ack_count < 255u) s->dup_ack_count++;
            } else {
                s->dup_ack_count = 1u;
                s->last_ack_seen = ack;
            }
            if (s->dup_ack_count >= 3u && s->rtx_valid) {
                s->rtx_due = 1u;
            }
        }
    }

    if ((flags & NET_TCP_FLAG_RST) != 0u) {
        g_tcp_dbg.rst_seen++;
        s->state = NET_TCP_STATE_RESET;
        s->connect_result = NET_TCP_ERR_RESET;
        s->rtx_valid = 0u;
        return;
    }

    if (s->state == NET_TCP_STATE_SYN_RECEIVED) {
        if ((flags & NET_TCP_FLAG_ACK) != 0u && ack == s->snd_nxt) {
            s->state = NET_TCP_STATE_ESTABLISHED;
            if (!s->accepted && s->listener_id >= 0) {
                if (net_tcp_listener_enqueue((int)s->listener_id, idx)) {
                    s->accepted = 1u;
                }
            }
        }
        return;
    }

    if (s->state == NET_TCP_STATE_SYN_SENT) {
        if ((flags & NET_TCP_FLAG_SYN) != 0u && (flags & NET_TCP_FLAG_ACK) != 0u && ack == s->snd_nxt) {
            g_tcp_dbg.synack_seen++;
            s->rcv_nxt = seq + 1u;
            /* For probe mode, a valid SYN-ACK proves reachability. ACK is best-effort. */
            s->state = NET_TCP_STATE_ESTABLISHED;
            s->snd_una = ack;
            s->connect_result = NET_TCP_OK;
            (void)net_tcp_send_pure_ack(s);
        }
        return;
    }

    if (s->state == NET_TCP_STATE_ESTABLISHED) {
        if (payload_len > 0u) {
            if (!net_tcp_seq_payload_fits(seq, payload_len, s->rcv_nxt, net_tcp_advertised_window(s))) {
                (void)net_tcp_send_pure_ack(s);
                return;
            }

            if (seq < s->rcv_nxt) {
                uint32_t trim = s->rcv_nxt - seq;
                if (trim >= payload_len) payload_len = 0u;
                else {
                    payload += trim;
                    payload_len = (uint16_t)(payload_len - trim);
                    seq = s->rcv_nxt;
                }
            }

            if (payload_len > 0u && seq == s->rcv_nxt) {
                uint16_t free_space = (uint16_t)(NET_TCP_RX_BUFFER_SIZE - s->rx_len);
                uint16_t copy_len = payload_len > free_space ? free_space : payload_len;
                if (copy_len > 0u) {
                    memcpy(s->rx_buf + s->rx_len, payload, copy_len);
                    s->rx_len = (uint16_t)(s->rx_len + copy_len);
                }
                if (copy_len < payload_len) {
                    s->rx_overflow = 1u;
                }
                s->rcv_nxt = seq + copy_len;
                net_tcp_try_merge_ooo(s);
            } else if (payload_len > 0u && seq > s->rcv_nxt) {
                (void)net_tcp_ooo_insert(s, seq, payload, payload_len);
            }
            net_tcp_schedule_ack(s);
        }

        if ((flags & NET_TCP_FLAG_FIN) != 0u) {
            s->rcv_nxt = seq + 1u;
            (void)net_tcp_send_pure_ack(s);
            s->peer_closed = 1u;
            s->state = NET_TCP_STATE_CLOSE_WAIT;
            s->rtx_valid = 0u;
        }
        return;
    }

    if (s->state == NET_TCP_STATE_CLOSE_WAIT) {
        if (payload_len > 0u) {
            if (seq == s->rcv_nxt) {
                s->rcv_nxt = seq + payload_len;
            }
            (void)net_tcp_send_pure_ack(s);
        }
        return;
    }

    if (s->state == NET_TCP_STATE_FIN_WAIT_1) {
        if ((flags & NET_TCP_FLAG_ACK) != 0u && ack == s->snd_nxt) {
            s->state = NET_TCP_STATE_FIN_WAIT_2;
        }
        if ((flags & NET_TCP_FLAG_FIN) != 0u) {
            s->rcv_nxt = seq + 1u;
            (void)net_tcp_send_pure_ack(s);
            if ((flags & NET_TCP_FLAG_ACK) != 0u && ack == s->snd_nxt) {
                s->state = NET_TCP_STATE_TIME_WAIT;
                s->time_wait_ms = NET_TCP_TIME_WAIT_MS;
            } else {
                s->state = NET_TCP_STATE_CLOSING;
            }
            s->rtx_valid = 0u;
        }
        return;
    }

    if (s->state == NET_TCP_STATE_FIN_WAIT_2) {
        if ((flags & NET_TCP_FLAG_FIN) != 0u) {
            s->rcv_nxt = seq + 1u;
            (void)net_tcp_send_pure_ack(s);
            s->state = NET_TCP_STATE_TIME_WAIT;
            s->time_wait_ms = NET_TCP_TIME_WAIT_MS;
            s->rtx_valid = 0u;
        }
        return;
    }

    if (s->state == NET_TCP_STATE_CLOSING) {
        if ((flags & NET_TCP_FLAG_ACK) != 0u && ack == s->snd_nxt) {
            s->state = NET_TCP_STATE_TIME_WAIT;
            s->time_wait_ms = NET_TCP_TIME_WAIT_MS;
            s->rtx_valid = 0u;
        }
        if ((flags & NET_TCP_FLAG_FIN) != 0u) {
            s->rcv_nxt = seq + 1u;
            (void)net_tcp_send_pure_ack(s);
        }
        return;
    }

    if (s->state == NET_TCP_STATE_TIME_WAIT) {
        if ((flags & NET_TCP_FLAG_FIN) != 0u) {
            s->rcv_nxt = seq + 1u;
            (void)net_tcp_send_pure_ack(s);
            s->time_wait_ms = NET_TCP_TIME_WAIT_MS;
        }
        return;
    }

    if (s->state == NET_TCP_STATE_LAST_ACK) {
        if ((flags & NET_TCP_FLAG_ACK) != 0u && ack == s->snd_nxt) {
            s->state = NET_TCP_STATE_CLOSED;
            s->rtx_valid = 0u;
        }
    }
}

static uint32_t net_now_ms32(void) {
    return (uint32_t)pit_get_uptime_ms();
}

static int net_arp_is_expired(const net_arp_entry_t* e, uint32_t now_ms) {
    return e && e->valid && (uint32_t)(now_ms - e->age) >= NET_ARP_TTL_MS;
}

static int net_send_raw_internal(const void* data, uint16_t len) {
    if (g_default_netif.driver_ops && g_default_netif.driver_ops->linkoutput) {
        int rc = g_default_netif.driver_ops->linkoutput(data, len);
        if (rc == 0) g_default_netif.tx_frames++;
        return rc;
    }
    return -1;
}

static int net_send_eth(const uint8_t* dst_mac, uint16_t eth_type, const void* payload, uint16_t payload_len) {
    if ((!payload && payload_len > 0) || payload_len > 1500u) return -1;

    uint8_t src_mac[6];
    uint8_t frame[1514];
    uint16_t frame_len;

    net_get_mac(src_mac);
    memcpy(frame, dst_mac, 6);
    memcpy(frame + 6, src_mac, 6);
    net_write_be16(frame + 12, eth_type);
    if (payload_len > 0) {
        memcpy(frame + 14, payload, payload_len);
    }

    frame_len = (uint16_t)(14u + payload_len);
    if (frame_len < 60u) {
        memset(frame + frame_len, 0, (size_t)(60u - frame_len));
        frame_len = 60u;
    }

    return net_send_raw_internal(frame, frame_len);
}

static int net_ipv4_is_zero(const uint8_t ip[4]) {
    return (ip[0] | ip[1] | ip[2] | ip[3]) == 0u;
}

static int net_ipv4_is_loopback(const uint8_t ip[4]) {
    return ip[0] == 127u;
}

static int net_ipv4_is_self(const uint8_t ip[4]) {
    if (memcmp(ip, g_ipv4_addr, 4) == 0 && !net_ipv4_is_zero(g_ipv4_addr)) return 1;
    for (uint32_t i = 0; i < g_netif_count; ++i) {
        netif_t* nif = g_netifs[i];
        if (!nif) continue;
        if (net_ipv4_is_zero(nif->ipv4_addr)) continue;
        if (memcmp(nif->ipv4_addr, ip, 4) == 0) return 1;
    }
    return 0;
}

static int net_ipv4_is_local(const uint8_t ip[4]) {
    /* "local to this host": our primary NIC IP, any netif IP, or 127.0.0.0/8. */
    if (net_ipv4_is_loopback(ip)) return 1;
    return net_ipv4_is_self(ip);
}

static int net_ipv4_is_broadcast(const uint8_t ip[4]) {
    return ip[0] == 255u && ip[1] == 255u && ip[2] == 255u && ip[3] == 255u;
}

static int net_ipv4_is_same_subnet_mask(const uint8_t lhs[4], const uint8_t rhs[4], const uint8_t mask[4]) {
    for (uint32_t i = 0; i < 4u; ++i) {
        if ((lhs[i] & mask[i]) != (rhs[i] & mask[i])) return 0;
    }
    return 1;
}

static int netif_register(netif_t* nif) {
    if (!nif) return -1;
    if (g_netif_count >= NET_MAX_NETIFS) return -1;
    g_netifs[g_netif_count++] = nif;
    return 0;
}

static netif_t* netif_pick_egress(const uint8_t dst_ip[4]) {
    if (!dst_ip) return NULL;

    for (uint32_t i = 0; i < g_netif_count; ++i) {
        netif_t* nif = g_netifs[i];
        if (!nif) continue;
        if ((nif->flags & (NET_NETIF_FLAG_UP | NET_NETIF_FLAG_LINK_UP)) != (NET_NETIF_FLAG_UP | NET_NETIF_FLAG_LINK_UP)) continue;
        if (net_ipv4_is_zero(nif->ipv4_addr)) continue;

        {
            int same_subnet = 1;
            for (uint32_t b = 0; b < 4u; ++b) {
                if ((nif->ipv4_addr[b] & nif->ipv4_netmask[b]) != (dst_ip[b] & nif->ipv4_netmask[b])) {
                    same_subnet = 0;
                    break;
                }
            }
            if (same_subnet || net_ipv4_is_broadcast(dst_ip)) return nif;
        }
    }

    for (uint32_t i = 0; i < g_netif_count; ++i) {
        netif_t* nif = g_netifs[i];
        if (!nif) continue;
        if ((nif->flags & (NET_NETIF_FLAG_UP | NET_NETIF_FLAG_LINK_UP)) != (NET_NETIF_FLAG_UP | NET_NETIF_FLAG_LINK_UP)) continue;
        if (!net_ipv4_is_zero(nif->ipv4_gateway)) return nif;
    }

    return &g_default_netif;
}

static int net_ipv4_select_next_hop(const uint8_t dst_ip[4], uint8_t out_next_hop[4]) {
    netif_t* nif;
    if (!dst_ip || !out_next_hop) return -1;
    if (net_ipv4_is_zero(g_ipv4_addr) || net_ipv4_is_zero(dst_ip)) return -1;

    nif = netif_pick_egress(dst_ip);
    if (!nif) return -1;

    if (net_ipv4_is_zero(nif->ipv4_addr)) return -1;

    if (net_ipv4_is_broadcast(dst_ip) || net_ipv4_is_same_subnet_mask(nif->ipv4_addr, dst_ip, nif->ipv4_netmask)) {
        memcpy(out_next_hop, dst_ip, 4);
        return 0;
    }

    if (net_ipv4_is_zero(nif->ipv4_gateway)) return -1;
    memcpy(out_next_hop, nif->ipv4_gateway, 4);
    return 0;
}

static int net_arp_find_ip(const uint8_t ip[4]) {
    uint32_t now_ms = net_now_ms32();

    for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; ++i) {
        if (net_arp_is_expired(&g_arp_cache[i], now_ms)) {
            g_arp_cache[i].valid = 0;
            continue;
        }
        if (g_arp_cache[i].valid && memcmp(g_arp_cache[i].ip, ip, 4) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void net_arp_cache_insert(const uint8_t ip[4], const uint8_t mac[6]) {
    int existing = net_arp_find_ip(ip);
    uint32_t slot = 0;

    g_arp_tick++;

    if (existing >= 0) {
        slot = (uint32_t)existing;
    } else {
        uint32_t oldest_tick = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; ++i) {
            if (!g_arp_cache[i].valid) {
                slot = i;
                break;
            }
            if (g_arp_cache[i].age < oldest_tick) {
                oldest_tick = g_arp_cache[i].age;
                slot = i;
            }
        }
    }

    memcpy(g_arp_cache[slot].ip, ip, 4);
    memcpy(g_arp_cache[slot].mac, mac, 6);
    g_arp_cache[slot].age = net_now_ms32();
    g_arp_cache[slot].valid = 1;
}

static uint16_t g_ipv4_next_id = 1u;

static uint16_t net_ipv4_alloc_id(void) {
    uint16_t id = g_ipv4_next_id++;
    if (g_ipv4_next_id == 0u) g_ipv4_next_id = 1u;
    return id;
}

static int net_send_ipv4_fragment(const uint8_t* dst_mac,
                                  const uint8_t src_ip[4],
                                  const uint8_t dst_ip[4],
                                  uint8_t proto,
                                  uint16_t ip_id,
                                  uint16_t frag_off_units,
                                  int more_frags,
                                  const uint8_t* frag_payload,
                                  uint16_t frag_len,
                                  int loopback) {
    uint8_t pkt[20 + 1500];
    uint16_t total_len = (uint16_t)(20u + frag_len);
    uint16_t flags_frag = (uint16_t)(frag_off_units & 0x1FFFu);
    if (more_frags) flags_frag |= 0x2000u;

    if (frag_len > NET_IPV4_FRAG_PAYLOAD) return -1;

    memset(pkt, 0, 20);
    pkt[0] = 0x45u;
    pkt[1] = 0u;
    net_write_be16(pkt + 2, total_len);
    net_write_be16(pkt + 4, ip_id);
    net_write_be16(pkt + 6, flags_frag);
    pkt[8] = 64u;
    pkt[9] = proto;
    memcpy(pkt + 12, src_ip, 4);
    memcpy(pkt + 16, dst_ip, 4);
    net_write_be16(pkt + 10, net_checksum16(pkt, 20));
    memcpy(pkt + 20, frag_payload, frag_len);

    if (loopback) {
        uint8_t frame[14 + sizeof(pkt)];
        uint16_t frame_len = (uint16_t)(14u + total_len);
        memset(frame, 0, 12);
        net_write_be16(frame + 12, NET_ETH_TYPE_IPV4);
        memcpy(frame + 14, pkt, total_len);
        net_rx_enqueue_frame(frame, frame_len);
        return 0;
    }

    return net_send_eth(dst_mac, NET_ETH_TYPE_IPV4, pkt, total_len);
}

static int net_send_ipv4(const uint8_t* dst_mac,
                         const uint8_t dst_ip[4],
                         uint8_t proto,
                         const uint8_t* l4_payload,
                         uint16_t l4_len) {
    uint8_t src_ip[4];
    uint16_t ip_id;
    uint16_t remaining;
    uint16_t offset;
    int loopback;

    if (!dst_mac || !dst_ip || !l4_payload || l4_len == 0u || l4_len > NET_IPV4_TX_MAX_PAYLOAD) return -1;

    loopback = net_ipv4_is_loopback(dst_ip) || net_ipv4_is_self(dst_ip);

    /* A configured IPv4 address is only required for packets that leave the host. */
    if (!loopback && net_ipv4_is_zero(g_ipv4_addr)) return -1;

    /* Use the loopback dst as src for 127.0.0.0/8 so the peer sees a loopback origin. */
    if (net_ipv4_is_loopback(dst_ip)) {
        memcpy(src_ip, dst_ip, 4);
    } else {
        memcpy(src_ip, g_ipv4_addr, 4);
    }

    if (l4_len <= NET_IPV4_FRAG_PAYLOAD) {
        /* Single-fragment path: DF=1, no fragmentation offset. */
        uint8_t pkt[20 + 1500];
        uint16_t total_len = (uint16_t)(20u + l4_len);
        memset(pkt, 0, 20);
        pkt[0] = 0x45u;
        pkt[1] = 0u;
        net_write_be16(pkt + 2, total_len);
        net_write_be16(pkt + 4, 0u);
        net_write_be16(pkt + 6, 0x4000u); /* DF=1 */
        pkt[8] = 64u;
        pkt[9] = proto;
        memcpy(pkt + 12, src_ip, 4);
        memcpy(pkt + 16, dst_ip, 4);
        net_write_be16(pkt + 10, net_checksum16(pkt, 20));
        memcpy(pkt + 20, l4_payload, l4_len);

        if (loopback) {
            uint8_t frame[14 + sizeof(pkt)];
            uint16_t frame_len = (uint16_t)(14u + total_len);
            memset(frame, 0, 12);
            net_write_be16(frame + 12, NET_ETH_TYPE_IPV4);
            memcpy(frame + 14, pkt, total_len);
            net_rx_enqueue_frame(frame, frame_len);
            return 0;
        }
        return net_send_eth(dst_mac, NET_ETH_TYPE_IPV4, pkt, total_len);
    }

    /* Multi-fragment path: chunks of NET_IPV4_FRAG_PAYLOAD (mult of 8), last may be shorter. */
    ip_id = net_ipv4_alloc_id();
    remaining = l4_len;
    offset = 0u;
    while (remaining > 0u) {
        uint16_t frag_len = (remaining > NET_IPV4_FRAG_PAYLOAD) ? NET_IPV4_FRAG_PAYLOAD : remaining;
        int more = (frag_len < remaining) ? 1 : 0;
        uint16_t off_units = (uint16_t)(offset / 8u);
        int rc = net_send_ipv4_fragment(dst_mac, src_ip, dst_ip, proto, ip_id, off_units, more,
                                        l4_payload + offset, frag_len, loopback);
        if (rc != 0) return rc;
        offset = (uint16_t)(offset + frag_len);
        remaining = (uint16_t)(remaining - frag_len);
    }
    return 0;
}

static int net_send_icmp_echo(const uint8_t* dst_mac,
                              const uint8_t dst_ip[4],
                              uint8_t icmp_type,
                              uint16_t id,
                              uint16_t seq,
                              const uint8_t* body,
                              uint16_t body_len) {
    uint8_t icmp[8 + 56];
    uint16_t icmp_len;

    if (body_len > 56u) body_len = 56u;
    icmp_len = (uint16_t)(8u + body_len);

    memset(icmp, 0, 8);
    icmp[0] = icmp_type;
    icmp[1] = 0;
    net_write_be16(icmp + 4, id);
    net_write_be16(icmp + 6, seq);
    if (body_len > 0u && body) {
        memcpy(icmp + 8, body, body_len);
    }
    net_write_be16(icmp + 2, net_checksum16(icmp, icmp_len));

    return net_send_ipv4(dst_mac, dst_ip, NET_IP_PROTO_ICMP, icmp, icmp_len);
}

static int net_send_arp_reply(const uint8_t dst_mac[6], const uint8_t dst_ip[4]) {
    uint8_t src_mac[6];
    uint8_t pkt[28];

    if (!dst_mac || !dst_ip || net_ipv4_is_zero(g_ipv4_addr)) return -1;

    net_get_mac(src_mac);

    net_write_be16(pkt + 0, NET_ARP_HTYPE_ETHERNET);
    net_write_be16(pkt + 2, NET_ETH_TYPE_IPV4);
    pkt[4] = 6;
    pkt[5] = 4;
    net_write_be16(pkt + 6, NET_ARP_OP_REPLY);
    memcpy(pkt + 8, src_mac, 6);
    memcpy(pkt + 14, g_ipv4_addr, 4);
    memcpy(pkt + 18, dst_mac, 6);
    memcpy(pkt + 24, dst_ip, 4);

    if (net_send_eth(dst_mac, NET_ETH_TYPE_ARP, pkt, sizeof(pkt)) == 0) {
        g_arp_stats.tx_arp_replies++;
        return 0;
    }

    return -1;
}

static int net_icmp_matches_pending_error(const uint8_t* icmp, uint16_t icmp_len) {
    const uint8_t* inner_ip;
    const uint8_t* inner_icmp;
    uint8_t inner_ihl_words;
    uint16_t inner_ihl;
    uint16_t inner_total_len;
    uint16_t id;
    uint16_t seq;

    if (!icmp || icmp_len < 8u + 20u + 8u) return 0;

    inner_ip = icmp + 8;
    inner_ihl_words = (uint8_t)(inner_ip[0] & 0x0Fu);
    inner_ihl = (uint16_t)(inner_ihl_words * 4u);
    if ((inner_ip[0] >> 4) != 4u || inner_ihl < 20u) return 0;
    if (icmp_len < 8u + inner_ihl + 8u) return 0;

    inner_total_len = net_read_be16(inner_ip + 2);
    if (inner_total_len < inner_ihl + 8u) return 0;
    if (inner_ip[9] != NET_IP_PROTO_ICMP) return 0;
    if (memcmp(inner_ip + 12, g_ipv4_addr, 4) != 0) return 0;
    if (memcmp(inner_ip + 16, g_ping_expect_ip, 4) != 0) return 0;

    inner_icmp = inner_ip + inner_ihl;
    if (inner_icmp[0] != NET_ICMP_ECHO_REQUEST || inner_icmp[1] != 0u) return 0;

    id = net_read_be16(inner_icmp + 4);
    seq = net_read_be16(inner_icmp + 6);
    return id == g_ping_expect_id && seq == g_ping_expect_seq;
}

static void net_arp_input(const uint8_t* frame, uint16_t len) {
    const uint8_t* arp;
    uint16_t htype;
    uint16_t ptype;
    uint16_t op;
    const uint8_t* sha;
    const uint8_t* spa;
    const uint8_t* tpa;

    if (len < 42u) {
        g_arp_stats.dropped_frames++;
        return;
    }

    arp = frame + 14;
    htype = net_read_be16(arp + 0);
    ptype = net_read_be16(arp + 2);
    op = net_read_be16(arp + 6);

    if (htype != NET_ARP_HTYPE_ETHERNET || ptype != NET_ETH_TYPE_IPV4 || arp[4] != 6u || arp[5] != 4u) {
        g_arp_stats.dropped_frames++;
        return;
    }

    sha = arp + 8;
    spa = arp + 14;
    tpa = arp + 24;

    g_arp_stats.rx_arp_packets++;
    if (op == NET_ARP_OP_REQUEST) g_arp_stats.rx_arp_requests++;
    if (op == NET_ARP_OP_REPLY) g_arp_stats.rx_arp_replies++;

    net_arp_cache_insert(spa, sha);

    if (op == NET_ARP_OP_REQUEST && !net_ipv4_is_zero(g_ipv4_addr) && memcmp(tpa, g_ipv4_addr, 4) == 0) {
        (void)net_send_arp_reply(sha, spa);
    }
}

static void etharp_input(const uint8_t* frame, uint16_t len) {
    net_arp_input(frame, len);
}

static void udp_input(const uint8_t src_ip[4], const uint8_t* udp, uint16_t udp_len) {
    net_handle_udp_ipv4(src_ip, udp, udp_len);
}

static void tcp_input(const uint8_t src_ip[4], const uint8_t dst_ip[4], const uint8_t* tcp, uint16_t tcp_len) {
    net_handle_tcp_ipv4(src_ip, dst_ip, tcp, tcp_len);
}

static void ip4_input(const uint8_t* frame, uint16_t len) {
    net_ipv4_input(frame, len);
}

static int net_ipv4_reassemble(const uint8_t* frame, uint16_t len, uint8_t* out_pkt, uint16_t* out_len) {
    const uint8_t* ip = frame + 14;
    const uint8_t* src_ip = ip + 12;
    const uint8_t* dst_ip = ip + 16;
    uint8_t ihl_words = (uint8_t)(ip[0] & 0x0Fu);
    uint16_t ihl = (uint16_t)(ihl_words * 4u);
    uint16_t total_len = net_read_be16(ip + 2);
    uint16_t frag = net_read_be16(ip + 6);
    uint16_t frag_off = (uint16_t)((frag & 0x1FFFu) * 8u);
    uint8_t mf = (uint8_t)((frag & 0x2000u) != 0u);
    uint16_t id = net_read_be16(ip + 4);
    uint16_t payload_len;
    const uint8_t* payload;
    int idx = -1;
    int free_idx = -1;
    uint32_t now = net_now_ms32();

    if (!frame || !out_pkt || !out_len || len < 34u) return -1;
    if (total_len < ihl || ihl < 20u || total_len > 1500u || (14u + total_len) > len) return -1;
    payload_len = (uint16_t)(total_len - ihl);
    payload = ip + ihl;
    if (payload_len == 0u) return -1;
    if ((uint32_t)frag_off + payload_len > NET_IPV4_REASM_MAX_DATA) return -1;

    for (uint32_t i = 0; i < NET_IPV4_REASM_CTX_MAX; ++i) {
        net_ipv4_reasm_ctx_t* c = &g_ipv4_reasm[i];
        if (c->used && (uint32_t)(now - c->last_update_ms) > NET_IPV4_REASM_TIMEOUT_MS) {
            c->used = 0u;
            g_p2_stats.ipv4_frag_reasm_drop++;
        }
        if (!c->used && free_idx < 0) free_idx = (int)i;
        if (c->used && c->id == id && c->proto == ip[9] &&
            memcmp(c->src_ip, src_ip, 4) == 0 && memcmp(c->dst_ip, dst_ip, 4) == 0) {
            idx = (int)i;
        }
    }

    if (idx < 0) {
        if (frag_off != 0u && !mf) return 0;
        if (free_idx < 0) {
            g_p2_stats.ipv4_frag_reasm_drop++;
            return -1;
        }
        idx = free_idx;
        memset(&g_ipv4_reasm[idx], 0, sizeof(g_ipv4_reasm[idx]));
        g_ipv4_reasm[idx].used = 1u;
        memcpy(g_ipv4_reasm[idx].src_ip, src_ip, 4);
        memcpy(g_ipv4_reasm[idx].dst_ip, dst_ip, 4);
        g_ipv4_reasm[idx].id = id;
        g_ipv4_reasm[idx].proto = ip[9];
    }

    {
        net_ipv4_reasm_ctx_t* c = &g_ipv4_reasm[idx];
        c->last_update_ms = now;
        if (frag_off == 0u) {
            c->have_first = 1u;
            c->ihl = ihl;
            memcpy(c->header, ip, ihl);
        }

        for (uint16_t i = 0; i < payload_len; ++i) {
            if (c->present[frag_off + i]) {
                c->used = 0u;
                g_p2_stats.ipv4_frag_reasm_drop++;
                return -1;
            }
        }
        memcpy(c->data + frag_off, payload, payload_len);
        memset(c->present + frag_off, 1, payload_len);

        if (!mf) {
            c->have_last = 1u;
            c->total_payload_len = (uint16_t)(frag_off + payload_len);
        }

        if (c->have_first && c->have_last) {
            for (uint16_t i = 0; i < c->total_payload_len; ++i) {
                if (!c->present[i]) return 0;
            }

            memcpy(out_pkt, frame, 14u);
            memcpy(out_pkt + 14u, c->header, c->ihl);
            net_write_be16(out_pkt + 14u + 2u, (uint16_t)(c->ihl + c->total_payload_len));
            net_write_be16(out_pkt + 14u + 6u, 0u);
            net_write_be16(out_pkt + 14u + 10u, 0u);
            net_write_be16(out_pkt + 14u + 10u, net_checksum16(out_pkt + 14u, c->ihl));
            memcpy(out_pkt + 14u + c->ihl, c->data, c->total_payload_len);
            *out_len = (uint16_t)(14u + c->ihl + c->total_payload_len);
            c->used = 0u;
            g_p2_stats.ipv4_frag_reasm_ok++;
            return 1;
        }
    }

    return 0;
}

static void net_icmp_input(const uint8_t* frame,
                           uint16_t len,
                           const uint8_t* ip,
                           uint16_t ihl,
                           uint16_t total_len,
                           const uint8_t src_ip[4],
                           const uint8_t dst_ip[4]) {
    const uint8_t* icmp = ip + ihl;
    uint16_t icmp_len = (uint16_t)(total_len - ihl);

    (void)len;

    if (icmp_len < 8u) {
        g_arp_stats.dropped_frames++;
        return;
    }
    if (net_checksum16(icmp, icmp_len) != 0u) {
        g_arp_stats.dropped_frames++;
        g_p2_stats.ipv4_malformed++;
        return;
    }

    if (icmp[0] == NET_ICMP_DEST_UNREACH) g_p2_stats.icmp_rx_unreach++;
    if (icmp[0] == NET_ICMP_TIME_EXCEEDED) g_p2_stats.icmp_rx_timeex++;
    if (icmp[0] == 12u) g_p2_stats.icmp_rx_param++;

    if (icmp[0] == NET_ICMP_ECHO_REPLY && icmp[1] == 0u) {
        uint16_t id = net_read_be16(icmp + 4);
        uint16_t seq = net_read_be16(icmp + 6);
        if (g_ping_waiting && id == g_ping_expect_id && seq == g_ping_expect_seq &&
            memcmp(src_ip, g_ping_expect_ip, 4) == 0) {
            g_ping_result = NET_PING_OK;
            g_ping_waiting = 0;
        }
        return;
    }

    if (g_ping_waiting &&
        (icmp[0] == NET_ICMP_DEST_UNREACH || icmp[0] == NET_ICMP_TIME_EXCEEDED) &&
        net_icmp_matches_pending_error(icmp, icmp_len)) {
        g_ping_result = (icmp[0] == NET_ICMP_DEST_UNREACH)
                        ? NET_PING_ERR_DEST_UNREACH
                        : NET_PING_ERR_TIME_EXCEEDED;
        g_ping_waiting = 0;
        return;
    }

    if (g_ping_waiting && net_icmp_matches_pending_error(icmp, icmp_len)) {
        g_ping_result = NET_PING_ERR_ICMP;
        g_ping_waiting = 0;
        return;
    }

    if (icmp[0] == NET_ICMP_ECHO_REQUEST && icmp[1] == 0u && net_ipv4_is_local(dst_ip)) {
        uint16_t id = net_read_be16(icmp + 4);
        uint16_t seq = net_read_be16(icmp + 6);
        uint16_t body_len = (uint16_t)(icmp_len - 8u);
        (void)net_send_icmp_echo(frame + 6, src_ip, NET_ICMP_ECHO_REPLY, id, seq, icmp + 8, body_len);
    }
}

static void net_ipv4_input(const uint8_t* frame, uint16_t len) {
    uint8_t reasm_frame[14u + 60u + NET_IPV4_REASM_MAX_DATA];
    const uint8_t* frame_use = frame;
    uint16_t len_use = len;
    const uint8_t* ip;
    const uint8_t* src_ip;
    const uint8_t* dst_ip;
    uint8_t ihl_words;
    uint16_t ihl;
    uint16_t total_len;
    uint16_t frag;
    uint8_t is_fragment;

    if (len_use < 34u) {
        g_arp_stats.dropped_frames++;
        g_p2_stats.ipv4_malformed++;
        return;
    }

    ip = frame_use + 14u;
    ihl_words = (uint8_t)(ip[0] & 0x0Fu);
    ihl = (uint16_t)(ihl_words * 4u);
    total_len = net_read_be16(ip + 2u);

    if ((ip[0] >> 4) != 4u || ihl < 20u || ihl > 60u || total_len < ihl || total_len > 1500u || (14u + total_len) > len_use) {
        g_arp_stats.dropped_frames++;
        g_p2_stats.ipv4_malformed++;
        return;
    }
    if (net_checksum16(ip, ihl) != 0u) {
        g_arp_stats.dropped_frames++;
        g_p2_stats.ipv4_malformed++;
        return;
    }
    if ((net_read_be16(ip + 6u) & 0x8000u) != 0u) {
        g_arp_stats.dropped_frames++;
        g_p2_stats.ipv4_malformed++;
        return;
    }

    frag = net_read_be16(ip + 6u);
    is_fragment = (uint8_t)(((frag & 0x2000u) != 0u) || ((frag & 0x1FFFu) != 0u));
    if (is_fragment) {
        int r;
        g_p2_stats.ipv4_frag_rx++;
        r = net_ipv4_reassemble(frame_use, len_use, reasm_frame, &len_use);
        if (r <= 0) {
            if (r < 0) {
                g_arp_stats.dropped_frames++;
            }
            return;
        }
        frame_use = reasm_frame;
        ip = frame_use + 14u;
        ihl_words = (uint8_t)(ip[0] & 0x0Fu);
        ihl = (uint16_t)(ihl_words * 4u);
        total_len = net_read_be16(ip + 2u);
    }

    src_ip = ip + 12u;
    dst_ip = ip + 16u;

    /* Drop only if we have no configured IP AND the frame is not a loopback packet. */
    if (net_ipv4_is_zero(g_ipv4_addr) && !net_ipv4_is_loopback(dst_ip)) return;
    if (!net_ipv4_is_local(dst_ip) && !net_ipv4_is_broadcast(dst_ip)) {
        g_arp_stats.dropped_frames++;
        return;
    }

    if (ip[9] == NET_IP_PROTO_ICMP) {
        net_icmp_input(frame_use, len_use, ip, ihl, total_len, src_ip, dst_ip);
    } else if (ip[9] == NET_IP_PROTO_UDP) {
        udp_input(src_ip, ip + ihl, (uint16_t)(total_len - ihl));
    } else if (ip[9] == NET_IP_PROTO_TCP) {
        tcp_input(src_ip, dst_ip, ip + ihl, (uint16_t)(total_len - ihl));
    } else {
        g_p2_stats.ipv4_malformed++;
    }
}

static void net_ethernet_input(const uint8_t* frame, uint16_t len) {
    uint16_t eth_type;

    if (len < 14u) {
        g_arp_stats.dropped_frames++;
        return;
    }

    eth_type = net_read_be16(frame + 12);
    if (eth_type == NET_ETH_TYPE_ARP) {
        etharp_input(frame, len);
        return;
    }
    if (eth_type == NET_ETH_TYPE_IPV4) {
        ip4_input(frame, len);
    }
}

void net_core_input(const uint8_t* frame, uint16_t len) {
    net_ethernet_input(frame, len);
}

static int16_t net_pbuf_alloc(void) {
    int16_t idx = g_rx_pbuf_free_head;
    if (idx < 0) return -1;
    g_rx_pbuf_free_head = g_rx_pbuf_pool[idx].next_free;
    g_rx_pbuf_pool[idx].next_free = -1;
    g_rx_pbuf_pool[idx].chain_next = -1;
    g_rx_pbuf_pool[idx].len = 0u;
    g_rx_pbuf_pool[idx].tot_len = 0u;
    g_rx_pbuf_pool[idx].refcount = 1u;
    return idx;
}

static void net_pbuf_free_chain(int16_t head_idx) {
    int16_t idx = head_idx;
    while (idx >= 0 && idx < (int16_t)NET_RX_PBUF_POOL_SIZE) {
        int16_t next = g_rx_pbuf_pool[idx].chain_next;
        if (g_rx_pbuf_pool[idx].refcount > 1u) {
            g_rx_pbuf_pool[idx].refcount--;
            break;
        }
        g_rx_pbuf_pool[idx].next_free = g_rx_pbuf_free_head;
        g_rx_pbuf_pool[idx].chain_next = -1;
        g_rx_pbuf_pool[idx].len = 0u;
        g_rx_pbuf_pool[idx].tot_len = 0u;
        g_rx_pbuf_pool[idx].refcount = 0u;
        g_rx_pbuf_free_head = idx;
        idx = next;
    }
}

static void net_rx_defer_init(void) {
    memset(g_rx_pbuf_pool, 0, sizeof(g_rx_pbuf_pool));
    memset(&g_rx_defer_queue, 0, sizeof(g_rx_defer_queue));
    memset(&g_rx_defer_stats, 0, sizeof(g_rx_defer_stats));

    for (uint16_t i = 0; i < NET_RX_PBUF_POOL_SIZE; ++i) {
        g_rx_pbuf_pool[i].len = 0u;
        g_rx_pbuf_pool[i].tot_len = 0u;
        g_rx_pbuf_pool[i].refcount = 0u;
        g_rx_pbuf_pool[i].chain_next = -1;
        g_rx_pbuf_pool[i].next_free = (i + 1u < NET_RX_PBUF_POOL_SIZE) ? (int16_t)(i + 1u) : -1;
    }
    g_rx_pbuf_free_head = 0;
}

static void net_rx_enqueue_frame(const uint8_t* frame, uint16_t len) {
    uint32_t flags;
    int16_t pidx;
    int16_t tail;

    if (!frame || len == 0u) {
        g_rx_defer_stats.drop_invalid++;
        g_default_netif.rx_drops++;
        return;
    }
    if (len > NET_RX_PBUF_DATA_SIZE) {
        g_rx_defer_stats.drop_too_large++;
        g_default_netif.rx_drops++;
        return;
    }

    flags = net_irq_save();
    if (g_rx_defer_queue.count >= NET_RX_DEFER_QUEUE_LEN) {
        g_rx_defer_stats.drop_queue_full++;
        g_default_netif.rx_drops++;
        net_irq_restore(flags);
        return;
    }

    pidx = net_pbuf_alloc();
    if (pidx < 0) {
        g_rx_defer_stats.drop_pool_empty++;
        g_default_netif.rx_drops++;
        net_irq_restore(flags);
        return;
    }

    tail = pidx;
    g_rx_pbuf_pool[tail].len = len;
    memcpy(g_rx_pbuf_pool[tail].data, frame, len);
    g_rx_pbuf_pool[pidx].tot_len = len;

    g_rx_defer_queue.slots[g_rx_defer_queue.tail] = (uint16_t)pidx;
    g_rx_defer_queue.tail = (uint16_t)((g_rx_defer_queue.tail + 1u) % NET_RX_DEFER_QUEUE_LEN);
    g_rx_defer_queue.count++;
    g_rx_defer_stats.enqueued++;
    net_irq_restore(flags);
}

uint32_t net_poll(uint32_t budget) {
    uint32_t processed = 0;
    uint32_t limit = budget == 0u ? NET_RX_POLL_BUDGET_DEFAULT : budget;

    while (processed < limit) {
        uint32_t flags;
        uint16_t pidx;

        flags = net_irq_save();
        if (g_rx_defer_queue.count == 0u) {
            net_irq_restore(flags);
            break;
        }

        pidx = g_rx_defer_queue.slots[g_rx_defer_queue.head];
        g_rx_defer_queue.head = (uint16_t)((g_rx_defer_queue.head + 1u) % NET_RX_DEFER_QUEUE_LEN);
        g_rx_defer_queue.count--;
        g_rx_defer_stats.dequeued++;
        net_irq_restore(flags);

        if (pidx >= NET_RX_PBUF_POOL_SIZE) {
            g_rx_defer_stats.drop_invalid++;
            g_default_netif.rx_drops++;
            continue;
        }

        net_core_input(g_rx_pbuf_pool[pidx].data, g_rx_pbuf_pool[pidx].len);
        g_default_netif.rx_frames++;

        flags = net_irq_save();
        net_pbuf_free_chain((int16_t)pidx);
        net_irq_restore(flags);

        processed++;
    }

    return processed;
}

static void net_timers_run_deferred(void) {
    uint32_t flags;

    flags = net_irq_save();
    if (g_timer_state.link_refresh_due) {
        uint8_t prev_link = g_timer_state.last_link_up;
        g_timer_state.link_refresh_due = 0u;
        net_irq_restore(flags);

        netif_sync_link_state();

        flags = net_irq_save();
        g_timer_state.link_refresh_count++;
        g_timer_state.last_link_up = (g_default_netif.flags & NET_NETIF_FLAG_LINK_UP) != 0u ? 1u : 0u;
        if (g_timer_state.last_link_up != prev_link) {
            g_timer_state.link_state_changes++;
            g_default_netif.link_changes++;
        }
    }
    net_irq_restore(flags);

    for (uint32_t i = 0; i < NET_TCP_MAX_SOCKETS; ++i) {
        uint8_t payload[NET_TCP_RTX_PAYLOAD_MAX];
        uint8_t remote_ip[4];
        uint16_t local_port;
        uint16_t remote_port;
        uint16_t payload_len;
        uint8_t tx_flags;
        uint8_t retries;
        uint32_t tx_seq;
        uint32_t tx_ack;
        int should_send = 0;
        int send_rc;

        flags = net_irq_save();
        if (!g_tcp_sockets[i].in_use || !g_tcp_sockets[i].rtx_valid || !g_tcp_sockets[i].rtx_due) {
            net_irq_restore(flags);
            continue;
        }

        retries = g_tcp_sockets[i].rtx_retries;
        if (retries >= NET_TCP_RTX_MAX_RETRIES) {
            g_tcp_sockets[i].rtx_valid = 0u;
            g_tcp_sockets[i].rtx_due = 0u;
            g_tcp_sockets[i].state = NET_TCP_STATE_RESET;
            g_tcp_sockets[i].connect_result = NET_TCP_ERR_TIMEOUT;
            g_timer_state.tcp_rtx_timeout_count++;
            net_irq_restore(flags);
            continue;
        }
        if (retries == 0u) {
            net_tcp_cc_on_timeout(&g_tcp_sockets[i]);
        }

        memcpy(remote_ip, g_tcp_sockets[i].remote_ip, 4);
        local_port = g_tcp_sockets[i].local_port;
        remote_port = g_tcp_sockets[i].remote_port;
        tx_seq = g_tcp_sockets[i].rtx_seq;
        tx_ack = g_tcp_sockets[i].rtx_ack;
        tx_flags = g_tcp_sockets[i].rtx_flags;
        payload_len = g_tcp_sockets[i].rtx_payload_len;
        if (payload_len > 0u) {
            memcpy(payload, g_tcp_sockets[i].rtx_payload, payload_len);
        }

        g_tcp_sockets[i].rtx_due = 0u;
        g_tcp_sockets[i].rtx_retries = (uint8_t)(retries + 1u);
        should_send = 1;
        net_irq_restore(flags);

        if (!should_send) continue;

        send_rc = net_tcp_send_segment(remote_ip,
                                       local_port,
                                       remote_port,
                                       tx_seq,
                                       tx_ack,
                                       tx_flags,
                                       payload_len > 0u ? payload : NULL,
                                       payload_len);

        flags = net_irq_save();
        if (g_tcp_sockets[i].in_use && g_tcp_sockets[i].rtx_valid) {
            if (send_rc == 0) {
                uint32_t next_rto = g_tcp_sockets[i].rtx_rto_ms < 4000u ? (g_tcp_sockets[i].rtx_rto_ms * 2u) : 4000u;
                g_tcp_sockets[i].rtx_elapsed_ms = 0u;
                g_tcp_sockets[i].rtx_rto_ms = next_rto;
                g_timer_state.tcp_rtx_sent_count++;
            } else {
                g_tcp_sockets[i].rtx_due = 1u;
            }
        }
        net_irq_restore(flags);
    }

    /* Delayed ACK flush + keepalive probes. */
    for (uint32_t i = 0; i < NET_TCP_MAX_SOCKETS; ++i) {
        net_tcp_socket_state_t* s = &g_tcp_sockets[i];
        if (!s->in_use) continue;

        if (s->delack_pending && s->delack_ms == 0u) {
            (void)net_tcp_send_pure_ack(s);
        }

        if (!s->keepalive_enabled) continue;
        if (s->state != NET_TCP_STATE_ESTABLISHED && s->state != NET_TCP_STATE_CLOSE_WAIT) continue;

        if (s->keepalive_probes == 0u) {
            if (s->idle_ms >= NET_TCP_KEEPALIVE_IDLE_MS) {
                (void)net_tcp_send_segment_window(s->remote_ip,
                                                  s->local_port,
                                                  s->remote_port,
                                                  s->snd_una - 1u,
                                                  s->rcv_nxt,
                                                  NET_TCP_FLAG_ACK,
                                                  net_tcp_advertised_window(s),
                                                  NULL,
                                                  0u);
                s->keepalive_probes = 1u;
                s->keepalive_probe_ms = NET_TCP_KEEPALIVE_PROBE_MS;
            }
        } else if (s->keepalive_probe_ms == 0u) {
            if (s->keepalive_probes >= NET_TCP_KEEPALIVE_MAX_PROBES) {
                s->state = NET_TCP_STATE_RESET;
                s->connect_result = NET_TCP_ERR_TIMEOUT;
                s->rtx_valid = 0u;
            } else {
                (void)net_tcp_send_segment_window(s->remote_ip,
                                                  s->local_port,
                                                  s->remote_port,
                                                  s->snd_una - 1u,
                                                  s->rcv_nxt,
                                                  NET_TCP_FLAG_ACK,
                                                  net_tcp_advertised_window(s),
                                                  NULL,
                                                  0u);
                s->keepalive_probes++;
                s->keepalive_probe_ms = NET_TCP_KEEPALIVE_PROBE_MS;
            }
        }
    }
}

void net_worker_step(void) {
    (void)net_poll(0u);
    net_timers_run_deferred();
    net_dhcp_client_step();
}

void net_timers_tick(uint32_t elapsed_ms) {
    uint32_t flags;

    if (elapsed_ms == 0u) return;

    flags = net_irq_save();

    g_net_timer_accum_ms += elapsed_ms;
    g_timer_state.arp_accum_ms += elapsed_ms;
    g_timer_state.link_refresh_accum_ms += elapsed_ms;

    if (g_timer_state.link_refresh_accum_ms >= NET_TIMER_LINK_REFRESH_MS) {
        g_timer_state.link_refresh_accum_ms -= NET_TIMER_LINK_REFRESH_MS;
        g_timer_state.link_refresh_due = 1u;
    }

    if (g_dhcp_timer.lease_remaining_ms > 0u) {
        g_dhcp_timer.lease_remaining_ms = g_dhcp_timer.lease_remaining_ms > elapsed_ms
                                              ? (g_dhcp_timer.lease_remaining_ms - elapsed_ms)
                                              : 0u;
    }
    if (g_dhcp_timer.t1_remaining_ms > 0u) {
        g_dhcp_timer.t1_remaining_ms = g_dhcp_timer.t1_remaining_ms > elapsed_ms
                                           ? (g_dhcp_timer.t1_remaining_ms - elapsed_ms)
                                           : 0u;
    }
    if (g_dhcp_timer.t2_remaining_ms > 0u) {
        g_dhcp_timer.t2_remaining_ms = g_dhcp_timer.t2_remaining_ms > elapsed_ms
                                           ? (g_dhcp_timer.t2_remaining_ms - elapsed_ms)
                                           : 0u;
    }

    if (g_dhcp_timer.retry_remaining_ms > 0u) {
        g_dhcp_timer.retry_remaining_ms = g_dhcp_timer.retry_remaining_ms > elapsed_ms
                                              ? (g_dhcp_timer.retry_remaining_ms - elapsed_ms)
                                              : 0u;
        if (g_dhcp_timer.retry_remaining_ms == 0u && g_dhcp_timer.lease_remaining_ms > 0u) {
            uint32_t next_retry = NET_DHCP_RETRY_BASE_MS;
            uint32_t shift = g_dhcp_timer.retry_count;
            if (shift > 6u) shift = 6u;
            next_retry <<= shift;
            if (next_retry > NET_DHCP_RETRY_MAX_MS) next_retry = NET_DHCP_RETRY_MAX_MS;
            g_dhcp_timer.retry_remaining_ms = next_retry;
            g_dhcp_timer.retry_count++;
            g_dhcp_timer.retry_due_count++;
        }
    } else if (g_dhcp_timer.lease_remaining_ms > 0u &&
               (g_dhcp_timer.t1_remaining_ms == 0u || g_dhcp_timer.t2_remaining_ms == 0u)) {
        g_dhcp_timer.retry_remaining_ms = NET_DHCP_RETRY_BASE_MS;
    }

    for (uint32_t i = 0; i < NET_TCP_MAX_SOCKETS; ++i) {
        net_tcp_socket_state_t* s = &g_tcp_sockets[i];
        if (s->in_use && s->state == NET_TCP_STATE_TIME_WAIT && s->time_wait_ms > 0u) {
            s->time_wait_ms = s->time_wait_ms > elapsed_ms ? (s->time_wait_ms - elapsed_ms) : 0u;
            if (s->time_wait_ms == 0u) {
                s->state = NET_TCP_STATE_CLOSED;
                s->rtx_valid = 0u;
            }
        }
        if (s->in_use && s->delack_pending) {
            uint16_t dec = elapsed_ms > 0xFFFFu ? 0xFFFFu : (uint16_t)elapsed_ms;
            s->delack_ms = s->delack_ms > dec ? (uint16_t)(s->delack_ms - dec) : 0u;
        }
        if (s->in_use && net_tcp_socket_is_active_state(s->state)) {
            s->idle_ms += elapsed_ms;
            if (s->keepalive_enabled && s->keepalive_probes > 0u && s->keepalive_probe_ms > 0u) {
                uint16_t dec2 = elapsed_ms > 0xFFFFu ? 0xFFFFu : (uint16_t)elapsed_ms;
                s->keepalive_probe_ms = s->keepalive_probe_ms > dec2
                                            ? (uint16_t)(s->keepalive_probe_ms - dec2)
                                            : 0u;
            }
        }
        if (!s->in_use || !s->rtx_valid || s->rtx_due) continue;
        s->rtx_elapsed_ms += elapsed_ms;
        g_timer_state.tcp_rtx_scan_count++;
        if (s->rtx_elapsed_ms >= s->rtx_rto_ms) {
            s->rtx_due = 1u;
            g_timer_state.tcp_rtx_due_count++;
        }
    }

    if (g_timer_state.arp_accum_ms < 1000u) {
        net_irq_restore(flags);
        return;
    }
    g_timer_state.arp_accum_ms -= 1000u;
    net_irq_restore(flags);

    {
        uint32_t now_ms = net_now_ms32();
        for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; ++i) {
            if (net_arp_is_expired(&g_arp_cache[i], now_ms)) {
                g_arp_cache[i].valid = 0;
            }
        }
    }
}

void net_get_timer_debug(net_timer_debug_t* out_debug) {
    uint32_t tcp_rtx_active = 0u;

    if (!out_debug) return;

    for (uint32_t i = 0; i < NET_TCP_MAX_SOCKETS; ++i) {
        if (g_tcp_sockets[i].in_use && g_tcp_sockets[i].rtx_valid) tcp_rtx_active++;
    }

    out_debug->timer_tick_ms_accum = g_net_timer_accum_ms;
    out_debug->link_refresh_period_ms = NET_TIMER_LINK_REFRESH_MS;
    out_debug->link_refresh_count = g_timer_state.link_refresh_count;
    out_debug->link_state_changes = g_timer_state.link_state_changes;
    out_debug->dhcp_lease_remaining_ms = g_dhcp_timer.lease_remaining_ms;
    out_debug->dhcp_t1_remaining_ms = g_dhcp_timer.t1_remaining_ms;
    out_debug->dhcp_t2_remaining_ms = g_dhcp_timer.t2_remaining_ms;
    out_debug->dhcp_retry_remaining_ms = g_dhcp_timer.retry_remaining_ms;
    out_debug->dhcp_retry_count = g_dhcp_timer.retry_count;
    out_debug->tcp_rtx_scans = g_timer_state.tcp_rtx_scan_count;
    out_debug->tcp_rtx_due = g_timer_state.tcp_rtx_due_count;
    out_debug->tcp_rtx_sent = g_timer_state.tcp_rtx_sent_count;
    out_debug->tcp_rtx_timeout = g_timer_state.tcp_rtx_timeout_count;
    out_debug->tcp_rtx_active = tcp_rtx_active;
}

void net_get_p2_stats(net_p2_stats_t* out_stats) {
    if (!out_stats) return;
    *out_stats = g_p2_stats;
}

void net_dhcp_note_lease(uint32_t lease_seconds, uint32_t t1_seconds, uint32_t t2_seconds) {
    uint64_t lease_ms = (uint64_t)lease_seconds * 1000u;
    uint64_t t1_ms;
    uint64_t t2_ms;

    if (lease_seconds == 0u) {
        g_dhcp_timer.lease_remaining_ms = 0u;
        g_dhcp_timer.t1_remaining_ms = 0u;
        g_dhcp_timer.t2_remaining_ms = 0u;
        g_dhcp_timer.retry_remaining_ms = 0u;
        g_dhcp_timer.retry_count = 0u;
        g_dhcp_timer.retry_due_count = 0u;
        return;
    }

    if (lease_ms > 0xFFFFFFFFu) lease_ms = 0xFFFFFFFFu;

    if (t1_seconds == 0u) {
        t1_ms = lease_ms / 2u;
    } else {
        t1_ms = (uint64_t)t1_seconds * 1000u;
    }

    if (t2_seconds == 0u) {
        t2_ms = (lease_ms * 875u) / 1000u;
    } else {
        t2_ms = (uint64_t)t2_seconds * 1000u;
    }

    if (t1_ms > lease_ms) t1_ms = lease_ms;
    if (t2_ms > lease_ms) t2_ms = lease_ms;
    if (t2_ms < t1_ms) t2_ms = t1_ms;

    g_dhcp_timer.lease_remaining_ms = (uint32_t)lease_ms;
    g_dhcp_timer.t1_remaining_ms = (uint32_t)t1_ms;
    g_dhcp_timer.t2_remaining_ms = (uint32_t)t2_ms;
    g_dhcp_timer.retry_remaining_ms = 0u;
    g_dhcp_timer.retry_count = 0u;
    g_dhcp_timer.retry_due_count = 0u;
    g_dhcp_client.retries = 0u;
    g_dhcp_client.waiting_ack = 0u;
}

static int net_dhcp_client_send_request(int broadcast) {
    uint8_t request[300];
    uint8_t mac[6];
    uint8_t dst_ip[4] = {255u, 255u, 255u, 255u};
    uint16_t off = 240u;

    if (g_dhcp_client.socket_id < 0) return -1;
    if (!g_dhcp_client.enabled || net_ipv4_is_zero(g_dhcp_client.lease_ip)) return -1;

    net_get_mac(mac);
    memset(request, 0, sizeof(request));
    request[0] = 1u;
    request[1] = 1u;
    request[2] = 6u;
    request[4] = (uint8_t)((g_dhcp_client.xid >> 24) & 0xFFu);
    request[5] = (uint8_t)((g_dhcp_client.xid >> 16) & 0xFFu);
    request[6] = (uint8_t)((g_dhcp_client.xid >> 8) & 0xFFu);
    request[7] = (uint8_t)(g_dhcp_client.xid & 0xFFu);
    request[10] = broadcast ? 0x80u : 0x00u;
    memcpy(request + 28, mac, 6);
    request[236] = 99u;
    request[237] = 130u;
    request[238] = 83u;
    request[239] = 99u;

    request[off++] = 53u; request[off++] = 1u; request[off++] = 3u;
    request[off++] = 50u; request[off++] = 4u;
    request[off++] = g_dhcp_client.lease_ip[0];
    request[off++] = g_dhcp_client.lease_ip[1];
    request[off++] = g_dhcp_client.lease_ip[2];
    request[off++] = g_dhcp_client.lease_ip[3];
    if (!broadcast && !net_ipv4_is_zero(g_dhcp_client.server_ip)) {
        request[off++] = 54u; request[off++] = 4u;
        request[off++] = g_dhcp_client.server_ip[0];
        request[off++] = g_dhcp_client.server_ip[1];
        request[off++] = g_dhcp_client.server_ip[2];
        request[off++] = g_dhcp_client.server_ip[3];
        memcpy(dst_ip, g_dhcp_client.server_ip, 4);
    }
    request[off++] = 55u; request[off++] = 3u; request[off++] = 1u; request[off++] = 3u; request[off++] = 6u;
    request[off++] = 255u;

    if (net_udp_socket_sendto(g_dhcp_client.socket_id, dst_ip, 67u, request, off) != NET_UDP_OK) {
        return -1;
    }

    g_dhcp_client.waiting_ack = 1u;
    g_dhcp_client.ack_wait_ms = NET_DHCP_ACK_WAIT_MS;
    g_dhcp_client.xid++;
    return 0;
}

static void net_dhcp_client_on_ack(const uint8_t* pkt, uint16_t len) {
    uint8_t msg_type = 0u;
    uint8_t mask[4] = {255u, 255u, 255u, 0u};
    uint8_t gw[4] = {0u, 0u, 0u, 0u};
    uint8_t server[4] = {0u, 0u, 0u, 0u};
    uint8_t yiaddr[4];
    uint32_t lease_s = 3600u;
    uint32_t t1_s = 0u;
    uint32_t t2_s = 0u;
    uint32_t rx_xid;
    uint32_t expect_xid;

    if (!pkt || len < 244u) return;
    if (pkt[0] != 2u || pkt[1] != 1u || pkt[2] != 6u) return;
    if (pkt[236] != 99u || pkt[237] != 130u || pkt[238] != 83u || pkt[239] != 99u) return;
    rx_xid = ((uint32_t)pkt[4] << 24) |
             ((uint32_t)pkt[5] << 16) |
             ((uint32_t)pkt[6] << 8) |
             (uint32_t)pkt[7];
    expect_xid = g_dhcp_client.xid - 1u;
    if (rx_xid != expect_xid) return;

    if (net_dhcp_parse_options(pkt, len, &msg_type, server, mask, gw, &lease_s, &t1_s, &t2_s) != 0) return;

    if (msg_type == 6u) {
        g_dhcp_client.state = NET_DHCP_CLIENT_FAILED;
        g_dhcp_client.waiting_ack = 0u;
        return;
    }
    if (msg_type != 5u) return;

    memcpy(yiaddr, pkt + 16, 4);
    if (net_ipv4_is_zero(yiaddr)) return;

    memcpy(g_dhcp_client.lease_ip, yiaddr, 4);
    memcpy(g_dhcp_client.netmask, mask, 4);
    memcpy(g_dhcp_client.gateway, gw, 4);
    if (!net_ipv4_is_zero(server)) memcpy(g_dhcp_client.server_ip, server, 4);

    net_set_ipv4(yiaddr[0], yiaddr[1], yiaddr[2], yiaddr[3]);
    net_set_ipv4_netmask(mask[0], mask[1], mask[2], mask[3]);
    if (!net_ipv4_is_zero(gw)) {
        net_set_ipv4_gateway(gw[0], gw[1], gw[2], gw[3]);
    }
    net_dhcp_note_lease(lease_s, t1_s, t2_s);

    g_dhcp_client.waiting_ack = 0u;
    g_dhcp_client.retries = 0u;
    g_dhcp_client.state = NET_DHCP_CLIENT_BOUND;
}

void net_dhcp_client_seed(const uint8_t server_ip[4],
                          const uint8_t lease_ip[4],
                          const uint8_t netmask[4],
                          const uint8_t gateway[4],
                          uint32_t lease_seconds,
                          uint32_t t1_seconds,
                          uint32_t t2_seconds) {
    uint8_t fallback_ip[4];
    uint8_t fallback_mask[4];
    uint8_t fallback_gw[4];
    uint8_t had_fallback = g_dhcp_client.have_fallback;

    if (!server_ip || !lease_ip || !netmask || !gateway) return;

    if (!had_fallback) {
        net_get_ipv4(fallback_ip);
        net_get_ipv4_netmask(fallback_mask);
        net_get_ipv4_gateway(fallback_gw);
    } else {
        memcpy(fallback_ip, g_dhcp_client.fallback_ip, 4);
        memcpy(fallback_mask, g_dhcp_client.fallback_mask, 4);
        memcpy(fallback_gw, g_dhcp_client.fallback_gw, 4);
    }

    memset(&g_dhcp_client, 0, sizeof(g_dhcp_client));
    memcpy(g_dhcp_client.fallback_ip, fallback_ip, 4);
    memcpy(g_dhcp_client.fallback_mask, fallback_mask, 4);
    memcpy(g_dhcp_client.fallback_gw, fallback_gw, 4);
    g_dhcp_client.have_fallback = 1u;
    g_dhcp_client.enabled = 1u;
    g_dhcp_client.state = NET_DHCP_CLIENT_BOUND;
    g_dhcp_client.socket_id = -1;
    g_dhcp_client.xid = (uint32_t)pit_get_uptime_ms() ^ 0xA5A5D1C3u;
    memcpy(g_dhcp_client.server_ip, server_ip, 4);
    memcpy(g_dhcp_client.lease_ip, lease_ip, 4);
    memcpy(g_dhcp_client.netmask, netmask, 4);
    memcpy(g_dhcp_client.gateway, gateway, 4);
    net_dhcp_note_lease(lease_seconds, t1_seconds, t2_seconds);
}

static void net_dhcp_client_step(void) {
    uint8_t response[600];
    uint16_t rx_len = 0u;
    net_udp_endpoint_t from;
    int rc;

    if (!g_dhcp_client.enabled) return;

    if (g_dhcp_client.socket_id < 0) {
        int sock = net_udp_socket_open();
        if (sock >= 0 && net_udp_socket_bind(sock, 68u) >= 0) {
            g_dhcp_client.socket_id = sock;
        } else {
            if (sock >= 0) (void)net_udp_socket_close(sock);
            return;
        }
    }

    if (g_dhcp_client.waiting_ack) {
        rc = net_udp_socket_recvfrom(g_dhcp_client.socket_id,
                                     response,
                                     sizeof(response),
                                     &rx_len,
                                     &from,
                                     0u);
        if (rc >= 0 && rx_len >= 244u) {
            net_dhcp_client_on_ack(response, rx_len);
        }
        if (g_dhcp_client.waiting_ack && g_dhcp_client.ack_wait_ms > 0u) {
            g_dhcp_client.ack_wait_ms = g_dhcp_client.ack_wait_ms > 10u ? (g_dhcp_client.ack_wait_ms - 10u) : 0u;
            if (g_dhcp_client.ack_wait_ms == 0u) {
                g_dhcp_client.waiting_ack = 0u;
                g_dhcp_client.retries++;
            }
        }
    }

    if (g_dhcp_client.waiting_ack) return;

    if (g_dhcp_timer.retry_due_count != g_dhcp_client.seen_retry_due) {
        int broadcast = 0;
        g_dhcp_client.seen_retry_due = g_dhcp_timer.retry_due_count;

        if (g_dhcp_timer.t2_remaining_ms == 0u) {
            g_dhcp_client.state = NET_DHCP_CLIENT_REBINDING;
            broadcast = 1;
        } else if (g_dhcp_timer.t1_remaining_ms == 0u) {
            g_dhcp_client.state = NET_DHCP_CLIENT_RENEWING;
        }

        if (g_dhcp_client.retries < NET_DHCP_MAX_RETRIES) {
            (void)net_dhcp_client_send_request(broadcast);
        } else if (g_dhcp_timer.lease_remaining_ms == 0u) {
            if (g_dhcp_client.have_fallback) {
                net_set_ipv4(g_dhcp_client.fallback_ip[0], g_dhcp_client.fallback_ip[1], g_dhcp_client.fallback_ip[2], g_dhcp_client.fallback_ip[3]);
                net_set_ipv4_netmask(g_dhcp_client.fallback_mask[0], g_dhcp_client.fallback_mask[1], g_dhcp_client.fallback_mask[2], g_dhcp_client.fallback_mask[3]);
                net_set_ipv4_gateway(g_dhcp_client.fallback_gw[0], g_dhcp_client.fallback_gw[1], g_dhcp_client.fallback_gw[2], g_dhcp_client.fallback_gw[3]);
            }
            g_dhcp_client.state = NET_DHCP_CLIENT_FAILED;
            g_dhcp_client.enabled = 0u;
            if (g_dhcp_client.socket_id >= 0) {
                (void)net_udp_socket_close(g_dhcp_client.socket_id);
                g_dhcp_client.socket_id = -1;
            }
        }
    }
}

void net_get_rx_defer_stats(net_rx_defer_stats_t* out_stats) {
    if (!out_stats) return;
    *out_stats = g_rx_defer_stats;
}

static void net_on_rx_frame(const uint8_t* frame, uint16_t len) {
    net_rx_enqueue_frame(frame, len);
}

static void netif_sync_link_state(void) {
    uint8_t old_link = (g_default_netif.flags & NET_NETIF_FLAG_LINK_UP) != 0u ? 1u : 0u;

    if (!g_default_netif.driver_ops) {
        g_default_netif.flags = 0u;
        return;
    }

    g_default_netif.flags = NET_NETIF_FLAG_UP;
    if (g_default_netif.driver_ops->link_up && g_default_netif.driver_ops->link_up()) {
        g_default_netif.flags |= NET_NETIF_FLAG_LINK_UP;
    }

    if (old_link && (g_default_netif.flags & NET_NETIF_FLAG_LINK_UP) == 0u) {
        g_dhcp_client.waiting_ack = 0u;
        if (g_dhcp_client.enabled) {
            g_dhcp_client.state = NET_DHCP_CLIENT_REBINDING;
            g_dhcp_timer.retry_remaining_ms = NET_DHCP_RETRY_BASE_MS;
        }
    }
}

const netif_t* net_default_netif(void) {
    return &g_default_netif;
}

int net_core_init(void) {
    pci_initialize();
    net_rx_defer_init();
    memset(&g_dhcp_timer, 0, sizeof(g_dhcp_timer));
    memset(&g_dhcp_client, 0, sizeof(g_dhcp_client));
    g_dhcp_client.socket_id = -1;
    memset(&g_timer_state, 0, sizeof(g_timer_state));
    memset(g_ipv4_reasm, 0, sizeof(g_ipv4_reasm));
    memset(g_dns_cache, 0, sizeof(g_dns_cache));
    memset(&g_p2_stats, 0, sizeof(g_p2_stats));
    memset(g_netifs, 0, sizeof(g_netifs));
    g_netif_count = 0u;
    g_net_timer_accum_ms = 0u;
    memset(g_udp_sockets, 0, sizeof(g_udp_sockets));
    g_udp_next_ephemeral = NET_UDP_EPHEMERAL_START;
    memset(g_tcp_sockets, 0, sizeof(g_tcp_sockets));
    memset(g_tcp_listeners, 0, sizeof(g_tcp_listeners));
    g_tcp_next_ephemeral = NET_TCP_EPHEMERAL_START;

    g_loopback_netif.flags = NET_NETIF_FLAG_UP | NET_NETIF_FLAG_LINK_UP;
    g_loopback_netif.rx_frames = 0u;
    g_loopback_netif.tx_frames = 0u;
    g_loopback_netif.rx_drops = 0u;
    g_loopback_netif.link_changes = 0u;
    (void)netif_register(&g_loopback_netif);

    memset(g_default_netif.hwaddr, 0, sizeof(g_default_netif.hwaddr));
    g_default_netif.driver_ops = NULL;
    g_default_netif.flags = 0u;
    memcpy(g_default_netif.name, "none", 5u);
    (void)netif_register(&g_default_netif);

    if (e1000_initialize() == 0) {
        g_driver = NET_DRIVER_E1000;
        g_default_netif.driver_ops = &g_e1000_ops;
        memcpy(g_default_netif.name, "e1000", 6u);
        g_default_netif.driver_ops->get_mac(g_default_netif.hwaddr);
        netif_sync_link_state();
        g_timer_state.last_link_up = (g_default_netif.flags & NET_NETIF_FLAG_LINK_UP) != 0u ? 1u : 0u;
        e1000_set_rx_callback(net_on_rx_frame);
        return 0;
    }

    if (rtl8139_initialize() == 0) {
        g_driver = NET_DRIVER_RTL8139;
        g_default_netif.driver_ops = &g_rtl8139_ops;
        memcpy(g_default_netif.name, "rtl8139", 8u);
        g_default_netif.driver_ops->get_mac(g_default_netif.hwaddr);
        netif_sync_link_state();
        g_timer_state.last_link_up = (g_default_netif.flags & NET_NETIF_FLAG_LINK_UP) != 0u ? 1u : 0u;
        rtl8139_set_rx_callback(net_on_rx_frame);
        return 0;
    }

    g_driver = NET_DRIVER_NONE;
    netif_sync_link_state();
    g_timer_state.last_link_up = (g_default_netif.flags & NET_NETIF_FLAG_LINK_UP) != 0u ? 1u : 0u;
    klog("net: no supported NIC initialized");
    return -1;
}

int net_initialize(void) {
    return net_core_init();
}

int net_is_ready(void) {
    if (g_default_netif.driver_ops && g_default_netif.driver_ops->is_ready) {
        return g_default_netif.driver_ops->is_ready();
    }
    return 0;
}

int net_link_up(void) {
    if (g_default_netif.driver_ops && g_default_netif.driver_ops->link_up) {
        int up = g_default_netif.driver_ops->link_up();
        netif_sync_link_state();
        return up;
    }
    return 0;
}

const char* net_driver_name(void) {
    return g_default_netif.name;
}

int net_send_test_frame(void) {
    if (g_default_netif.driver_ops && g_default_netif.driver_ops->send_test_frame) {
        return g_default_netif.driver_ops->send_test_frame();
    }
    return -1;
}

void net_get_mac(uint8_t out_mac[6]) {
    if (!out_mac) return;

    if (g_default_netif.driver_ops && g_default_netif.driver_ops->get_mac) {
        g_default_netif.driver_ops->get_mac(g_default_netif.hwaddr);
        memcpy(out_mac, g_default_netif.hwaddr, sizeof(g_default_netif.hwaddr));
        return;
    }

    for (uint32_t i = 0; i < 6; ++i) out_mac[i] = 0;
}

void net_set_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    g_ipv4_addr[0] = a;
    g_ipv4_addr[1] = b;
    g_ipv4_addr[2] = c;
    g_ipv4_addr[3] = d;
}

void net_get_ipv4(uint8_t out_ip[4]) {
    if (!out_ip) return;
    out_ip[0] = g_ipv4_addr[0];
    out_ip[1] = g_ipv4_addr[1];
    out_ip[2] = g_ipv4_addr[2];
    out_ip[3] = g_ipv4_addr[3];
}

void net_set_ipv4_netmask(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    g_ipv4_netmask[0] = a;
    g_ipv4_netmask[1] = b;
    g_ipv4_netmask[2] = c;
    g_ipv4_netmask[3] = d;
}

void net_get_ipv4_netmask(uint8_t out_mask[4]) {
    if (!out_mask) return;
    out_mask[0] = g_ipv4_netmask[0];
    out_mask[1] = g_ipv4_netmask[1];
    out_mask[2] = g_ipv4_netmask[2];
    out_mask[3] = g_ipv4_netmask[3];
}

void net_set_ipv4_gateway(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    g_ipv4_gateway[0] = a;
    g_ipv4_gateway[1] = b;
    g_ipv4_gateway[2] = c;
    g_ipv4_gateway[3] = d;
}

void net_get_ipv4_gateway(uint8_t out_gw[4]) {
    if (!out_gw) return;
    out_gw[0] = g_ipv4_gateway[0];
    out_gw[1] = g_ipv4_gateway[1];
    out_gw[2] = g_ipv4_gateway[2];
    out_gw[3] = g_ipv4_gateway[3];
}

int net_send_arp_request(const uint8_t target_ip[4]) {
    uint8_t src_mac[6];
    uint8_t pkt[28];
    static const uint8_t bcast[6] = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu};

    if (!target_ip || net_ipv4_is_zero(g_ipv4_addr) || !net_is_ready()) return -1;

    net_get_mac(src_mac);

    net_write_be16(pkt + 0, NET_ARP_HTYPE_ETHERNET);
    net_write_be16(pkt + 2, NET_ETH_TYPE_IPV4);
    pkt[4] = 6;
    pkt[5] = 4;
    net_write_be16(pkt + 6, NET_ARP_OP_REQUEST);
    memcpy(pkt + 8, src_mac, 6);
    memcpy(pkt + 14, g_ipv4_addr, 4);
    memset(pkt + 18, 0, 6);
    memcpy(pkt + 24, target_ip, 4);

    if (net_send_eth(bcast, NET_ETH_TYPE_ARP, pkt, sizeof(pkt)) == 0) {
        g_arp_stats.tx_arp_requests++;
        return 0;
    }

    return -1;
}

int net_arp_lookup(const uint8_t ip[4], uint8_t out_mac[6]) {
    int idx;

    if (!ip || !out_mac) return -1;

    /* Loopback and self-addressed packets never travel on the wire. */
    if (net_ipv4_is_loopback(ip) || net_ipv4_is_self(ip)) {
        memset(out_mac, 0, 6);
        return 0;
    }

    idx = net_arp_find_ip(ip);
    if (idx < 0) {
        g_arp_stats.cache_misses++;
        return -1;
    }

    memcpy(out_mac, g_arp_cache[idx].mac, 6);
    g_arp_cache[idx].age = net_now_ms32();
    g_arp_stats.cache_hits++;
    return 0;
}

int net_arp_resolve_retry(const uint8_t ip[4], uint8_t out_mac[6], uint32_t retries, uint32_t wait_ms) {
    uint32_t probes = retries == 0u ? 1u : retries;
    uint32_t delay = wait_ms == 0u ? 100u : wait_ms;

    if (!ip || !out_mac) return -1;
    if (net_arp_lookup(ip, out_mac) == 0) return 0;

    for (uint32_t i = 0; i < probes; ++i) {
        uint32_t elapsed = 0;

        if (net_send_arp_request(ip) != 0) {
            continue;
        }

        while (elapsed < delay) {
            uint32_t step = (delay - elapsed > 10u) ? 10u : (delay - elapsed);
            (void)net_poll(0u);
            pit_sleep(step);
            elapsed += step;
            if (net_arp_lookup(ip, out_mac) == 0) {
                return 0;
            }
        }
    }

    return -1;
}

uint32_t net_get_arp_cache(net_arp_entry_t* out_entries, uint32_t max_entries) {
    uint32_t count = 0;
    uint32_t now_ms = net_now_ms32();

    if (!out_entries || max_entries == 0) return 0;

    for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE && count < max_entries; ++i) {
        if (net_arp_is_expired(&g_arp_cache[i], now_ms)) {
            g_arp_cache[i].valid = 0;
            continue;
        }
        if (!g_arp_cache[i].valid) continue;
        out_entries[count++] = g_arp_cache[i];
    }

    return count;
}

void net_get_arp_stats(net_arp_stats_t* out_stats) {
    if (!out_stats) return;
    *out_stats = g_arp_stats;
}

int net_ping_ipv4(const uint8_t target_ip[4], uint16_t seq, uint32_t timeout_ms) {
    uint8_t dst_mac[6];
    uint8_t next_hop[4];
    uint8_t payload[32];
    uint16_t id = 0xD00Du;
    uint32_t timeout = timeout_ms == 0u ? 1000u : timeout_ms;
    uint32_t elapsed = 0;

    if (!target_ip || net_ipv4_is_zero(target_ip) || net_ipv4_is_broadcast(target_ip)) return NET_PING_ERR_INVALID;
    if (!net_ipv4_is_local(target_ip)) {
        if (!net_is_ready() || net_ipv4_is_zero(g_ipv4_addr)) return NET_PING_ERR_INVALID;
    }

    if (net_ipv4_is_local(target_ip)) {
        memcpy(next_hop, target_ip, 4);
    } else if (net_ipv4_select_next_hop(target_ip, next_hop) != 0) {
        return NET_PING_ERR_INVALID;
    }
    if (net_arp_resolve_retry(next_hop, dst_mac, 3u, 150u) != 0) return NET_PING_ERR_ARP_UNRESOLVED;

    for (uint32_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)(i + 1u);

    memcpy(g_ping_expect_ip, target_ip, 4);
    g_ping_expect_id = id;
    g_ping_expect_seq = seq;
    g_ping_result = NET_PING_ERR_TIMEOUT;
    g_ping_waiting = 1;

    if (net_send_icmp_echo(dst_mac, target_ip, NET_ICMP_ECHO_REQUEST, id, seq, payload, sizeof(payload)) != 0) {
        g_ping_result = NET_PING_ERR_TX;
        g_ping_waiting = 0;
        return NET_PING_ERR_TX;
    }

    while (elapsed < timeout) {
        if (!g_ping_waiting) return g_ping_result;
        (void)net_poll(0u);
        pit_sleep(10u);
        elapsed += 10u;
    }

    g_ping_result = NET_PING_ERR_TIMEOUT;
    g_ping_waiting = 0;
    return NET_PING_ERR_TIMEOUT;
}

int net_udp_socket_open(void) {
    for (uint32_t i = 0; i < NET_UDP_MAX_SOCKETS; ++i) {
        if (!g_udp_sockets[i].in_use) {
            memset(&g_udp_sockets[i], 0, sizeof(g_udp_sockets[i]));
            g_udp_sockets[i].in_use = 1;
            return (int)i;
        }
    }
    return NET_UDP_ERR_NO_SOCKETS;
}

int net_udp_socket_close(int socket_id) {
    if (!net_udp_socket_id_valid(socket_id)) return NET_UDP_ERR_INVALID;
    if (!g_udp_sockets[socket_id].in_use) return NET_UDP_ERR_INVALID;

    memset(&g_udp_sockets[socket_id], 0, sizeof(g_udp_sockets[socket_id]));
    return NET_UDP_OK;
}

int net_udp_socket_bind(int socket_id, uint16_t local_port) {
    uint16_t port = local_port;

    if (!net_udp_socket_id_valid(socket_id)) return NET_UDP_ERR_INVALID;
    if (!g_udp_sockets[socket_id].in_use) return NET_UDP_ERR_INVALID;

    if (port == 0u) {
        port = net_udp_allocate_ephemeral_port();
        if (port == 0u) return NET_UDP_ERR_NO_SOCKETS;
    } else if (net_udp_port_in_use(port, socket_id)) {
        return NET_UDP_ERR_PORT_IN_USE;
    }

    g_udp_sockets[socket_id].bound = 1;
    g_udp_sockets[socket_id].local_port = port;
    return (int)port;
}

int net_udp_socket_sendto(int socket_id,
                          const uint8_t dst_ip[4],
                          uint16_t dst_port,
                          const void* payload,
                          uint16_t payload_len) {
    uint8_t l4[8 + 1472];
    uint8_t dst_mac[6];
    uint8_t next_hop[4];
    uint16_t src_port;
    uint16_t udp_len;
    const uint8_t* bytes = (const uint8_t*)payload;
    static const uint8_t bcast[6] = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu};

    if (!net_udp_socket_id_valid(socket_id) || !dst_ip || (!payload && payload_len > 0u)) return NET_UDP_ERR_INVALID;
    if (!g_udp_sockets[socket_id].in_use || !g_udp_sockets[socket_id].bound) return NET_UDP_ERR_NOT_BOUND;
    if (dst_port == 0u || payload_len > 1472u) return NET_UDP_ERR_INVALID;
    /* Loopback does not need a live NIC or a configured IPv4 address. */
    if (!net_ipv4_is_local(dst_ip)) {
        if (!net_is_ready() || net_ipv4_is_zero(g_ipv4_addr)) return NET_UDP_ERR_NOT_READY;
    }

    src_port = g_udp_sockets[socket_id].local_port;
    udp_len = (uint16_t)(8u + payload_len);

    net_write_be16(l4 + 0, src_port);
    net_write_be16(l4 + 2, dst_port);
    net_write_be16(l4 + 4, udp_len);
    net_write_be16(l4 + 6, 0u);
    if (payload_len > 0u) {
        memcpy(l4 + 8, bytes, payload_len);
    }
    net_write_be16(l4 + 6, net_udp_checksum_ipv4(g_ipv4_addr, dst_ip, l4, udp_len));

    if (net_ipv4_is_local(dst_ip)) {
        const uint8_t* loop_src = net_ipv4_is_loopback(dst_ip) ? dst_ip : g_ipv4_addr;
        return net_udp_deliver_datagram(dst_port, loop_src, src_port, bytes, payload_len)
                   ? NET_UDP_OK
                   : NET_UDP_ERR_TX;
    }

    if (net_ipv4_is_broadcast(dst_ip)) {
        if (net_send_ipv4(bcast, dst_ip, NET_IP_PROTO_UDP, l4, udp_len) == 0) return NET_UDP_OK;
        return NET_UDP_ERR_TX;
    }

    if (net_ipv4_select_next_hop(dst_ip, next_hop) != 0) return NET_UDP_ERR_INVALID;
    if (net_arp_resolve_retry(next_hop, dst_mac, 3u, 150u) != 0) return NET_UDP_ERR_ARP_UNRESOLVED;
    if (net_send_ipv4(dst_mac, dst_ip, NET_IP_PROTO_UDP, l4, udp_len) == 0) return NET_UDP_OK;
    return NET_UDP_ERR_TX;
}

int net_udp_socket_recvfrom(int socket_id,
                            void* out_payload,
                            uint16_t payload_capacity,
                            uint16_t* out_payload_len,
                            net_udp_endpoint_t* out_from,
                            uint32_t timeout_ms) {
    net_udp_socket_state_t* sock;
    uint32_t elapsed = 0;
    uint32_t timeout = timeout_ms;

    if (!net_udp_socket_id_valid(socket_id)) return NET_UDP_ERR_INVALID;
    sock = &g_udp_sockets[socket_id];
    if (!sock->in_use || !sock->bound) return NET_UDP_ERR_NOT_BOUND;
    if (!out_payload && payload_capacity > 0u) return NET_UDP_ERR_INVALID;

    for (;;) {
        if (sock->count > 0u) {
            uint32_t slot = sock->head;
            net_udp_datagram_t* dgram = &sock->queue[slot];
            uint16_t copy_len = dgram->stored_len;

            if (copy_len > payload_capacity) copy_len = payload_capacity;
            if (copy_len > 0u && out_payload) {
                memcpy(out_payload, dgram->data, copy_len);
            }
            if (out_payload_len) *out_payload_len = copy_len;
            if (out_from) {
                memcpy(out_from->ip, dgram->src_ip, 4);
                out_from->port = dgram->src_port;
            }

            sock->head = (sock->head + 1u) % NET_UDP_QUEUE_DEPTH;
            sock->count--;

            if (dgram->len > copy_len) return NET_UDP_ERR_MSG_TRUNC;
            return NET_UDP_OK;
        }

        if (timeout == 0u) return NET_UDP_ERR_WOULD_BLOCK;
        if (elapsed >= timeout) return NET_UDP_ERR_WOULD_BLOCK;

        (void)net_poll(0u);
        pit_sleep(1u);
        elapsed++;
    }
}

static int net_dns_cache_lookup(const char* hostname, uint8_t out_ip[4], int* out_negative) {
    uint32_t now = net_now_ms32();
    if (!hostname || !out_negative) return 0;

    for (uint32_t i = 0; i < NET_DNS_CACHE_SIZE; ++i) {
        net_dns_cache_entry_t* e = &g_dns_cache[i];
        if (!e->valid) continue;
        if ((uint32_t)(now - e->expires_at_ms) < 0x80000000u && now >= e->expires_at_ms) {
            e->valid = 0u;
            continue;
        }
        if (strcmp(e->name, hostname) != 0) continue;
        e->last_used_ms = now;
        *out_negative = e->negative ? 1 : 0;
        if (!e->negative && out_ip) memcpy(out_ip, e->ip, 4);
        return 1;
    }
    return 0;
}

static void net_dns_cache_store(const char* hostname, const uint8_t ip[4], uint32_t ttl_seconds, int negative) {
    int slot = -1;
    uint32_t now = net_now_ms32();
    uint32_t oldest_expire = 0xFFFFFFFFu;
    uint32_t ttl_s = ttl_seconds;

    if (!hostname) return;
    if (!negative && !ip) return;

    if (ttl_s < NET_DNS_TTL_MIN_S) ttl_s = NET_DNS_TTL_MIN_S;
    if (ttl_s > NET_DNS_TTL_MAX_S) ttl_s = NET_DNS_TTL_MAX_S;

    for (uint32_t i = 0; i < NET_DNS_CACHE_SIZE; ++i) {
        net_dns_cache_entry_t* e = &g_dns_cache[i];
        if (e->valid && strcmp(e->name, hostname) == 0) {
            slot = (int)i;
            break;
        }
        if (!e->valid && slot < 0) slot = (int)i;
        if (e->valid && e->expires_at_ms < oldest_expire) {
            oldest_expire = e->expires_at_ms;
            if (slot < 0) slot = (int)i;
        }
    }

    if (slot < 0) return;
    if (g_dns_cache[slot].valid) g_p2_stats.dns_cache_evict++;

    memset(&g_dns_cache[slot], 0, sizeof(g_dns_cache[slot]));
    g_dns_cache[slot].valid = 1u;
    g_dns_cache[slot].negative = negative ? 1u : 0u;
    strncpy(g_dns_cache[slot].name, hostname, NET_DNS_CACHE_NAME_MAX);
    g_dns_cache[slot].name[NET_DNS_CACHE_NAME_MAX] = '\0';
    if (!negative) memcpy(g_dns_cache[slot].ip, ip, 4);
    g_dns_cache[slot].expires_at_ms = now + (ttl_s * 1000u);
    g_dns_cache[slot].last_used_ms = now;
    g_p2_stats.dns_cache_insert++;
}

uint32_t net_dns_cache_count(void) {
    uint32_t count = 0;
    uint32_t now = net_now_ms32();
    for (uint32_t i = 0; i < NET_DNS_CACHE_SIZE; ++i) {
        if (!g_dns_cache[i].valid) continue;
        if ((uint32_t)(now - g_dns_cache[i].expires_at_ms) < 0x80000000u && now >= g_dns_cache[i].expires_at_ms) {
            continue;
        }
        count++;
    }
    return count;
}

uint32_t net_dns_cache_dump(net_dns_cache_debug_entry_t* out_entries, uint32_t max_entries) {
    uint32_t now = net_now_ms32();
    uint32_t count = 0u;

    if (!out_entries || max_entries == 0u) return 0u;

    for (uint32_t i = 0; i < NET_DNS_CACHE_SIZE && count < max_entries; ++i) {
        net_dns_cache_entry_t* e = &g_dns_cache[i];
        if (!e->valid) continue;
        if ((uint32_t)(now - e->expires_at_ms) < 0x80000000u && now >= e->expires_at_ms) continue;

        memset(&out_entries[count], 0, sizeof(out_entries[count]));
        strncpy(out_entries[count].name, e->name, sizeof(out_entries[count].name) - 1u);
        out_entries[count].negative = e->negative;
        if (!e->negative) memcpy(out_entries[count].ip, e->ip, 4);
        out_entries[count].ttl_left_ms = e->expires_at_ms > now ? (e->expires_at_ms - now) : 0u;
        out_entries[count].age_ms = now - e->last_used_ms;
        count++;
    }

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t rank = 0u;
        for (uint32_t j = 0; j < count; ++j) {
            if (out_entries[j].age_ms > out_entries[i].age_ms) rank++;
        }
        out_entries[i].lru_rank = rank;
    }

    return count;
}


int net_dns_query_a(const char* hostname,
                    const uint8_t dns_server_ip[4],
                    uint8_t out_ip[4],
                    uint32_t timeout_ms) {
    uint8_t query[512];
    uint8_t response[512];
    uint16_t query_len = 0;
    uint16_t qname_len = 0;
    uint16_t rx_len = 0;
    uint16_t id;
    uint16_t off;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t flags;
    uint32_t timeout = timeout_ms == 0u ? 800u : timeout_ms;
    int sock = -1;
    int rc;
    net_udp_endpoint_t from;
    uint8_t default_dns[2][4] = {{10u, 0u, 2u, 3u}, {10u, 0u, 2u, 2u}};
    uint32_t server_count = dns_server_ip ? 1u : 2u;
    uint8_t timeout_only = 1u;
    int cache_neg = 0;
    uint32_t start_server = g_dns_next_id % (server_count == 0u ? 1u : server_count);

    if (!hostname || !out_ip) return NET_DNS_ERR_INVALID;
    if (dns_server_ip && net_ipv4_is_zero(dns_server_ip)) return NET_DNS_ERR_INVALID;

    if (net_dns_cache_lookup(hostname, out_ip, &cache_neg)) {
        if (cache_neg) {
            g_p2_stats.dns_cache_neg_hit++;
            return NET_DNS_ERR_NOT_FOUND;
        }
        g_p2_stats.dns_cache_hit++;
        return NET_DNS_OK;
    }
    g_p2_stats.dns_cache_miss++;

    sock = net_udp_socket_open();
    if (sock < 0) return NET_DNS_ERR_SOCKET;

    rc = net_udp_socket_bind(sock, 0u);
    if (rc < 0) {
        (void)net_udp_socket_close(sock);
        return NET_DNS_ERR_SOCKET;
    }

    memset(query, 0, sizeof(query));
    net_write_be16(query + 2, 0x0100u);
    net_write_be16(query + 4, 1u);

    if (net_dns_encode_qname(hostname, query + 12, (uint16_t)(sizeof(query) - 12u), &qname_len) != 0) {
        (void)net_udp_socket_close(sock);
        return NET_DNS_ERR_INVALID;
    }

    query_len = (uint16_t)(12u + qname_len);
    if ((uint16_t)(query_len + 4u) > sizeof(query)) {
        (void)net_udp_socket_close(sock);
        return NET_DNS_ERR_INVALID;
    }
    net_write_be16(query + query_len, 1u);
    net_write_be16(query + query_len + 2u, 1u);
    query_len = (uint16_t)(query_len + 4u);

    for (uint32_t s = 0; s < server_count; ++s) {
        uint32_t server_idx = (start_server + s) % server_count;
        const uint8_t* server = dns_server_ip ? dns_server_ip : default_dns[server_idx];
        if (net_ipv4_is_zero(server)) continue;

        for (uint32_t attempt = 0; attempt < 3u; ++attempt) {
            uint32_t attempt_timeout = timeout;
            if (attempt_timeout > 4000u) attempt_timeout = 4000u;
            id = g_dns_next_id++;
            net_write_be16(query + 0, id);

            rc = net_udp_socket_sendto(sock, server, 53u, query, query_len);
            if (rc != NET_UDP_OK) {
                (void)net_udp_socket_close(sock);
                return NET_DNS_ERR_TX;
            }

            rc = net_udp_socket_recvfrom(sock, response, sizeof(response), &rx_len, &from, attempt_timeout);
            if (rc == NET_UDP_ERR_WOULD_BLOCK) {
                g_p2_stats.dns_query_retry++;
                g_p2_stats.dns_query_timeout++;
                if (timeout < 4000u) {
                    timeout = timeout < 2000u ? timeout * 2u : 4000u;
                    g_p2_stats.dns_query_backoff++;
                }
                continue;
            }
            timeout_only = 0u;
            if (rc < 0) {
                (void)net_udp_socket_close(sock);
                return NET_DNS_ERR_FORMAT;
            }
            if (rx_len < 12u) {
                (void)net_udp_socket_close(sock);
                return NET_DNS_ERR_FORMAT;
            }

            if (net_read_be16(response + 0) != id) {
                continue;
            }

            flags = net_read_be16(response + 2);
            if ((flags & 0x8000u) == 0u) {
                (void)net_udp_socket_close(sock);
                return NET_DNS_ERR_FORMAT;
            }
            if ((flags & 0x000Fu) == 3u) {
                (void)net_udp_socket_close(sock);
                net_dns_cache_store(hostname, NULL, NET_DNS_NEG_TTL_S, 1);
                return NET_DNS_ERR_NOT_FOUND;
            }
            if ((flags & 0x000Fu) != 0u) {
                (void)net_udp_socket_close(sock);
                return NET_DNS_ERR_FORMAT;
            }

            qdcount = net_read_be16(response + 4);
            ancount = net_read_be16(response + 6);
            off = 12u;

            for (uint16_t i = 0; i < qdcount; ++i) {
                if (net_dns_skip_name(response, rx_len, off, &off) != 0) {
                    (void)net_udp_socket_close(sock);
                    return NET_DNS_ERR_FORMAT;
                }
                if ((uint16_t)(off + 4u) > rx_len) {
                    (void)net_udp_socket_close(sock);
                    return NET_DNS_ERR_FORMAT;
                }
                off = (uint16_t)(off + 4u);
            }

            for (uint16_t i = 0; i < ancount; ++i) {
                uint16_t type;
                uint16_t cls;
                uint16_t rdlen;
                uint32_t ttl;

                if (net_dns_skip_name(response, rx_len, off, &off) != 0) {
                    (void)net_udp_socket_close(sock);
                    return NET_DNS_ERR_FORMAT;
                }
                if ((uint16_t)(off + 10u) > rx_len) {
                    (void)net_udp_socket_close(sock);
                    return NET_DNS_ERR_FORMAT;
                }

                type = net_read_be16(response + off);
                cls = net_read_be16(response + off + 2u);
                ttl = net_read_be32(response + off + 4u);
                rdlen = net_read_be16(response + off + 8u);
                off = (uint16_t)(off + 10u);

                if ((uint16_t)(off + rdlen) > rx_len) {
                    (void)net_udp_socket_close(sock);
                    return NET_DNS_ERR_FORMAT;
                }
                if (type == 1u && cls == 1u && rdlen == 4u) {
                    memcpy(out_ip, response + off, 4);
                    net_dns_cache_store(hostname, out_ip, ttl, 0);
                    (void)net_udp_socket_close(sock);
                    return NET_DNS_OK;
                }
                off = (uint16_t)(off + rdlen);
            }
        }
    }

    (void)net_udp_socket_close(sock);
    if (timeout_only) return NET_DNS_ERR_TIMEOUT;
    return NET_DNS_ERR_NOT_FOUND;
}

int net_tcp_listener_open(void) {
    for (uint32_t i = 0; i < NET_TCP_MAX_LISTENERS; ++i) {
        if (!g_tcp_listeners[i].in_use) {
            memset(&g_tcp_listeners[i], 0, sizeof(g_tcp_listeners[i]));
            g_tcp_listeners[i].in_use = 1u;
            return (int)i;
        }
    }
    return NET_TCP_ERR_NO_SOCKETS;
}

int net_tcp_listener_close(int listener_id) {
    if (listener_id < 0 || listener_id >= (int)NET_TCP_MAX_LISTENERS) return NET_TCP_ERR_INVALID;
    if (!g_tcp_listeners[listener_id].in_use) return NET_TCP_ERR_INVALID;
    memset(&g_tcp_listeners[listener_id], 0, sizeof(g_tcp_listeners[listener_id]));
    return NET_TCP_OK;
}

int net_tcp_listener_bind(int listener_id, uint16_t local_port) {
    if (listener_id < 0 || listener_id >= (int)NET_TCP_MAX_LISTENERS) return NET_TCP_ERR_INVALID;
    if (!g_tcp_listeners[listener_id].in_use || local_port == 0u) return NET_TCP_ERR_INVALID;
    if (net_tcp_port_in_use(local_port)) return NET_TCP_ERR_INVALID;
    g_tcp_listeners[listener_id].local_port = local_port;
    g_tcp_listeners[listener_id].bound = 1u;
    return NET_TCP_OK;
}

int net_tcp_listener_listen(int listener_id, uint16_t backlog) {
    uint16_t bl = backlog;
    if (listener_id < 0 || listener_id >= (int)NET_TCP_MAX_LISTENERS) return NET_TCP_ERR_INVALID;
    if (!g_tcp_listeners[listener_id].in_use || !g_tcp_listeners[listener_id].bound) return NET_TCP_ERR_INVALID;
    if (bl == 0u) bl = 1u;
    if (bl > NET_TCP_ACCEPTQ_MAX) bl = NET_TCP_ACCEPTQ_MAX;
    g_tcp_listeners[listener_id].backlog = bl;
    return NET_TCP_OK;
}

int net_tcp_listener_accept(int listener_id, uint32_t timeout_ms, int* out_socket_id) {
    uint32_t elapsed = 0u;
    uint32_t timeout = timeout_ms;
    net_tcp_listener_state_t* l;

    if (!out_socket_id) return NET_TCP_ERR_INVALID;
    if (listener_id < 0 || listener_id >= (int)NET_TCP_MAX_LISTENERS) return NET_TCP_ERR_INVALID;
    l = &g_tcp_listeners[listener_id];
    if (!l->in_use || l->backlog == 0u) return NET_TCP_ERR_INVALID;

    for (;;) {
        if (l->qcount > 0u) {
            int sid = l->pending[l->qhead];
            l->qhead = (uint16_t)((l->qhead + 1u) % NET_TCP_ACCEPTQ_MAX);
            l->qcount--;
            if (sid >= 0 && sid < (int)NET_TCP_MAX_SOCKETS && g_tcp_sockets[sid].in_use) {
                *out_socket_id = sid;
                return NET_TCP_OK;
            }
            continue;
        }
        if (timeout == 0u || elapsed >= timeout) return NET_TCP_ERR_WOULD_BLOCK;
        (void)net_poll(0u);
        pit_sleep(1u);
        elapsed++;
    }
}

int net_tcp_socket_peer(int socket_id, uint8_t out_ip[4], uint16_t* out_port) {
    net_tcp_socket_state_t* s;
    if (!net_tcp_socket_id_valid(socket_id)) return NET_TCP_ERR_INVALID;
    s = &g_tcp_sockets[socket_id];
    if (!s->in_use) return NET_TCP_ERR_INVALID;
    if (out_ip) memcpy(out_ip, s->remote_ip, 4);
    if (out_port) *out_port = s->remote_port;
    return NET_TCP_OK;
}

int net_tcp_probe_connect(const uint8_t dst_ip[4], uint16_t dst_port, uint32_t timeout_ms) {
    uint32_t timeout = timeout_ms == 0u ? 2000u : timeout_ms;
    uint32_t start_ms;
    uint32_t last_retx_ms;
    uint32_t syn_retries = 0;
    uint32_t syn_retry_limit = 6u;
    uint32_t retx_interval_ms = 500u;
    uint16_t local_port;
    uint32_t isn;
    int sid;
    net_tcp_socket_state_t* s;

    if (!dst_ip || dst_port == 0u || net_ipv4_is_zero(dst_ip) || net_ipv4_is_broadcast(dst_ip)) return NET_TCP_ERR_INVALID;
    if (!net_is_ready() || net_ipv4_is_zero(g_ipv4_addr)) return NET_TCP_ERR_NOT_READY;

    sid = net_tcp_socket_alloc();
    if (sid < 0) return NET_TCP_ERR_NO_SOCKETS;
    s = &g_tcp_sockets[sid];

    local_port = net_tcp_allocate_ephemeral_port();
    if (local_port == 0u) {
        net_tcp_socket_release(sid);
        return NET_TCP_ERR_NO_SOCKETS;
    }

    memcpy(s->remote_ip, dst_ip, 4);
    s->remote_port = dst_port;
    s->local_port = local_port;
    isn = (uint32_t)pit_get_uptime_ms() ^ ((uint32_t)local_port << 16);
    s->snd_nxt = isn + 1u;
    s->rcv_nxt = 0u;
    s->state = NET_TCP_STATE_SYN_SENT;
    s->connect_result = NET_TCP_ERR_TIMEOUT;

    if (net_tcp_send_segment(dst_ip, local_port, dst_port, isn, 0u, NET_TCP_FLAG_SYN, NULL, 0u) != 0) {
        net_tcp_socket_release(sid);
        return NET_TCP_ERR_ARP;
    }
    net_tcp_rtx_arm(s, isn, 0u, NET_TCP_FLAG_SYN, NULL, 0u);
    g_tcp_dbg.syn_sent++;
    g_tcp_dbg.last_syn_sent_ms = (uint32_t)pit_get_uptime_ms();

    start_ms = g_tcp_dbg.last_syn_sent_ms;
    last_retx_ms = start_ms;

    for (;;) {
        uint32_t now_ms = (uint32_t)pit_get_uptime_ms();
        uint32_t elapsed_ms = now_ms - start_ms;

        if (s->state == NET_TCP_STATE_ESTABLISHED) {
            (void)net_tcp_send_segment(dst_ip,
                                       local_port,
                                       dst_port,
                                       s->snd_nxt,
                                       s->rcv_nxt,
                                       NET_TCP_FLAG_RST | NET_TCP_FLAG_ACK,
                                       NULL,
                                       0u);
            net_tcp_socket_release(sid);
            g_tcp_dbg.connect_ok++;
            return NET_TCP_OK;
        }
        if (s->state == NET_TCP_STATE_RESET) {
            int rc = s->connect_result;
            net_tcp_socket_release(sid);
            return rc;
        }

        if (elapsed_ms >= timeout) break;

        if (s->state == NET_TCP_STATE_SYN_SENT &&
            (now_ms - last_retx_ms) >= retx_interval_ms &&
            syn_retries < syn_retry_limit) {
            last_retx_ms = now_ms;
            syn_retries++;
            (void)net_tcp_send_segment(dst_ip, local_port, dst_port, isn, 0u, NET_TCP_FLAG_SYN, NULL, 0u);
            g_tcp_dbg.syn_retx++;
        }

        (void)net_poll(0u);
        pit_sleep(10u);
    }

    net_tcp_socket_release(sid);
    g_tcp_dbg.connect_timeout++;
    return NET_TCP_ERR_TIMEOUT;
}

int net_tcp_client_connect(const uint8_t dst_ip[4], uint16_t dst_port, uint32_t timeout_ms, int* out_socket_id) {
    uint32_t timeout = timeout_ms == 0u ? 2000u : timeout_ms;
    uint32_t start_ms;
    uint32_t last_retx_ms;
    uint32_t syn_retries = 0;
    uint32_t syn_retry_limit = 6u;
    uint32_t retx_interval_ms = 500u;
    uint16_t local_port;
    uint32_t isn;
    int sid;
    net_tcp_socket_state_t* s;

    if (!dst_ip || !out_socket_id || dst_port == 0u || net_ipv4_is_zero(dst_ip) || net_ipv4_is_broadcast(dst_ip)) {
        return NET_TCP_ERR_INVALID;
    }
    if (!net_ipv4_is_local(dst_ip)) {
        if (!net_is_ready() || net_ipv4_is_zero(g_ipv4_addr)) return NET_TCP_ERR_NOT_READY;
    }

    sid = net_tcp_socket_alloc();
    if (sid < 0) return NET_TCP_ERR_NO_SOCKETS;
    s = &g_tcp_sockets[sid];

    local_port = net_tcp_allocate_ephemeral_port();
    if (local_port == 0u) {
        net_tcp_socket_release(sid);
        return NET_TCP_ERR_NO_SOCKETS;
    }

    memcpy(s->remote_ip, dst_ip, 4);
    s->remote_port = dst_port;
    s->local_port = local_port;
    isn = (uint32_t)pit_get_uptime_ms() ^ ((uint32_t)local_port << 16);
    s->snd_una = isn;
    s->snd_nxt = isn + 1u;
    s->rcv_nxt = 0u;
    s->state = NET_TCP_STATE_SYN_SENT;
    s->connect_result = NET_TCP_ERR_TIMEOUT;

    if (net_tcp_send_segment(dst_ip, local_port, dst_port, isn, 0u, NET_TCP_FLAG_SYN, NULL, 0u) != 0) {
        net_tcp_socket_release(sid);
        return NET_TCP_ERR_ARP;
    }
    net_tcp_rtx_arm(s, isn, 0u, NET_TCP_FLAG_SYN, NULL, 0u);
    g_tcp_dbg.syn_sent++;
    g_tcp_dbg.last_syn_sent_ms = (uint32_t)pit_get_uptime_ms();

    start_ms = g_tcp_dbg.last_syn_sent_ms;
    last_retx_ms = start_ms;

    for (;;) {
        uint32_t now_ms = (uint32_t)pit_get_uptime_ms();
        uint32_t elapsed_ms = now_ms - start_ms;

        if (s->state == NET_TCP_STATE_ESTABLISHED) {
            *out_socket_id = sid;
            g_tcp_dbg.connect_ok++;
            return NET_TCP_OK;
        }
        if (s->state == NET_TCP_STATE_RESET) {
            int rc = s->connect_result;
            net_tcp_socket_release(sid);
            return rc;
        }

        if (elapsed_ms >= timeout) break;

        if (s->state == NET_TCP_STATE_SYN_SENT &&
            (now_ms - last_retx_ms) >= retx_interval_ms &&
            syn_retries < syn_retry_limit) {
            last_retx_ms = now_ms;
            syn_retries++;
            (void)net_tcp_send_segment(dst_ip, local_port, dst_port, isn, 0u, NET_TCP_FLAG_SYN, NULL, 0u);
            g_tcp_dbg.syn_retx++;
        }

        (void)net_poll(0u);
        pit_sleep(10u);
    }

    net_tcp_socket_release(sid);
    g_tcp_dbg.connect_timeout++;
    return NET_TCP_ERR_TIMEOUT;
}

int net_tcp_client_send(int socket_id, const void* payload, uint16_t payload_len, uint32_t timeout_ms) {
    const uint8_t* bytes = (const uint8_t*)payload;
    uint32_t timeout = timeout_ms == 0u ? 2000u : timeout_ms;
    net_tcp_socket_state_t* s;
    uint16_t sent = 0;

    if (!net_tcp_socket_id_valid(socket_id) || (!payload && payload_len > 0u)) return NET_TCP_ERR_INVALID;
    s = &g_tcp_sockets[socket_id];
    if (!s->in_use || s->state == NET_TCP_STATE_RESET) return NET_TCP_ERR_INVALID;
    if (s->state != NET_TCP_STATE_ESTABLISHED) return NET_TCP_ERR_NOT_READY;

    while (sent < payload_len) {
        uint16_t chunk = (uint16_t)(payload_len - sent);
        uint32_t wait_start_ms;
        uint32_t wait_ms;
        uint32_t tx_seq;
        uint32_t cwnd_cap = s->cwnd;
        uint32_t peer_cap = s->snd_wnd;

        if (chunk > NET_TCP_SEG_MAX) chunk = NET_TCP_SEG_MAX;
        if (cwnd_cap > 0u && cwnd_cap < chunk) chunk = (uint16_t)cwnd_cap;
        if (peer_cap > 0u && peer_cap < chunk) chunk = (uint16_t)peer_cap;
        if (chunk == 0u) chunk = 1u;
        tx_seq = s->snd_nxt;
        if (net_tcp_send_segment(s->remote_ip,
                                 s->local_port,
                                 s->remote_port,
                                 tx_seq,
                                 s->rcv_nxt,
                                 NET_TCP_FLAG_ACK,
                                 bytes + sent,
                                 chunk) != 0) {
            return NET_TCP_ERR_TX;
        }

        net_tcp_rtx_arm(s, tx_seq, s->rcv_nxt, NET_TCP_FLAG_ACK, bytes + sent, chunk);
        net_tcp_note_tx(s);

        s->snd_nxt += chunk;
        wait_start_ms = (uint32_t)pit_get_uptime_ms();
        for (;;) {
            if (s->state == NET_TCP_STATE_RESET) return NET_TCP_ERR_RESET;
            if (s->snd_una >= s->snd_nxt) break;
            wait_ms = (uint32_t)pit_get_uptime_ms() - wait_start_ms;
            if (wait_ms >= timeout) return NET_TCP_ERR_TIMEOUT;
            (void)net_poll(0u);
            pit_sleep(1u);
        }

        sent = (uint16_t)(sent + chunk);
    }

    return NET_TCP_OK;
}

int net_tcp_client_recv(int socket_id,
                        void* out_payload,
                        uint16_t payload_capacity,
                        uint16_t* out_payload_len,
                        uint32_t timeout_ms) {
    net_tcp_socket_state_t* s;
    uint32_t timeout = timeout_ms;
    uint32_t elapsed = 0u;

    if (!net_tcp_socket_id_valid(socket_id)) return NET_TCP_ERR_INVALID;
    if (!out_payload && payload_capacity > 0u) return NET_TCP_ERR_INVALID;
    s = &g_tcp_sockets[socket_id];
    if (!s->in_use || !net_tcp_socket_is_active_state(s->state)) return NET_TCP_ERR_INVALID;

    if (out_payload_len) *out_payload_len = 0u;

    for (;;) {
        if (s->rx_len > 0u) {
            uint16_t copy_len = s->rx_len;
            if (copy_len > payload_capacity) copy_len = payload_capacity;
            if (copy_len > 0u && out_payload) {
                memcpy(out_payload, s->rx_buf, copy_len);
            }
            if (copy_len < s->rx_len) {
                memmove(s->rx_buf, s->rx_buf + copy_len, (size_t)(s->rx_len - copy_len));
            }
            s->rx_len = (uint16_t)(s->rx_len - copy_len);
            if (out_payload_len) *out_payload_len = copy_len;
            (void)net_tcp_send_pure_ack(s);
            return NET_TCP_OK;
        }

        if (s->state == NET_TCP_STATE_RESET) return NET_TCP_ERR_RESET;
        if (s->peer_closed || s->state == NET_TCP_STATE_CLOSED || s->state == NET_TCP_STATE_TIME_WAIT) return NET_TCP_ERR_CLOSED;
        if (timeout == 0u) return NET_TCP_ERR_WOULD_BLOCK;
        if (elapsed >= timeout) return NET_TCP_ERR_WOULD_BLOCK;

        (void)net_poll(0u);
        pit_sleep(1u);
        elapsed++;
    }
}

int net_tcp_client_close(int socket_id, uint32_t timeout_ms) {
    uint32_t timeout = timeout_ms == 0u ? 2000u : timeout_ms;
    uint32_t start_ms;
    net_tcp_socket_state_t* s;

    if (!net_tcp_socket_id_valid(socket_id)) return NET_TCP_ERR_INVALID;
    s = &g_tcp_sockets[socket_id];
    if (!s->in_use) return NET_TCP_ERR_INVALID;

    if (s->state == NET_TCP_STATE_CLOSED) {
        net_tcp_socket_release(socket_id);
        return NET_TCP_OK;
    }
    if (s->state == NET_TCP_STATE_RESET) {
        int rc = s->connect_result;
        net_tcp_socket_release(socket_id);
        return rc;
    }

    if (s->state == NET_TCP_STATE_ESTABLISHED) {
        uint32_t fin_seq = s->snd_nxt;
        if (net_tcp_send_segment(s->remote_ip,
                                 s->local_port,
                                 s->remote_port,
                                 fin_seq,
                                 s->rcv_nxt,
                                 NET_TCP_FLAG_FIN | NET_TCP_FLAG_ACK,
                                 NULL,
                                 0u) != 0) {
            return NET_TCP_ERR_TX;
        }
        net_tcp_rtx_arm(s, fin_seq, s->rcv_nxt, NET_TCP_FLAG_FIN | NET_TCP_FLAG_ACK, NULL, 0u);
        s->snd_nxt += 1u;
        s->state = NET_TCP_STATE_FIN_WAIT_1;
    } else if (s->state == NET_TCP_STATE_CLOSE_WAIT) {
        uint32_t fin_seq = s->snd_nxt;
        if (net_tcp_send_segment(s->remote_ip,
                                 s->local_port,
                                 s->remote_port,
                                 fin_seq,
                                 s->rcv_nxt,
                                 NET_TCP_FLAG_FIN | NET_TCP_FLAG_ACK,
                                 NULL,
                                 0u) != 0) {
            return NET_TCP_ERR_TX;
        }
        net_tcp_rtx_arm(s, fin_seq, s->rcv_nxt, NET_TCP_FLAG_FIN | NET_TCP_FLAG_ACK, NULL, 0u);
        s->snd_nxt += 1u;
        s->state = NET_TCP_STATE_LAST_ACK;
    }

    start_ms = (uint32_t)pit_get_uptime_ms();
    while (((uint32_t)pit_get_uptime_ms() - start_ms) < timeout) {
        if (s->state == NET_TCP_STATE_CLOSED) {
            net_tcp_socket_release(socket_id);
            return NET_TCP_OK;
        }
        if (s->state == NET_TCP_STATE_TIME_WAIT) {
            /* Keep draining until TIME_WAIT expiry closes the socket. */
        }
        if (s->state == NET_TCP_STATE_RESET) {
            int rc = s->connect_result;
            net_tcp_socket_release(socket_id);
            return rc;
        }
        (void)net_poll(0u);
        pit_sleep(10u);
    }

    net_tcp_socket_release(socket_id);
    return NET_TCP_ERR_TIMEOUT;
}

int net_udp_socket_readable(int socket_id) {
    if (socket_id < 0 || socket_id >= (int)NET_UDP_MAX_SOCKETS) return 0;
    if (!g_udp_sockets[socket_id].in_use) return 0;
    return g_udp_sockets[socket_id].count > 0u ? 1 : 0;
}

int net_tcp_client_readable(int socket_id) {
    net_tcp_socket_state_t* s;
    if (socket_id < 0 || socket_id >= (int)NET_TCP_MAX_SOCKETS) return 0;
    s = &g_tcp_sockets[socket_id];
    if (!s->in_use) return 0;
    if (s->rx_len > 0u) return 1;
    if (s->peer_closed) return 1;
    if (s->state == NET_TCP_STATE_CLOSED || s->state == NET_TCP_STATE_RESET) return 1;
    return 0;
}

int net_tcp_client_writable(int socket_id) {
    net_tcp_socket_state_t* s;
    if (socket_id < 0 || socket_id >= (int)NET_TCP_MAX_SOCKETS) return 0;
    s = &g_tcp_sockets[socket_id];
    if (!s->in_use) return 0;
    if (s->state != NET_TCP_STATE_ESTABLISHED && s->state != NET_TCP_STATE_CLOSE_WAIT) return 0;
    if (s->rtx_valid) return 0;
    return 1;
}

int net_tcp_listener_readable(int listener_id) {
    if (listener_id < 0 || listener_id >= (int)NET_TCP_MAX_LISTENERS) return 0;
    if (!g_tcp_listeners[listener_id].in_use) return 0;
    return g_tcp_listeners[listener_id].qcount > 0u ? 1 : 0;
}

int net_tcp_set_nodelay(int socket_id, int enabled) {
    net_tcp_socket_state_t* s;
    if (!net_tcp_socket_id_valid(socket_id)) return NET_TCP_ERR_INVALID;
    s = &g_tcp_sockets[socket_id];
    if (!s->in_use) return NET_TCP_ERR_INVALID;
    s->nodelay = enabled ? 1u : 0u;
    return NET_TCP_OK;
}

int net_tcp_set_keepalive(int socket_id, int enabled) {
    net_tcp_socket_state_t* s;
    if (!net_tcp_socket_id_valid(socket_id)) return NET_TCP_ERR_INVALID;
    s = &g_tcp_sockets[socket_id];
    if (!s->in_use) return NET_TCP_ERR_INVALID;
    s->keepalive_enabled = enabled ? 1u : 0u;
    s->keepalive_probes = 0u;
    s->keepalive_probe_ms = 0u;
    s->idle_ms = 0u;
    return NET_TCP_OK;
}

void net_tcp_get_debug_stats(net_tcp_debug_stats_t* out_stats) {
    if (!out_stats) return;
    *out_stats = g_tcp_dbg;
}

void net_get_stats(net_stats_t* out_stats) {
    if (!out_stats) return;

    if (g_driver == NET_DRIVER_E1000) {
        e1000_stats_t st;
        e1000_get_stats(&st);
        out_stats->interrupts = st.interrupts;
        out_stats->rx_packets = st.rx_packets;
        out_stats->tx_packets = st.tx_packets;
        out_stats->rx_irqs = st.rx_irqs;
        out_stats->tx_irqs = st.tx_irqs;
        out_stats->link_events = st.link_events;
        out_stats->rx_drops = st.rx_drops;
        return;
    }

    if (g_driver == NET_DRIVER_RTL8139) {
        rtl8139_stats_t st;
        rtl8139_get_stats(&st);
        out_stats->interrupts = st.interrupts;
        out_stats->rx_packets = st.rx_packets;
        out_stats->tx_packets = st.tx_packets;
        out_stats->rx_irqs = st.rx_irqs;
        out_stats->tx_irqs = st.tx_irqs;
        out_stats->link_events = st.link_events;
        out_stats->rx_drops = st.rx_drops;
        return;
    }

    out_stats->interrupts = 0;
    out_stats->rx_packets = 0;
    out_stats->tx_packets = 0;
    out_stats->rx_irqs = 0;
    out_stats->tx_irqs = 0;
    out_stats->link_events = 0;
    out_stats->rx_drops = 0;
}

void net_get_debug_info(net_debug_info_t* out_info) {
    if (!out_info) return;

    if (g_driver == NET_DRIVER_E1000) {
        e1000_debug_info_t dbg;
        e1000_get_debug_info(&dbg);
        out_info->vendor_id = dbg.vendor_id;
        out_info->device_id = dbg.device_id;
        out_info->io_base = dbg.io_base;
        out_info->irq = dbg.irq;
        out_info->reg_a = dbg.ctrl;
        out_info->reg_b = dbg.status;
        out_info->reg_c = dbg.ral0;
        out_info->reg_d = dbg.rah0;
        return;
    }

    if (g_driver == NET_DRIVER_RTL8139) {
        rtl8139_debug_info_t dbg;
        rtl8139_get_debug_info(&dbg);
        out_info->vendor_id = dbg.vendor_id;
        out_info->device_id = dbg.device_id;
        out_info->io_base = dbg.io_base;
        out_info->irq = dbg.irq;
        out_info->reg_a = dbg.command;
        out_info->reg_b = dbg.media_status;
        out_info->reg_c = 0;
        out_info->reg_d = 0;
        return;
    }

    out_info->vendor_id = 0;
    out_info->device_id = 0;
    out_info->io_base = 0;
    out_info->irq = 0;
    out_info->reg_a = 0;
    out_info->reg_b = 0;
    out_info->reg_c = 0;
    out_info->reg_d = 0;
}

