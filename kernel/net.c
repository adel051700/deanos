#include "include/kernel/net.h"

#include "include/kernel/e1000.h"
#include "include/kernel/log.h"
#include "include/kernel/pci.h"
#include "include/kernel/pit.h"
#include "include/kernel/rtl8139.h"

#include <string.h>

typedef enum net_driver_kind {
    NET_DRIVER_NONE = 0,
    NET_DRIVER_E1000,
    NET_DRIVER_RTL8139,
} net_driver_kind_t;

static net_driver_kind_t g_driver = NET_DRIVER_NONE;

#define NET_ETH_TYPE_ARP  0x0806u
#define NET_ETH_TYPE_IPV4 0x0800u
#define NET_IPV4_PROTO_ICMP 1u
#define NET_IPV4_PROTO_UDP 17u

#define NET_ARP_TABLE_SIZE 8u
#define NET_ARP_TTL_MS 300000u
#define NET_PING_ID 0xD30Au
#define NET_UDP_MAX_SOCKETS 8u
#define NET_UDP_RX_QUEUE_LEN 8u
#define NET_UDP_MAX_PAYLOAD 1472u
#define NET_UDP_EPHEMERAL_START 49152u
#define NET_UDP_EPHEMERAL_END   65535u
#define NET_DNS_MAX_NAME_LEN 253u
#define NET_DNS_PORT 53u

typedef struct __attribute__((packed)) net_eth_hdr {
    uint8_t dst[6];
    uint8_t src[6];
    uint8_t type_be[2];
} net_eth_hdr_t;

typedef struct net_arp_entry {
    uint8_t valid;
    uint8_t ip[4];
    uint8_t mac[6];
    uint64_t expires_at_ms;
} net_arp_entry_t;

typedef struct net_proto_stats {
    uint32_t arp_rx;
    uint32_t arp_tx;
    uint32_t ipv4_rx;
    uint32_t ipv4_tx;
    uint32_t icmp_rx;
    uint32_t icmp_tx;
    uint32_t udp_rx;
    uint32_t udp_tx;
    uint32_t ping_requests;
    uint32_t ping_replies;
    uint32_t dns_queries;
    uint32_t dns_replies;
    uint32_t dns_timeouts;
    uint32_t dns_failures;
    uint32_t dns_seen;
    uint32_t dns_short;
    uint32_t dns_bad_header;
    uint32_t dns_no_a_answer;
    uint32_t udp_bad_checksum;
    uint32_t udp_no_socket;
    uint32_t udp_queue_drop;
} net_proto_stats_t;

typedef struct net_udp_rx_packet {
    uint8_t used;
    uint8_t src_ip[4];
    uint16_t src_port;
    uint16_t len;
    uint8_t data[NET_UDP_MAX_PAYLOAD];
} net_udp_rx_packet_t;

typedef struct net_udp_socket {
    uint8_t in_use;
    uint16_t local_port;
    uint8_t rx_head;
    uint8_t rx_tail;
    uint8_t rx_count;
    net_udp_rx_packet_t rxq[NET_UDP_RX_QUEUE_LEN];
} net_udp_socket_t;

typedef struct net_ping_wait {
    volatile uint8_t active;
    volatile uint8_t done;
    volatile uint8_t ttl;
    volatile uint16_t seq;
    uint8_t target_ip[4];
    volatile uint64_t started_ms;
    volatile uint64_t finished_ms;
} net_ping_wait_t;

static net_ipv4_config_t g_ipv4 = {
    .address = {10, 0, 2, 15},
    .netmask = {255, 255, 255, 0},
    .gateway = {10, 0, 2, 2},
    .dns_server = {10, 0, 2, 3},
    .configured = 1,
};

static net_arp_entry_t g_arp_table[NET_ARP_TABLE_SIZE];
static uint32_t g_arp_next_slot = 0;
static net_proto_stats_t g_proto_stats;
static volatile uint16_t g_ping_sequence = 1;
static net_ping_wait_t g_ping_wait;
static net_udp_socket_t g_udp_sockets[NET_UDP_MAX_SOCKETS];
static uint16_t g_udp_next_ephemeral_port = NET_UDP_EPHEMERAL_START;
static uint16_t g_udp_last_nosock_dst_port = 0;
static uint16_t g_dns_last_local_port = 0;

static int net_send_arp_packet(uint16_t op, const uint8_t target_mac[6], const uint8_t target_ip[4]);

static uint16_t net_read_be16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void net_write_be16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}


static int net_ip_is_zero(const uint8_t ip[4]) {
    return (ip[0] | ip[1] | ip[2] | ip[3]) == 0;
}

static int net_ip_equal(const uint8_t a[4], const uint8_t b[4]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static int net_ip_same_subnet(const uint8_t a[4], const uint8_t b[4], const uint8_t mask[4]) {
    return ((a[0] & mask[0]) == (b[0] & mask[0])) &&
           ((a[1] & mask[1]) == (b[1] & mask[1])) &&
           ((a[2] & mask[2]) == (b[2] & mask[2])) &&
           ((a[3] & mask[3]) == (b[3] & mask[3]));
}


static uint16_t net_checksum16(const void* data, uint32_t len) {
    const uint8_t* b = (const uint8_t*)data;
    uint32_t sum = 0;
    while (len > 1u) {
        sum += ((uint32_t)b[0] << 8) | (uint32_t)b[1];
        b += 2;
        len -= 2;
    }
    if (len) {
        sum += ((uint32_t)b[0] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

static uint16_t net_udp_checksum(const uint8_t src_ip[4], const uint8_t dst_ip[4], const uint8_t* udp, uint16_t udp_len) {
    uint32_t sum = 0;

    sum += ((uint32_t)src_ip[0] << 8) | src_ip[1];
    sum += ((uint32_t)src_ip[2] << 8) | src_ip[3];
    sum += ((uint32_t)dst_ip[0] << 8) | dst_ip[1];
    sum += ((uint32_t)dst_ip[2] << 8) | dst_ip[3];
    sum += NET_IPV4_PROTO_UDP;
    sum += udp_len;

    for (uint16_t i = 0; i + 1u < udp_len; i += 2u) {
        sum += ((uint32_t)udp[i] << 8) | udp[i + 1u];
    }
    if (udp_len & 1u) {
        sum += ((uint32_t)udp[udp_len - 1u] << 8);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

static int net_send_raw_driver(const void* frame, uint16_t len) {
    if (g_driver == NET_DRIVER_E1000) return e1000_send_raw(frame, len);
    if (g_driver == NET_DRIVER_RTL8139) return rtl8139_send_raw(frame, len);
    return -1;
}

static int net_send_eth(const uint8_t dst_mac[6], uint16_t ether_type, const void* payload, uint16_t payload_len) {
    if (!dst_mac || (!payload && payload_len != 0u) || !net_is_ready()) return -1;
    if (payload_len > 1500u) return -1;

    uint8_t frame[1514];
    uint8_t src_mac[6];
    uint16_t frame_len = (uint16_t)(sizeof(net_eth_hdr_t) + payload_len);

    net_get_mac(src_mac);
    memcpy(frame + 0, dst_mac, 6);
    memcpy(frame + 6, src_mac, 6);
    net_write_be16(frame + 12, ether_type);
    if (payload_len) {
        memcpy(frame + sizeof(net_eth_hdr_t), payload, payload_len);
    }

    if (frame_len < 60u) {
        memset(frame + frame_len, 0, (size_t)(60u - frame_len));
        frame_len = 60u;
    }

    return net_send_raw_driver(frame, frame_len);
}

static void net_arp_cache_insert(const uint8_t ip[4], const uint8_t mac[6]) {
    if (!ip || !mac) return;

    uint64_t now = pit_get_uptime_ms();
    for (uint32_t i = 0; i < NET_ARP_TABLE_SIZE; ++i) {
        if (g_arp_table[i].valid && net_ip_equal(g_arp_table[i].ip, ip)) {
            memcpy(g_arp_table[i].mac, mac, 6);
            g_arp_table[i].expires_at_ms = now + NET_ARP_TTL_MS;
            return;
        }
    }

    uint32_t slot = NET_ARP_TABLE_SIZE;
    for (uint32_t i = 0; i < NET_ARP_TABLE_SIZE; ++i) {
        if (!g_arp_table[i].valid || g_arp_table[i].expires_at_ms <= now) {
            slot = i;
            break;
        }
    }
    if (slot == NET_ARP_TABLE_SIZE) {
        slot = g_arp_next_slot % NET_ARP_TABLE_SIZE;
        g_arp_next_slot = (g_arp_next_slot + 1u) % NET_ARP_TABLE_SIZE;
    }

    g_arp_table[slot].valid = 1;
    memcpy(g_arp_table[slot].ip, ip, 4);
    memcpy(g_arp_table[slot].mac, mac, 6);
    g_arp_table[slot].expires_at_ms = now + NET_ARP_TTL_MS;
}

static int net_arp_cache_lookup(const uint8_t ip[4], uint8_t out_mac[6]) {
    if (!ip || !out_mac) return -1;
    uint64_t now = pit_get_uptime_ms();

    for (uint32_t i = 0; i < NET_ARP_TABLE_SIZE; ++i) {
        if (!g_arp_table[i].valid) continue;
        if (g_arp_table[i].expires_at_ms <= now) {
            g_arp_table[i].valid = 0;
            continue;
        }
        if (net_ip_equal(g_arp_table[i].ip, ip)) {
            memcpy(out_mac, g_arp_table[i].mac, 6);
            return 0;
        }
    }
    return -1;
}

static int net_resolve_next_hop(const uint8_t target_ip[4], uint8_t out_next_hop_mac[6], uint32_t timeout_ms) {
    if (!target_ip || !out_next_hop_mac) return -1;
    if (!net_is_ready() || !g_ipv4.configured) return -1;
    if (timeout_ms == 0u) timeout_ms = 1000u;

    uint8_t next_hop_ip[4];

    if (net_ip_same_subnet(target_ip, g_ipv4.address, g_ipv4.netmask) || net_ip_is_zero(g_ipv4.gateway)) {
        memcpy(next_hop_ip, target_ip, 4);
    } else {
        memcpy(next_hop_ip, g_ipv4.gateway, 4);
    }

    uint64_t deadline = pit_get_uptime_ms() + timeout_ms;
    while (net_arp_cache_lookup(next_hop_ip, out_next_hop_mac) != 0) {
        (void)net_send_arp_packet(1, 0, next_hop_ip);
        if (pit_get_uptime_ms() >= deadline) {
            return -2;
        }
        pit_sleep(10);
    }
    return 0;
}

static int net_udp_find_socket_by_port(uint16_t port) {
    if (port == 0u) return -1;
    for (uint32_t i = 0; i < NET_UDP_MAX_SOCKETS; ++i) {
        if (g_udp_sockets[i].in_use && g_udp_sockets[i].local_port == port) {
            return (int)i;
        }
    }
    return -1;
}

static uint16_t net_udp_alloc_ephemeral_port(void) {
    for (uint32_t attempt = 0; attempt <= (NET_UDP_EPHEMERAL_END - NET_UDP_EPHEMERAL_START); ++attempt) {
        uint16_t candidate = g_udp_next_ephemeral_port;
        g_udp_next_ephemeral_port++;
        if (g_udp_next_ephemeral_port < NET_UDP_EPHEMERAL_START) {
            g_udp_next_ephemeral_port = NET_UDP_EPHEMERAL_START;
        }
        if (net_udp_find_socket_by_port(candidate) < 0) {
            return candidate;
        }
    }
    return 0;
}

static int net_udp_queue_push(net_udp_socket_t* sock, const uint8_t src_ip[4], uint16_t src_port, const uint8_t* payload, uint16_t payload_len) {
    if (!sock || !src_ip || (!payload && payload_len != 0u)) return -1;
    if (payload_len > NET_UDP_MAX_PAYLOAD) return -1;
    if (sock->rx_count >= NET_UDP_RX_QUEUE_LEN) return -1;

    net_udp_rx_packet_t* pkt = &sock->rxq[sock->rx_tail];
    pkt->used = 1;
    memcpy(pkt->src_ip, src_ip, 4);
    pkt->src_port = src_port;
    pkt->len = payload_len;
    if (payload_len) {
        memcpy(pkt->data, payload, payload_len);
    }

    sock->rx_tail = (uint8_t)((sock->rx_tail + 1u) % NET_UDP_RX_QUEUE_LEN);
    sock->rx_count++;
    return 0;
}

static int net_udp_queue_pop(net_udp_socket_t* sock, void* out_payload, uint16_t payload_capacity,
                             uint8_t out_src_ip[4], uint16_t* out_src_port, uint16_t* out_payload_len) {
    if (!sock || sock->rx_count == 0u) return -1;

    net_udp_rx_packet_t* pkt = &sock->rxq[sock->rx_head];
    if (!pkt->used) return -1;

    uint16_t copy_len = pkt->len;
    if (copy_len > payload_capacity) copy_len = payload_capacity;

    if (out_payload && copy_len) {
        memcpy(out_payload, pkt->data, copy_len);
    }
    if (out_src_ip) memcpy(out_src_ip, pkt->src_ip, 4);
    if (out_src_port) *out_src_port = pkt->src_port;
    if (out_payload_len) *out_payload_len = pkt->len;

    pkt->used = 0;
    sock->rx_head = (uint8_t)((sock->rx_head + 1u) % NET_UDP_RX_QUEUE_LEN);
    sock->rx_count--;
    return (int)copy_len;
}

static int net_send_arp_packet(uint16_t op, const uint8_t target_mac[6], const uint8_t target_ip[4]) {
    uint8_t payload[28];
    uint8_t src_mac[6];
    uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const uint8_t* dst = target_mac ? target_mac : bcast;

    net_get_mac(src_mac);

    net_write_be16(payload + 0, 1);      /* Ethernet */
    net_write_be16(payload + 2, 0x0800); /* IPv4 */
    payload[4] = 6;
    payload[5] = 4;
    net_write_be16(payload + 6, op);
    memcpy(payload + 8, src_mac, 6);
    memcpy(payload + 14, g_ipv4.address, 4);

    if (target_mac) memcpy(payload + 18, target_mac, 6);
    else memset(payload + 18, 0, 6);
    memcpy(payload + 24, target_ip, 4);

    if (net_send_eth(dst, NET_ETH_TYPE_ARP, payload, sizeof(payload)) == 0) {
        g_proto_stats.arp_tx++;
        return 0;
    }
    return -1;
}

static int net_send_ipv4_packet(const uint8_t dst_mac[6], const uint8_t dst_ip[4], uint8_t proto,
                                const void* payload, uint16_t payload_len, uint8_t ttl) {
    if (!dst_mac || !dst_ip || (!payload && payload_len != 0u)) return -1;
    if (!g_ipv4.configured) return -1;
    if (payload_len > (uint16_t)(1500u - 20u)) return -1;

    uint8_t packet[1500];
    uint16_t total_len = (uint16_t)(20u + payload_len);
    static uint16_t ident = 1;

    packet[0] = 0x45;
    packet[1] = 0x00;
    net_write_be16(packet + 2, total_len);
    net_write_be16(packet + 4, ident++);
    net_write_be16(packet + 6, 0x0000);
    packet[8] = ttl;
    packet[9] = proto;
    packet[10] = 0;
    packet[11] = 0;
    memcpy(packet + 12, g_ipv4.address, 4);
    memcpy(packet + 16, dst_ip, 4);
    net_write_be16(packet + 10, net_checksum16(packet, 20));

    if (payload_len) memcpy(packet + 20, payload, payload_len);

    if (net_send_eth(dst_mac, NET_ETH_TYPE_IPV4, packet, total_len) == 0) {
        g_proto_stats.ipv4_tx++;
        return 0;
    }
    return -1;
}

static void net_handle_icmp(const uint8_t src_ip[4], const uint8_t* packet, uint16_t packet_len, uint8_t ttl) {
    if (packet_len < 8u || !packet || !src_ip) return;
    if (net_checksum16(packet, packet_len) != 0) return;

    uint8_t type = packet[0];
    uint8_t code = packet[1];

    g_proto_stats.icmp_rx++;

    if (type == 8u && code == 0u) {
        uint8_t out[1500];
        uint8_t next_hop_ip[4];
        uint8_t next_hop_mac[6];

        memcpy(out, packet, packet_len);
        out[0] = 0;
        out[2] = 0;
        out[3] = 0;
        net_write_be16(out + 2, net_checksum16(out, packet_len));

        memcpy(next_hop_ip, src_ip, 4);
        if (net_arp_cache_lookup(next_hop_ip, next_hop_mac) != 0) {
            if (net_send_arp_packet(1, 0, next_hop_ip) != 0) return;
            return;
        }

        if (net_send_ipv4_packet(next_hop_mac, src_ip, NET_IPV4_PROTO_ICMP, out, packet_len, 64u) == 0) {
            g_proto_stats.icmp_tx++;
        }
        return;
    }

    if (type == 0u && code == 0u && packet_len >= 8u && g_ping_wait.active) {
        uint16_t ident = net_read_be16(packet + 4);
        uint16_t seq = net_read_be16(packet + 6);
        if (ident == NET_PING_ID && seq == g_ping_wait.seq && net_ip_equal(src_ip, g_ping_wait.target_ip)) {
            g_ping_wait.ttl = ttl;
            g_ping_wait.done = 1;
            g_ping_wait.active = 0;
            g_ping_wait.finished_ms = pit_get_uptime_ms();
            g_proto_stats.ping_replies++;
        }
    }
}

static void net_handle_udp(const uint8_t src_ip[4], const uint8_t dst_ip[4], const uint8_t* packet, uint16_t packet_len) {
    if (!src_ip || !dst_ip || !packet || packet_len < 8u) return;

    uint16_t src_port = net_read_be16(packet + 0);
    uint16_t dst_port = net_read_be16(packet + 2);
    uint16_t udp_len = net_read_be16(packet + 4);
    uint16_t udp_cksum = net_read_be16(packet + 6);

    if (udp_len < 8u || udp_len > packet_len) return;
    if (udp_cksum != 0u) {
        if (net_udp_checksum(src_ip, dst_ip, packet, udp_len) != 0u) {
            g_proto_stats.udp_bad_checksum++;
            return;
        }
    }

    g_proto_stats.udp_rx++;

    int sock_id = net_udp_find_socket_by_port(dst_port);
    if (sock_id < 0) {
        g_udp_last_nosock_dst_port = dst_port;
        g_proto_stats.udp_no_socket++;
        return;
    }

    net_udp_socket_t* sock = &g_udp_sockets[(uint32_t)sock_id];
    const uint8_t* payload = packet + 8;
    uint16_t payload_len = (uint16_t)(udp_len - 8u);
    if (net_udp_queue_push(sock, src_ip, src_port, payload, payload_len) != 0) {
        g_proto_stats.udp_queue_drop++;
    }
}

static void net_handle_ipv4(const uint8_t* packet, uint16_t len) {
    if (!packet || len < 20u) return;
    if (((packet[0] >> 4) & 0x0Fu) != 4u) return;

    uint8_t ihl = (uint8_t)((packet[0] & 0x0Fu) * 4u);
    if (ihl < 20u || len < ihl) return;

    uint16_t total_len = net_read_be16(packet + 2);
    if (total_len < ihl || total_len > len) return;
    if (net_checksum16(packet, ihl) != 0) return;

    const uint8_t* src_ip = packet + 12;
    const uint8_t* dst_ip = packet + 16;
    uint8_t broadcast_ip[4] = {255, 255, 255, 255};
    if (!net_ip_equal(dst_ip, g_ipv4.address) && !net_ip_equal(dst_ip, broadcast_ip)) return;

    g_proto_stats.ipv4_rx++;

    uint8_t proto = packet[9];
    uint8_t ttl = packet[8];
    const uint8_t* l4 = packet + ihl;
    uint16_t l4_len = (uint16_t)(total_len - ihl);

    if (proto == NET_IPV4_PROTO_ICMP) {
        net_handle_icmp(src_ip, l4, l4_len, ttl);
    } else if (proto == NET_IPV4_PROTO_UDP) {
        net_handle_udp(src_ip, dst_ip, l4, l4_len);
    }
}

static void net_handle_arp(const uint8_t* packet, uint16_t len, const uint8_t src_mac[6]) {
    if (!packet || len < 28u || !src_mac) return;

    uint16_t htype = net_read_be16(packet + 0);
    uint16_t ptype = net_read_be16(packet + 2);
    uint8_t hlen = packet[4];
    uint8_t plen = packet[5];
    uint16_t op = net_read_be16(packet + 6);

    if (htype != 1u || ptype != 0x0800u || hlen != 6u || plen != 4u) return;

    const uint8_t* sender_mac = packet + 8;
    const uint8_t* sender_ip = packet + 14;
    const uint8_t* target_ip = packet + 24;

    g_proto_stats.arp_rx++;
    net_arp_cache_insert(sender_ip, sender_mac);

    if (op == 1u && g_ipv4.configured && net_ip_equal(target_ip, g_ipv4.address)) {
        (void)src_mac;
        (void)net_send_arp_packet(2, sender_mac, sender_ip);
    }
}

static void net_rx_callback(const void* frame, uint16_t len) {
    net_on_receive(frame, len);
}

int net_initialize(void) {
    pci_initialize();

    if (e1000_initialize() == 0) {
        g_driver = NET_DRIVER_E1000;
        e1000_set_rx_callback(net_rx_callback);
        return 0;
    }

    if (rtl8139_initialize() == 0) {
        g_driver = NET_DRIVER_RTL8139;
        rtl8139_set_rx_callback(net_rx_callback);
        return 0;
    }

    g_driver = NET_DRIVER_NONE;
    klog("net: no supported NIC initialized");
    return -1;
}

int net_is_ready(void) {
    if (g_driver == NET_DRIVER_E1000) return e1000_is_ready();
    if (g_driver == NET_DRIVER_RTL8139) return rtl8139_is_ready();
    return 0;
}

int net_link_up(void) {
    if (g_driver == NET_DRIVER_E1000) return e1000_link_up();
    if (g_driver == NET_DRIVER_RTL8139) return rtl8139_link_up();
    return 0;
}

const char* net_driver_name(void) {
    if (g_driver == NET_DRIVER_E1000) return "e1000";
    if (g_driver == NET_DRIVER_RTL8139) return "rtl8139";
    return "none";
}

int net_send_test_frame(void) {
    if (g_driver == NET_DRIVER_E1000) return e1000_send_test_frame();
    if (g_driver == NET_DRIVER_RTL8139) return rtl8139_send_test_frame();
    return -1;
}

int net_set_ipv4_config(const net_ipv4_config_t* config) {
    if (!config) return -1;
    g_ipv4 = *config;
    return 0;
}

int net_get_ipv4_config(net_ipv4_config_t* out_config) {
    if (!out_config) return -1;
    *out_config = g_ipv4;
    return 0;
}

void net_on_receive(const void* frame, uint16_t len) {
    if (!frame || len < sizeof(net_eth_hdr_t)) return;
    const uint8_t* b = (const uint8_t*)frame;

    uint16_t ether_type = net_read_be16(b + 12);
    const uint8_t* payload = b + sizeof(net_eth_hdr_t);
    uint16_t payload_len = (uint16_t)(len - sizeof(net_eth_hdr_t));

    if (ether_type == NET_ETH_TYPE_ARP) {
        net_handle_arp(payload, payload_len, b + 6);
    } else if (ether_type == NET_ETH_TYPE_IPV4) {
        net_handle_ipv4(payload, payload_len);
    }
}

int net_ping(const uint8_t target_ip[4], uint32_t timeout_ms, net_ping_result_t* out_result) {
    if (!target_ip) return -1;
    if (!net_is_ready() || !g_ipv4.configured) return -1;
    if (timeout_ms == 0) timeout_ms = 1000;

    uint8_t next_hop_mac[6];
    uint64_t deadline = pit_get_uptime_ms() + timeout_ms;
    if (net_resolve_next_hop(target_ip, next_hop_mac, timeout_ms) != 0) {
        return -2;
    }

    uint8_t icmp[40];
    uint16_t seq = g_ping_sequence++;
    icmp[0] = 8;
    icmp[1] = 0;
    icmp[2] = 0;
    icmp[3] = 0;
    net_write_be16(icmp + 4, NET_PING_ID);
    net_write_be16(icmp + 6, seq);
    for (uint32_t i = 8; i < sizeof(icmp); ++i) {
        icmp[i] = (uint8_t)(i + seq);
    }
    net_write_be16(icmp + 2, net_checksum16(icmp, sizeof(icmp)));

    g_ping_wait.active = 1;
    g_ping_wait.done = 0;
    g_ping_wait.ttl = 0;
    g_ping_wait.seq = seq;
    memcpy(g_ping_wait.target_ip, target_ip, 4);
    g_ping_wait.started_ms = pit_get_uptime_ms();
    g_ping_wait.finished_ms = g_ping_wait.started_ms;
    g_proto_stats.ping_requests++;

    if (net_send_ipv4_packet(next_hop_mac, target_ip, NET_IPV4_PROTO_ICMP, icmp, sizeof(icmp), 64u) != 0) {
        g_ping_wait.active = 0;
        return -3;
    }
    g_proto_stats.icmp_tx++;

    while (!g_ping_wait.done) {
        if (pit_get_uptime_ms() >= deadline) {
            g_ping_wait.active = 0;
            return -4;
        }
        pit_sleep(10);
    }

    if (out_result) {
        out_result->received = 1;
        out_result->sequence = seq;
        out_result->ttl = g_ping_wait.ttl;
        out_result->elapsed_ms = (uint32_t)(g_ping_wait.finished_ms - g_ping_wait.started_ms);
    }

    return 0;
}

int net_udp_open(void) {
    for (uint32_t i = 0; i < NET_UDP_MAX_SOCKETS; ++i) {
        if (!g_udp_sockets[i].in_use) {
            memset(&g_udp_sockets[i], 0, sizeof(g_udp_sockets[i]));
            g_udp_sockets[i].in_use = 1;
            return (int)i;
        }
    }
    return -1;
}

int net_udp_close(int socket_id) {
    if (socket_id < 0 || socket_id >= (int)NET_UDP_MAX_SOCKETS) return -1;
    if (!g_udp_sockets[(uint32_t)socket_id].in_use) return -1;
    memset(&g_udp_sockets[(uint32_t)socket_id], 0, sizeof(g_udp_sockets[(uint32_t)socket_id]));
    return 0;
}

int net_udp_bind(int socket_id, uint16_t local_port) {
    if (socket_id < 0 || socket_id >= (int)NET_UDP_MAX_SOCKETS) return -1;
    net_udp_socket_t* sock = &g_udp_sockets[(uint32_t)socket_id];
    if (!sock->in_use) return -1;

    if (local_port == 0u) {
        local_port = net_udp_alloc_ephemeral_port();
        if (local_port == 0u) return -1;
    }

    int owner = net_udp_find_socket_by_port(local_port);
    if (owner >= 0 && owner != socket_id) return -1;

    sock->local_port = local_port;
    return 0;
}

int net_udp_sendto(int socket_id, const uint8_t dst_ip[4], uint16_t dst_port, const void* payload, uint16_t payload_len) {
    if (socket_id < 0 || socket_id >= (int)NET_UDP_MAX_SOCKETS) return -1;
    if (!dst_ip || (!payload && payload_len != 0u) || dst_port == 0u) return -1;
    if (payload_len > (uint16_t)(1500u - 20u - 8u)) return -1;

    net_udp_socket_t* sock = &g_udp_sockets[(uint32_t)socket_id];
    if (!sock->in_use) return -1;
    if (sock->local_port == 0u) {
        if (net_udp_bind(socket_id, 0u) != 0) return -1;
    }

    g_proto_stats.udp_tx++;

    uint8_t next_hop_mac[6];
    if (net_resolve_next_hop(dst_ip, next_hop_mac, 1000u) != 0) return -2;

    uint8_t udp[1500];
    uint16_t udp_len = (uint16_t)(8u + payload_len);
    net_write_be16(udp + 0, sock->local_port);
    net_write_be16(udp + 2, dst_port);
    net_write_be16(udp + 4, udp_len);
    net_write_be16(udp + 6, 0u);
    if (payload_len) {
        memcpy(udp + 8, payload, payload_len);
    }

    /* IPv4 allows UDP checksum = 0 (disabled). This improves compatibility while debugging DNS timeouts. */
    net_write_be16(udp + 6, 0u);

    if (net_send_ipv4_packet(next_hop_mac, dst_ip, NET_IPV4_PROTO_UDP, udp, udp_len, 64u) != 0) {
        return -3;
    }

    return 0;
}

int net_udp_recvfrom(int socket_id, void* out_payload, uint16_t payload_capacity, uint32_t timeout_ms,
                     uint8_t out_src_ip[4], uint16_t* out_src_port, uint16_t* out_payload_len) {
    if (socket_id < 0 || socket_id >= (int)NET_UDP_MAX_SOCKETS) return -1;
    net_udp_socket_t* sock = &g_udp_sockets[(uint32_t)socket_id];
    if (!sock->in_use) return -1;

    uint64_t deadline = pit_get_uptime_ms() + timeout_ms;
    for (;;) {
        int rc = net_udp_queue_pop(sock, out_payload, payload_capacity, out_src_ip, out_src_port, out_payload_len);
        if (rc >= 0) return rc;

        if (pit_get_uptime_ms() >= deadline) {
            return -2;
        }
        pit_sleep(5);
    }
}

static int net_dns_encode_name(const char* hostname, uint8_t* out, uint16_t out_cap, uint16_t* out_len) {
    if (!hostname || !out || !out_len) return -1;

    uint16_t off = 0;
    const char* p = hostname;
    uint16_t total_chars = 0;
    while (*p) {
        uint16_t label_len = 0;
        const char* label = p;
        while (*p && *p != '.') {
            if (*p == ' ') return -1;
            label_len++;
            total_chars++;
            p++;
        }
        if (label_len == 0u || label_len > 63u) return -1;
        if (total_chars > NET_DNS_MAX_NAME_LEN) return -1;
        if (off + 1u + label_len >= out_cap) return -1;

        out[off++] = (uint8_t)label_len;
        memcpy(out + off, label, label_len);
        off = (uint16_t)(off + label_len);

        if (*p == '.') p++;
    }

    if (off + 1u > out_cap) return -1;
    out[off++] = 0;
    *out_len = off;
    return 0;
}

static int net_dns_skip_name(const uint8_t* msg, uint16_t msg_len, uint16_t* io_off) {
    if (!msg || !io_off || *io_off >= msg_len) return -1;

    uint16_t off = *io_off;
    uint16_t end_off = off;
    uint8_t jumped = 0;
    uint8_t jumps = 0;

    while (off < msg_len) {
        uint8_t len = msg[off];
        if ((len & 0xC0u) == 0xC0u) {
            if (off + 1u >= msg_len) return -1;
            uint16_t ptr = (uint16_t)(((uint16_t)(len & 0x3Fu) << 8) | msg[off + 1u]);
            if (ptr >= msg_len) return -1;
            if (!jumped) end_off = (uint16_t)(off + 2u);
            off = ptr;
            jumped = 1;
            if (++jumps > 16u) return -1;
            continue;
        }
        if (len == 0u) {
            if (!jumped) end_off = (uint16_t)(off + 1u);
            *io_off = end_off;
            return 0;
        }
        if ((len & 0xC0u) != 0u) return -1;

        off++;
        if ((uint16_t)(off + len) > msg_len) return -1;
        off = (uint16_t)(off + len);
        if (!jumped) end_off = off;
    }
    return -1;
}

int net_dns_resolve_a(const char* hostname, uint8_t out_ip[4], uint32_t timeout_ms, const uint8_t dns_server_override[4]) {
    if (!hostname || !out_ip) return -1;
    if (timeout_ms == 0u) timeout_ms = 2000u;
    if (timeout_ms < 15000u) timeout_ms = 15000u;

    uint8_t dns_servers[3][4];
    uint32_t dns_server_count = 0;
    uint8_t qemu_dns[4] = {10, 0, 2, 3};

    if (dns_server_override && !net_ip_is_zero(dns_server_override)) {
        memcpy(dns_servers[dns_server_count++], dns_server_override, 4);
    } else {
        if (!net_ip_is_zero(g_ipv4.dns_server)) {
            memcpy(dns_servers[dns_server_count++], g_ipv4.dns_server, 4);
        }
        if (!net_ip_is_zero(g_ipv4.gateway)) {
            if (dns_server_count == 0 || !net_ip_equal(dns_servers[0], g_ipv4.gateway)) {
                memcpy(dns_servers[dns_server_count++], g_ipv4.gateway, 4);
            }
        }
        if (dns_server_count < 3u) {
            uint8_t dup = 0;
            for (uint32_t i = 0; i < dns_server_count; ++i) {
                if (net_ip_equal(dns_servers[i], qemu_dns)) {
                    dup = 1;
                    break;
                }
            }
            if (!dup) {
                memcpy(dns_servers[dns_server_count++], qemu_dns, 4);
            }
        }
    }
    if (dns_server_count == 0u) return -1;

    int sock = net_udp_open();
    if (sock < 0) return -1;

    uint8_t query[300];
    uint16_t name_len = 0;
    if (net_dns_encode_name(hostname, query + 12, (uint16_t)(sizeof(query) - 12u - 4u), &name_len) != 0) {
        net_udp_close(sock);
        return -1;
    }

    uint16_t txid = (uint16_t)((pit_get_uptime_ms() ^ (uint64_t)(g_ping_sequence << 3)) & 0xFFFFu);
    net_write_be16(query + 0, txid);
    net_write_be16(query + 2, 0x0100u);
    net_write_be16(query + 4, 1u);
    net_write_be16(query + 6, 0u);
    net_write_be16(query + 8, 0u);
    net_write_be16(query + 10, 0u);
    net_write_be16(query + 12u + name_len + 0u, 1u);
    net_write_be16(query + 12u + name_len + 2u, 1u);
    uint16_t query_len = (uint16_t)(12u + name_len + 4u);

    g_proto_stats.dns_queries++;
    for (uint32_t si = 0; si < dns_server_count; ++si) {
        uint8_t* dns_server = dns_servers[si];
        if (net_udp_sendto(sock, dns_server, NET_DNS_PORT, query, query_len) != 0) {
            continue;
        }
        g_dns_last_local_port = g_udp_sockets[(uint32_t)sock].local_port;

        uint64_t deadline = pit_get_uptime_ms() + timeout_ms;
        uint8_t retried = 0;
        uint64_t retry_at = pit_get_uptime_ms() + (timeout_ms / 2u);
        for (;;) {
            uint64_t now = pit_get_uptime_ms();
            if (now >= deadline) {
                g_proto_stats.dns_timeouts++;
                break;
            }

            if (!retried && now >= retry_at) {
                (void)net_udp_sendto(sock, dns_server, NET_DNS_PORT, query, query_len);
                retried = 1;
            }

            uint32_t wait_ms = (uint32_t)(deadline - now);
            if (wait_ms > 100u) wait_ms = 100u;

            uint8_t resp[NET_UDP_MAX_PAYLOAD];
            uint8_t src_ip[4];
            uint16_t src_port = 0;
            uint16_t resp_len = 0;
            int rx = net_udp_recvfrom(sock, resp, sizeof(resp), wait_ms, src_ip, &src_port, &resp_len);
            if (rx < 0) continue;
            g_proto_stats.dns_seen++;
            if (resp_len < 12u) {
                g_proto_stats.dns_short++;
                continue;
            }

            (void)src_ip;
            (void)src_port;
            if (net_read_be16(resp + 0) != txid) {
                /* Unrelated packet on same local UDP port. */
                continue;
            }

            uint16_t flags = net_read_be16(resp + 2);
            uint16_t qdcount = net_read_be16(resp + 4);
            uint16_t ancount = net_read_be16(resp + 6);
            uint8_t is_response = (uint8_t)((flags >> 15) & 1u);
            uint8_t rcode = (uint8_t)(flags & 0x0Fu);
            if (!is_response || rcode != 0u || ancount == 0u) {
                g_proto_stats.dns_bad_header++;
                /* Keep waiting for a valid response within this attempt window. */
                continue;
            }

            uint16_t off = 12u;
            for (uint16_t qi = 0; qi < qdcount; ++qi) {
                if (net_dns_skip_name(resp, resp_len, &off) != 0) {
                    g_proto_stats.dns_bad_header++;
                    continue;
                }
                if (off + 4u > resp_len) {
                    g_proto_stats.dns_bad_header++;
                    continue;
                }
                off = (uint16_t)(off + 4u);
            }

            for (uint16_t ai = 0; ai < ancount; ++ai) {
                if (net_dns_skip_name(resp, resp_len, &off) != 0) {
                    g_proto_stats.dns_bad_header++;
                    continue;
                }
                if (off + 10u > resp_len) {
                    g_proto_stats.dns_bad_header++;
                    continue;
                }

                uint16_t type = net_read_be16(resp + off + 0u);
                uint16_t klass = net_read_be16(resp + off + 2u);
                uint16_t rdlen = net_read_be16(resp + off + 8u);
                off = (uint16_t)(off + 10u);
                if (off + rdlen > resp_len) {
                    g_proto_stats.dns_bad_header++;
                    continue;
                }

                if (type == 1u && klass == 1u && rdlen == 4u) {
                    memcpy(out_ip, resp + off, 4);
                    g_proto_stats.dns_replies++;
                    net_udp_close(sock);
                    return 0;
                }

                off = (uint16_t)(off + rdlen);
            }

            g_proto_stats.dns_no_a_answer++;
            /* Valid DNS packet but no A record; keep waiting for another answer. */
            continue;
        }
    }

    g_proto_stats.dns_failures++;
    net_udp_close(sock);
    return -3;
}

void net_get_mac(uint8_t out_mac[6]) {
    if (!out_mac) return;

    if (g_driver == NET_DRIVER_E1000) {
        e1000_get_mac(out_mac);
        return;
    }
    if (g_driver == NET_DRIVER_RTL8139) {
        rtl8139_get_mac(out_mac);
        return;
    }

    for (uint32_t i = 0; i < 6; ++i) out_mac[i] = 0;
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
        out_stats->arp_rx = g_proto_stats.arp_rx;
        out_stats->arp_tx = g_proto_stats.arp_tx;
        out_stats->ipv4_rx = g_proto_stats.ipv4_rx;
        out_stats->ipv4_tx = g_proto_stats.ipv4_tx;
        out_stats->icmp_rx = g_proto_stats.icmp_rx;
        out_stats->icmp_tx = g_proto_stats.icmp_tx;
        out_stats->udp_rx = g_proto_stats.udp_rx;
        out_stats->udp_tx = g_proto_stats.udp_tx;
        out_stats->ping_requests = g_proto_stats.ping_requests;
        out_stats->ping_replies = g_proto_stats.ping_replies;
        out_stats->dns_queries = g_proto_stats.dns_queries;
        out_stats->dns_replies = g_proto_stats.dns_replies;
        out_stats->dns_timeouts = g_proto_stats.dns_timeouts;
        out_stats->dns_failures = g_proto_stats.dns_failures;
        out_stats->dns_seen = g_proto_stats.dns_seen;
        out_stats->dns_short = g_proto_stats.dns_short;
        out_stats->dns_bad_header = g_proto_stats.dns_bad_header;
        out_stats->dns_no_a_answer = g_proto_stats.dns_no_a_answer;
        out_stats->udp_bad_checksum = g_proto_stats.udp_bad_checksum;
        out_stats->udp_no_socket = g_proto_stats.udp_no_socket;
        out_stats->udp_queue_drop = g_proto_stats.udp_queue_drop;
        out_stats->udp_last_nosock_dst_port = g_udp_last_nosock_dst_port;
        out_stats->dns_last_local_port = g_dns_last_local_port;
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
        out_stats->arp_rx = g_proto_stats.arp_rx;
        out_stats->arp_tx = g_proto_stats.arp_tx;
        out_stats->ipv4_rx = g_proto_stats.ipv4_rx;
        out_stats->ipv4_tx = g_proto_stats.ipv4_tx;
        out_stats->icmp_rx = g_proto_stats.icmp_rx;
        out_stats->icmp_tx = g_proto_stats.icmp_tx;
        out_stats->udp_rx = g_proto_stats.udp_rx;
        out_stats->udp_tx = g_proto_stats.udp_tx;
        out_stats->ping_requests = g_proto_stats.ping_requests;
        out_stats->ping_replies = g_proto_stats.ping_replies;
        out_stats->dns_queries = g_proto_stats.dns_queries;
        out_stats->dns_replies = g_proto_stats.dns_replies;
        out_stats->dns_timeouts = g_proto_stats.dns_timeouts;
        out_stats->dns_failures = g_proto_stats.dns_failures;
        out_stats->dns_seen = g_proto_stats.dns_seen;
        out_stats->dns_short = g_proto_stats.dns_short;
        out_stats->dns_bad_header = g_proto_stats.dns_bad_header;
        out_stats->dns_no_a_answer = g_proto_stats.dns_no_a_answer;
        out_stats->udp_bad_checksum = g_proto_stats.udp_bad_checksum;
        out_stats->udp_no_socket = g_proto_stats.udp_no_socket;
        out_stats->udp_queue_drop = g_proto_stats.udp_queue_drop;
        out_stats->udp_last_nosock_dst_port = g_udp_last_nosock_dst_port;
        out_stats->dns_last_local_port = g_dns_last_local_port;
        return;
    }

    out_stats->interrupts = 0;
    out_stats->rx_packets = 0;
    out_stats->tx_packets = 0;
    out_stats->rx_irqs = 0;
    out_stats->tx_irqs = 0;
    out_stats->link_events = 0;
    out_stats->rx_drops = 0;
    out_stats->arp_rx = g_proto_stats.arp_rx;
    out_stats->arp_tx = g_proto_stats.arp_tx;
    out_stats->ipv4_rx = g_proto_stats.ipv4_rx;
    out_stats->ipv4_tx = g_proto_stats.ipv4_tx;
    out_stats->icmp_rx = g_proto_stats.icmp_rx;
    out_stats->icmp_tx = g_proto_stats.icmp_tx;
    out_stats->udp_rx = g_proto_stats.udp_rx;
    out_stats->udp_tx = g_proto_stats.udp_tx;
    out_stats->ping_requests = g_proto_stats.ping_requests;
    out_stats->ping_replies = g_proto_stats.ping_replies;
    out_stats->dns_queries = g_proto_stats.dns_queries;
    out_stats->dns_replies = g_proto_stats.dns_replies;
    out_stats->dns_timeouts = g_proto_stats.dns_timeouts;
    out_stats->dns_failures = g_proto_stats.dns_failures;
    out_stats->dns_seen = g_proto_stats.dns_seen;
    out_stats->dns_short = g_proto_stats.dns_short;
    out_stats->dns_bad_header = g_proto_stats.dns_bad_header;
    out_stats->dns_no_a_answer = g_proto_stats.dns_no_a_answer;
    out_stats->udp_bad_checksum = g_proto_stats.udp_bad_checksum;
    out_stats->udp_no_socket = g_proto_stats.udp_no_socket;
    out_stats->udp_queue_drop = g_proto_stats.udp_queue_drop;
    out_stats->udp_last_nosock_dst_port = g_udp_last_nosock_dst_port;
    out_stats->dns_last_local_port = g_dns_last_local_port;
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

