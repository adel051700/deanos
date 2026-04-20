#ifndef _KERNEL_NET_H
#define _KERNEL_NET_H

#include <stdint.h>

#define NET_PING_OK                 0
#define NET_PING_ERR_INVALID       -1
#define NET_PING_ERR_ARP_UNRESOLVED -2
#define NET_PING_ERR_TX            -3
#define NET_PING_ERR_TIMEOUT       -4
#define NET_PING_ERR_DEST_UNREACH  -5
#define NET_PING_ERR_TIME_EXCEEDED -6
#define NET_PING_ERR_ICMP          -7

#define NET_UDP_OK                   0
#define NET_UDP_ERR_INVALID         -1
#define NET_UDP_ERR_NO_SOCKETS      -2
#define NET_UDP_ERR_NOT_READY       -3
#define NET_UDP_ERR_PORT_IN_USE     -4
#define NET_UDP_ERR_NOT_BOUND       -5
#define NET_UDP_ERR_ARP_UNRESOLVED  -6
#define NET_UDP_ERR_TX              -7
#define NET_UDP_ERR_WOULD_BLOCK     -8
#define NET_UDP_ERR_MSG_TRUNC       -9

#define NET_DNS_OK                   0
#define NET_DNS_ERR_INVALID         -1
#define NET_DNS_ERR_SOCKET          -2
#define NET_DNS_ERR_TX              -3
#define NET_DNS_ERR_TIMEOUT         -4
#define NET_DNS_ERR_FORMAT          -5
#define NET_DNS_ERR_NOT_FOUND       -6

#define NET_TCP_OK                   0
#define NET_TCP_ERR_INVALID         -1
#define NET_TCP_ERR_NOT_READY       -2
#define NET_TCP_ERR_ARP             -3
#define NET_TCP_ERR_TX              -4
#define NET_TCP_ERR_TIMEOUT         -5
#define NET_TCP_ERR_RESET           -6
#define NET_TCP_ERR_NO_SOCKETS      -7
#define NET_TCP_ERR_WOULD_BLOCK     -8
#define NET_TCP_ERR_CLOSED          -9
#define NET_TCP_ERR_ALREADY         -10

typedef struct net_stats {
    uint32_t interrupts;
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_irqs;
    uint32_t tx_irqs;
    uint32_t link_events;
    uint32_t rx_drops;
} net_stats_t;

typedef struct net_debug_info {
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t io_base;
    uint8_t irq;
    uint32_t reg_a;
    uint32_t reg_b;
    uint32_t reg_c;
    uint32_t reg_d;
} net_debug_info_t;

typedef struct net_arp_entry {
    uint8_t ip[4];
    uint8_t mac[6];
    uint32_t age;
    uint8_t valid;
} net_arp_entry_t;

typedef struct net_arp_stats {
    uint32_t rx_arp_packets;
    uint32_t rx_arp_requests;
    uint32_t rx_arp_replies;
    uint32_t tx_arp_requests;
    uint32_t tx_arp_replies;
    uint32_t cache_hits;
    uint32_t cache_misses;
    uint32_t dropped_frames;
} net_arp_stats_t;

typedef struct net_udp_endpoint {
    uint8_t ip[4];
    uint16_t port;
} net_udp_endpoint_t;

typedef struct net_tcp_debug_stats {
    uint32_t syn_sent;
    uint32_t syn_retx;
    uint32_t synack_seen;
    uint32_t rst_seen;
    uint32_t checksum_drop;
    uint32_t tuple_miss;
    uint32_t connect_ok;
    uint32_t connect_timeout;

    /* Last packet that triggered tuple_miss, for diagnostics. */
    uint8_t  last_miss_src_ip[4];
    uint16_t last_miss_src_port;
    uint16_t last_miss_dst_port;
    uint8_t  last_miss_flags;
    uint32_t last_miss_ack;
    uint32_t last_miss_seq;
    uint32_t last_miss_arrival_ms;
    uint32_t last_syn_sent_ms;
} net_tcp_debug_stats_t;

typedef struct netif netif_t;

#define NET_RX_PBUF_POOL_SIZE         64u
#define NET_RX_PBUF_DATA_SIZE       1600u
#define NET_RX_DEFER_QUEUE_LEN        64u
#define NET_RX_POLL_BUDGET_DEFAULT    16u

typedef int (*netif_linkoutput_fn)(const void* data, uint16_t len);
typedef int (*netif_state_fn)(void);
typedef int (*netif_test_tx_fn)(void);
typedef void (*netif_get_mac_fn)(uint8_t out_mac[6]);

typedef struct netif_driver_ops {
    netif_linkoutput_fn linkoutput;
    netif_state_fn is_ready;
    netif_state_fn link_up;
    netif_test_tx_fn send_test_frame;
    netif_get_mac_fn get_mac;
} netif_driver_ops_t;

struct netif {
    char name[8];
    uint16_t mtu;
    uint8_t flags;
    uint8_t hwaddr[6];
    uint8_t ipv4_addr[4];
    uint8_t ipv4_netmask[4];
    uint8_t ipv4_gateway[4];
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t rx_drops;
    uint32_t link_changes;
    const netif_driver_ops_t* driver_ops;
};

typedef struct net_rx_defer_stats {
    uint32_t enqueued;
    uint32_t dequeued;
    uint32_t drop_pool_empty;
    uint32_t drop_queue_full;
    uint32_t drop_too_large;
    uint32_t drop_invalid;
} net_rx_defer_stats_t;

typedef struct net_timer_debug {
    uint32_t timer_tick_ms_accum;
    uint32_t link_refresh_period_ms;
    uint32_t link_refresh_count;
    uint32_t link_state_changes;
    uint32_t dhcp_lease_remaining_ms;
    uint32_t dhcp_t1_remaining_ms;
    uint32_t dhcp_t2_remaining_ms;
    uint32_t dhcp_retry_remaining_ms;
    uint32_t dhcp_retry_count;
    uint32_t tcp_rtx_scans;
    uint32_t tcp_rtx_due;
    uint32_t tcp_rtx_sent;
    uint32_t tcp_rtx_timeout;
    uint32_t tcp_rtx_active;
} net_timer_debug_t;

typedef struct net_p2_stats {
    uint32_t ipv4_malformed;
    uint32_t ipv4_frag_rx;
    uint32_t ipv4_frag_reasm_ok;
    uint32_t ipv4_frag_reasm_drop;
    uint32_t icmp_rx_unreach;
    uint32_t icmp_rx_timeex;
    uint32_t icmp_rx_param;
    uint32_t dns_cache_hit;
    uint32_t dns_cache_miss;
    uint32_t dns_cache_neg_hit;
    uint32_t dns_cache_insert;
    uint32_t dns_cache_evict;
    uint32_t dns_query_retry;
    uint32_t dns_query_backoff;
    uint32_t dns_query_timeout;
} net_p2_stats_t;

typedef struct net_dns_cache_debug_entry {
    char name[64];
    uint8_t ip[4];
    uint8_t negative;
    uint32_t ttl_left_ms;
    uint32_t age_ms;
    uint32_t lru_rank;
} net_dns_cache_debug_entry_t;

int net_core_init(void);
void net_core_input(const uint8_t* frame, uint16_t len);
uint32_t net_poll(uint32_t budget);
void net_worker_step(void);
void net_timers_tick(uint32_t elapsed_ms);
void net_get_timer_debug(net_timer_debug_t* out_debug);
void net_get_p2_stats(net_p2_stats_t* out_stats);
void net_dhcp_note_lease(uint32_t lease_seconds, uint32_t t1_seconds, uint32_t t2_seconds);
void net_dhcp_client_seed(const uint8_t server_ip[4],
                          const uint8_t lease_ip[4],
                          const uint8_t netmask[4],
                          const uint8_t gateway[4],
                          uint32_t lease_seconds,
                          uint32_t t1_seconds,
                          uint32_t t2_seconds);
void net_get_rx_defer_stats(net_rx_defer_stats_t* out_stats);
const netif_t* net_default_netif(void);

int net_initialize(void);
int net_is_ready(void);
int net_link_up(void);
const char* net_driver_name(void);
int net_send_test_frame(void);
void net_get_mac(uint8_t out_mac[6]);
void net_set_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
void net_get_ipv4(uint8_t out_ip[4]);
void net_set_ipv4_netmask(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
void net_get_ipv4_netmask(uint8_t out_mask[4]);
void net_set_ipv4_gateway(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
void net_get_ipv4_gateway(uint8_t out_gw[4]);
int net_send_arp_request(const uint8_t target_ip[4]);
int net_arp_lookup(const uint8_t ip[4], uint8_t out_mac[6]);
int net_arp_resolve_retry(const uint8_t ip[4], uint8_t out_mac[6], uint32_t retries, uint32_t wait_ms);
uint32_t net_get_arp_cache(net_arp_entry_t* out_entries, uint32_t max_entries);
void net_get_arp_stats(net_arp_stats_t* out_stats);
int net_ping_ipv4(const uint8_t target_ip[4], uint16_t seq, uint32_t timeout_ms);
int net_udp_socket_open(void);
int net_udp_socket_close(int socket_id);
int net_udp_socket_bind(int socket_id, uint16_t local_port);
int net_udp_socket_sendto(int socket_id,
                          const uint8_t dst_ip[4],
                          uint16_t dst_port,
                          const void* payload,
                          uint16_t payload_len);
int net_udp_socket_recvfrom(int socket_id,
                            void* out_payload,
                            uint16_t payload_capacity,
                            uint16_t* out_payload_len,
                            net_udp_endpoint_t* out_from,
                            uint32_t timeout_ms);
int net_dns_query_a(const char* hostname,
                    const uint8_t dns_server_ip[4],
                    uint8_t out_ip[4],
                    uint32_t timeout_ms);
uint32_t net_dns_cache_count(void);
uint32_t net_dns_cache_dump(net_dns_cache_debug_entry_t* out_entries, uint32_t max_entries);
int net_tcp_probe_connect(const uint8_t dst_ip[4], uint16_t dst_port, uint32_t timeout_ms);
int net_tcp_listener_open(void);
int net_tcp_listener_close(int listener_id);
int net_tcp_listener_bind(int listener_id, uint16_t local_port);
int net_tcp_listener_listen(int listener_id, uint16_t backlog);
int net_tcp_listener_accept(int listener_id, uint32_t timeout_ms, int* out_socket_id);
int net_tcp_socket_peer(int socket_id, uint8_t out_ip[4], uint16_t* out_port);
int net_tcp_client_connect(const uint8_t dst_ip[4], uint16_t dst_port, uint32_t timeout_ms, int* out_socket_id);
int net_tcp_client_send(int socket_id, const void* payload, uint16_t payload_len, uint32_t timeout_ms);
int net_tcp_client_recv(int socket_id,
                        void* out_payload,
                        uint16_t payload_capacity,
                        uint16_t* out_payload_len,
                        uint32_t timeout_ms);
int net_tcp_client_close(int socket_id, uint32_t timeout_ms);
int net_udp_socket_readable(int socket_id);
int net_tcp_client_readable(int socket_id);
int net_tcp_client_writable(int socket_id);
int net_tcp_listener_readable(int listener_id);
int net_tcp_set_nodelay(int socket_id, int enabled);
int net_tcp_set_keepalive(int socket_id, int enabled);
void net_tcp_get_debug_stats(net_tcp_debug_stats_t* out_stats);
void net_get_stats(net_stats_t* out_stats);
void net_get_debug_info(net_debug_info_t* out_info);

#endif

