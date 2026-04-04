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

#define NET_ARP_TABLE_SIZE 8u
#define NET_ARP_TTL_MS 300000u
#define NET_PING_ID 0xD30Au

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
    uint32_t ping_requests;
    uint32_t ping_replies;
} net_proto_stats_t;

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
    .configured = 1,
};

static net_arp_entry_t g_arp_table[NET_ARP_TABLE_SIZE];
static uint32_t g_arp_next_slot = 0;
static net_proto_stats_t g_proto_stats;
static volatile uint16_t g_ping_sequence = 1;
static net_ping_wait_t g_ping_wait;

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

    uint8_t next_hop_ip[4];
    uint8_t next_hop_mac[6];
    if (net_ip_same_subnet(target_ip, g_ipv4.address, g_ipv4.netmask) || net_ip_is_zero(g_ipv4.gateway)) {
        memcpy(next_hop_ip, target_ip, 4);
    } else {
        memcpy(next_hop_ip, g_ipv4.gateway, 4);
    }

    uint64_t deadline = pit_get_uptime_ms() + timeout_ms;
    while (net_arp_cache_lookup(next_hop_ip, next_hop_mac) != 0) {
        (void)net_send_arp_packet(1, 0, next_hop_ip);
        if (pit_get_uptime_ms() >= deadline) {
            return -2;
        }
        pit_sleep(10);
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
        out_stats->ping_requests = g_proto_stats.ping_requests;
        out_stats->ping_replies = g_proto_stats.ping_replies;
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
        out_stats->ping_requests = g_proto_stats.ping_requests;
        out_stats->ping_replies = g_proto_stats.ping_replies;
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
    out_stats->ping_requests = g_proto_stats.ping_requests;
    out_stats->ping_replies = g_proto_stats.ping_replies;
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

