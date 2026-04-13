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

#define NET_ETH_TYPE_ARP  0x0806u
#define NET_ETH_TYPE_IPV4 0x0800u

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
#define NET_TCP_EPHEMERAL_START 40000u
#define NET_TCP_EPHEMERAL_END   49999u
#define NET_TCP_RX_BUFFER_SIZE   2048u

#define NET_TCP_FLAG_FIN 0x01u
#define NET_TCP_FLAG_SYN 0x02u
#define NET_TCP_FLAG_RST 0x04u
#define NET_TCP_FLAG_ACK 0x10u

typedef enum net_tcp_state {
    NET_TCP_STATE_CLOSED = 0,
    NET_TCP_STATE_SYN_SENT,
    NET_TCP_STATE_ESTABLISHED,
    NET_TCP_STATE_CLOSE_WAIT,
    NET_TCP_STATE_FIN_WAIT_1,
    NET_TCP_STATE_FIN_WAIT_2,
    NET_TCP_STATE_LAST_ACK,
    NET_TCP_STATE_RESET,
} net_tcp_state_t;

typedef struct net_tcp_socket_state {
    volatile uint8_t in_use;
    volatile uint8_t state;
    uint16_t local_port;
    uint16_t remote_port;
    uint8_t remote_ip[4];
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    volatile uint8_t peer_closed;
    volatile uint8_t rx_overflow;
    volatile uint16_t rx_len;
    uint8_t rx_buf[NET_TCP_RX_BUFFER_SIZE];
    volatile int connect_result;
} net_tcp_socket_state_t;

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

static net_driver_kind_t g_driver = NET_DRIVER_NONE;
static uint8_t g_ipv4_addr[4] = {10u, 0u, 2u, 15u};
static uint8_t g_ipv4_netmask[4] = {255u, 255u, 255u, 0u};
static uint8_t g_ipv4_gateway[4] = {10u, 0u, 2u, 2u};
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
static uint16_t g_tcp_next_ephemeral = NET_TCP_EPHEMERAL_START;
static uint16_t g_dns_next_id = 1u;
static net_tcp_debug_stats_t g_tcp_dbg = {0};

static int net_ipv4_select_next_hop(const uint8_t dst_ip[4], uint8_t out_next_hop[4]);
static int net_send_ipv4(const uint8_t* dst_mac,
                         const uint8_t dst_ip[4],
                         uint8_t proto,
                         const uint8_t* l4_payload,
                         uint16_t l4_len);

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

static int net_tcp_socket_alloc(void) {
    for (uint32_t i = 0; i < NET_TCP_MAX_SOCKETS; ++i) {
        if (!g_tcp_sockets[i].in_use) {
            memset(&g_tcp_sockets[i], 0, sizeof(g_tcp_sockets[i]));
            g_tcp_sockets[i].in_use = 1;
            g_tcp_sockets[i].state = NET_TCP_STATE_CLOSED;
            g_tcp_sockets[i].connect_result = NET_TCP_ERR_TIMEOUT;
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
           state == NET_TCP_STATE_ESTABLISHED ||
           state == NET_TCP_STATE_CLOSE_WAIT ||
           state == NET_TCP_STATE_FIN_WAIT_1 ||
           state == NET_TCP_STATE_FIN_WAIT_2 ||
           state == NET_TCP_STATE_LAST_ACK;
}

static int net_tcp_send_segment(const uint8_t dst_ip[4],
                                uint16_t src_port,
                                uint16_t dst_port,
                                uint32_t seq,
                                uint32_t ack,
                                uint8_t flags,
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
    net_write_be16(tcp + 14, 0x4000u);
    net_write_be16(tcp + 16, 0u);
    net_write_be16(tcp + 18, 0u);
    if (include_mss) {
        /* MSS option: kind=2, len=4, value=1460. Many edge stacks drop options-less SYNs. */
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
    payload = tcp + hdr_len;
    payload_len = (uint16_t)(tcp_len - hdr_len);

    idx = net_tcp_find_socket(src_ip, src_port, dst_port);
    if (idx < 0) {
        g_tcp_dbg.tuple_miss++;
        memcpy(g_tcp_dbg.last_miss_src_ip, src_ip, 4);
        g_tcp_dbg.last_miss_src_port = src_port;
        g_tcp_dbg.last_miss_dst_port = dst_port;
        g_tcp_dbg.last_miss_flags = flags;
        g_tcp_dbg.last_miss_seq = seq;
        g_tcp_dbg.last_miss_ack = ack;
        g_tcp_dbg.last_miss_arrival_ms = (uint32_t)pit_get_uptime_ms();
        return;
    }

    s = &g_tcp_sockets[idx];

    if ((flags & NET_TCP_FLAG_ACK) != 0u && ack > s->snd_una) {
        s->snd_una = ack;
    }

    if ((flags & NET_TCP_FLAG_RST) != 0u) {
        g_tcp_dbg.rst_seen++;
        s->state = NET_TCP_STATE_RESET;
        s->connect_result = NET_TCP_ERR_RESET;
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
            (void)net_tcp_send_segment(s->remote_ip,
                                       s->local_port,
                                       s->remote_port,
                                       s->snd_nxt,
                                       s->rcv_nxt,
                                       NET_TCP_FLAG_ACK,
                                       NULL,
                                       0u);
        }
        return;
    }

    if (s->state == NET_TCP_STATE_ESTABLISHED) {
        if (payload_len > 0u) {
            if (seq == s->rcv_nxt) {
                uint16_t free_space = (uint16_t)(NET_TCP_RX_BUFFER_SIZE - s->rx_len);
                uint16_t copy_len = payload_len > free_space ? free_space : payload_len;
                if (copy_len > 0u) {
                    memcpy(s->rx_buf + s->rx_len, payload, copy_len);
                    s->rx_len = (uint16_t)(s->rx_len + copy_len);
                }
                if (copy_len < payload_len) {
                    s->rx_overflow = 1u;
                }
                s->rcv_nxt = seq + payload_len;
            }
            (void)net_tcp_send_segment(s->remote_ip,
                                       s->local_port,
                                       s->remote_port,
                                       s->snd_nxt,
                                       s->rcv_nxt,
                                       NET_TCP_FLAG_ACK,
                                       NULL,
                                       0u);
        }

        if ((flags & NET_TCP_FLAG_FIN) != 0u) {
            s->rcv_nxt = seq + 1u;
            (void)net_tcp_send_segment(s->remote_ip,
                                       s->local_port,
                                       s->remote_port,
                                       s->snd_nxt,
                                       s->rcv_nxt,
                                       NET_TCP_FLAG_ACK,
                                       NULL,
                                       0u);
            s->peer_closed = 1u;
            s->state = NET_TCP_STATE_CLOSE_WAIT;
        }
        return;
    }

    if (s->state == NET_TCP_STATE_CLOSE_WAIT) {
        if (payload_len > 0u) {
            if (seq == s->rcv_nxt) {
                s->rcv_nxt = seq + payload_len;
            }
            (void)net_tcp_send_segment(s->remote_ip,
                                       s->local_port,
                                       s->remote_port,
                                       s->snd_nxt,
                                       s->rcv_nxt,
                                       NET_TCP_FLAG_ACK,
                                       NULL,
                                       0u);
        }
        return;
    }

    if (s->state == NET_TCP_STATE_FIN_WAIT_1) {
        if ((flags & NET_TCP_FLAG_ACK) != 0u && ack == s->snd_nxt) {
            s->state = NET_TCP_STATE_FIN_WAIT_2;
        }
        if ((flags & NET_TCP_FLAG_FIN) != 0u) {
            s->rcv_nxt = seq + 1u;
            (void)net_tcp_send_segment(s->remote_ip,
                                       s->local_port,
                                       s->remote_port,
                                       s->snd_nxt,
                                       s->rcv_nxt,
                                       NET_TCP_FLAG_ACK,
                                       NULL,
                                       0u);
            s->state = NET_TCP_STATE_CLOSED;
        }
        return;
    }

    if (s->state == NET_TCP_STATE_FIN_WAIT_2) {
        if ((flags & NET_TCP_FLAG_FIN) != 0u) {
            s->rcv_nxt = seq + 1u;
            (void)net_tcp_send_segment(s->remote_ip,
                                       s->local_port,
                                       s->remote_port,
                                       s->snd_nxt,
                                       s->rcv_nxt,
                                       NET_TCP_FLAG_ACK,
                                       NULL,
                                       0u);
            s->state = NET_TCP_STATE_CLOSED;
        }
        return;
    }

    if (s->state == NET_TCP_STATE_LAST_ACK) {
        if ((flags & NET_TCP_FLAG_ACK) != 0u && ack == s->snd_nxt) {
            s->state = NET_TCP_STATE_CLOSED;
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
    if (g_driver == NET_DRIVER_E1000) return e1000_send_raw(data, len);
    if (g_driver == NET_DRIVER_RTL8139) return rtl8139_send_raw(data, len);
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

static int net_ipv4_is_local(const uint8_t ip[4]) {
    return memcmp(ip, g_ipv4_addr, 4) == 0;
}

static int net_ipv4_is_broadcast(const uint8_t ip[4]) {
    return ip[0] == 255u && ip[1] == 255u && ip[2] == 255u && ip[3] == 255u;
}

static int net_ipv4_is_same_subnet(const uint8_t lhs[4], const uint8_t rhs[4]) {
    for (uint32_t i = 0; i < 4u; ++i) {
        if ((lhs[i] & g_ipv4_netmask[i]) != (rhs[i] & g_ipv4_netmask[i])) return 0;
    }
    return 1;
}

static int net_ipv4_select_next_hop(const uint8_t dst_ip[4], uint8_t out_next_hop[4]) {
    if (!dst_ip || !out_next_hop) return -1;
    if (net_ipv4_is_zero(g_ipv4_addr) || net_ipv4_is_zero(dst_ip)) return -1;

    if (net_ipv4_is_broadcast(dst_ip) || net_ipv4_is_same_subnet(g_ipv4_addr, dst_ip)) {
        memcpy(out_next_hop, dst_ip, 4);
        return 0;
    }

    if (net_ipv4_is_zero(g_ipv4_gateway)) return -1;
    memcpy(out_next_hop, g_ipv4_gateway, 4);
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

static int net_send_ipv4(const uint8_t* dst_mac,
                         const uint8_t dst_ip[4],
                         uint8_t proto,
                         const uint8_t* l4_payload,
                         uint16_t l4_len) {
    uint8_t pkt[20 + 1500];
    uint16_t total_len;

    if (!dst_mac || !dst_ip || !l4_payload || l4_len == 0u || l4_len > 1480u) return -1;
    if (net_ipv4_is_zero(g_ipv4_addr)) return -1;

    total_len = (uint16_t)(20u + l4_len);
    memset(pkt, 0, 20);
    pkt[0] = 0x45u;
    pkt[1] = 0u;
    net_write_be16(pkt + 2, total_len);
    net_write_be16(pkt + 4, 0u);
    net_write_be16(pkt + 6, 0x4000u);
    pkt[8] = 64u;
    pkt[9] = proto;
    memcpy(pkt + 12, g_ipv4_addr, 4);
    memcpy(pkt + 16, dst_ip, 4);
    net_write_be16(pkt + 10, net_checksum16(pkt, 20));
    memcpy(pkt + 20, l4_payload, l4_len);

    return net_send_eth(dst_mac, NET_ETH_TYPE_IPV4, pkt, total_len);
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

static void net_handle_arp_frame(const uint8_t* frame, uint16_t len) {
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

static void net_on_rx_frame(const uint8_t* frame, uint16_t len) {
    uint16_t eth_type;

    if (len < 14u) {
        g_arp_stats.dropped_frames++;
        return;
    }

    eth_type = net_read_be16(frame + 12);
    if (eth_type == NET_ETH_TYPE_ARP) {
        net_handle_arp_frame(frame, len);
        return;
    }

    if (eth_type == NET_ETH_TYPE_IPV4 && len >= 34u) {
        const uint8_t* ip = frame + 14;
        const uint8_t* src_ip = ip + 12;
        const uint8_t* dst_ip = ip + 16;
        uint8_t ihl_words = (uint8_t)(ip[0] & 0x0Fu);
        uint16_t ihl = (uint16_t)(ihl_words * 4u);
        uint16_t total_len = net_read_be16(ip + 2);

        if ((ip[0] >> 4) != 4u || ihl < 20u || (14u + total_len) > len || total_len < ihl) {
            g_arp_stats.dropped_frames++;
            return;
        }
        if (net_checksum16(ip, ihl) != 0u) {
            g_arp_stats.dropped_frames++;
            return;
        }
        if (net_ipv4_is_zero(g_ipv4_addr)) return;
        if (!net_ipv4_is_local(dst_ip) && !net_ipv4_is_broadcast(dst_ip)) {
            g_arp_stats.dropped_frames++;
            return;
        }

        if (ip[9] == NET_IP_PROTO_ICMP) {
            const uint8_t* icmp = ip + ihl;
            uint16_t icmp_len = (uint16_t)(total_len - ihl);

            if (icmp_len < 8u) {
                g_arp_stats.dropped_frames++;
                return;
            }
            if (net_checksum16(icmp, icmp_len) != 0u) {
                g_arp_stats.dropped_frames++;
                return;
            }

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

            if (icmp[0] == NET_ICMP_ECHO_REQUEST && icmp[1] == 0u &&
                memcmp(dst_ip, g_ipv4_addr, 4) == 0 && !net_ipv4_is_zero(g_ipv4_addr)) {
                uint16_t id = net_read_be16(icmp + 4);
                uint16_t seq = net_read_be16(icmp + 6);
                uint16_t body_len = (uint16_t)(icmp_len - 8u);
                (void)net_send_icmp_echo(frame + 6, src_ip, NET_ICMP_ECHO_REPLY, id, seq, icmp + 8, body_len);
            }
        } else if (ip[9] == NET_IP_PROTO_UDP) {
            net_handle_udp_ipv4(src_ip, ip + ihl, (uint16_t)(total_len - ihl));
        } else if (ip[9] == NET_IP_PROTO_TCP) {
            net_handle_tcp_ipv4(src_ip, dst_ip, ip + ihl, (uint16_t)(total_len - ihl));
        }
    }
}

int net_initialize(void) {
    pci_initialize();
    memset(g_udp_sockets, 0, sizeof(g_udp_sockets));
    g_udp_next_ephemeral = NET_UDP_EPHEMERAL_START;
    memset(g_tcp_sockets, 0, sizeof(g_tcp_sockets));
    g_tcp_next_ephemeral = NET_TCP_EPHEMERAL_START;

    if (e1000_initialize() == 0) {
        g_driver = NET_DRIVER_E1000;
        e1000_set_rx_callback(net_on_rx_frame);
        return 0;
    }

    if (rtl8139_initialize() == 0) {
        g_driver = NET_DRIVER_RTL8139;
        rtl8139_set_rx_callback(net_on_rx_frame);
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
    if (!net_is_ready() || net_ipv4_is_zero(g_ipv4_addr)) return NET_PING_ERR_INVALID;

    if (net_ipv4_select_next_hop(target_ip, next_hop) != 0) return NET_PING_ERR_INVALID;
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
    if (!net_is_ready() || net_ipv4_is_zero(g_ipv4_addr)) return NET_UDP_ERR_NOT_READY;
    if (dst_port == 0u || payload_len > 1472u) return NET_UDP_ERR_INVALID;

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
        return net_udp_deliver_datagram(dst_port, g_ipv4_addr, src_port, bytes, payload_len)
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

        pit_sleep(1u);
        elapsed++;
    }
}

static int net_dns_encode_qname(const char* hostname, uint8_t* out, uint16_t out_capacity, uint16_t* out_len) {
    uint16_t pos = 0;
    uint16_t label_start = 0;
    uint16_t i = 0;

    if (!hostname || !out || !out_len || out_capacity < 2u) return -1;
    if (hostname[0] == '\0') return -1;

    while (hostname[i] != '\0') {
        if (hostname[i] == '.') {
            uint16_t label_len = (uint16_t)(i - label_start);
            if (label_len == 0u || label_len > 63u) return -1;
            if ((uint16_t)(pos + 1u + label_len) >= out_capacity) return -1;
            out[pos++] = (uint8_t)label_len;
            memcpy(out + pos, hostname + label_start, label_len);
            pos = (uint16_t)(pos + label_len);
            label_start = (uint16_t)(i + 1u);
        }
        i++;
    }

    if (i == label_start) return -1;
    {
        uint16_t label_len = (uint16_t)(i - label_start);
        if (label_len == 0u || label_len > 63u) return -1;
        if ((uint16_t)(pos + 1u + label_len + 1u) > out_capacity) return -1;
        out[pos++] = (uint8_t)label_len;
        memcpy(out + pos, hostname + label_start, label_len);
        pos = (uint16_t)(pos + label_len);
    }

    out[pos++] = 0u;
    *out_len = pos;
    return 0;
}

static int net_dns_skip_name(const uint8_t* msg, uint16_t msg_len, uint16_t start_off, uint16_t* out_next) {
    uint16_t off = start_off;
    uint16_t steps = 0;

    if (!msg || !out_next || off >= msg_len) return -1;

    while (off < msg_len && steps < msg_len) {
        uint8_t len = msg[off];
        if ((len & 0xC0u) == 0xC0u) {
            if ((uint16_t)(off + 1u) >= msg_len) return -1;
            *out_next = (uint16_t)(off + 2u);
            return 0;
        }
        if (len == 0u) {
            *out_next = (uint16_t)(off + 1u);
            return 0;
        }
        if ((len & 0xC0u) != 0u) return -1;
        off = (uint16_t)(off + 1u + len);
        if (off > msg_len) return -1;
        steps++;
    }

    return -1;
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
    uint32_t timeout = timeout_ms == 0u ? 2000u : timeout_ms;
    int sock = -1;
    int rc;
    net_udp_endpoint_t from;
    uint8_t default_dns[2][4] = {{10u, 0u, 2u, 3u}, {10u, 0u, 2u, 2u}};
    uint32_t server_count = dns_server_ip ? 1u : 2u;
    uint8_t timeout_only = 1u;

    if (!hostname || !out_ip) return NET_DNS_ERR_INVALID;
    if (dns_server_ip && net_ipv4_is_zero(dns_server_ip)) return NET_DNS_ERR_INVALID;

    sock = net_udp_socket_open();
    if (sock < 0) return NET_DNS_ERR_SOCKET;

    rc = net_udp_socket_bind(sock, 0u);
    if (rc < 0) {
        (void)net_udp_socket_close(sock);
        return NET_DNS_ERR_SOCKET;
    }

    id = g_dns_next_id++;
    memset(query, 0, sizeof(query));
    net_write_be16(query + 0, id);
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
        const uint8_t* server = dns_server_ip ? dns_server_ip : default_dns[s];
        if (net_ipv4_is_zero(server)) continue;

        for (uint32_t attempt = 0; attempt < 3u; ++attempt) {
            rc = net_udp_socket_sendto(sock, server, 53u, query, query_len);
            if (rc != NET_UDP_OK) {
                (void)net_udp_socket_close(sock);
                return NET_DNS_ERR_TX;
            }

            rc = net_udp_socket_recvfrom(sock, response, sizeof(response), &rx_len, &from, timeout);
            if (rc == NET_UDP_ERR_WOULD_BLOCK) {
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
                rdlen = net_read_be16(response + off + 8u);
                off = (uint16_t)(off + 10u);

                if ((uint16_t)(off + rdlen) > rx_len) {
                    (void)net_udp_socket_close(sock);
                    return NET_DNS_ERR_FORMAT;
                }
                if (type == 1u && cls == 1u && rdlen == 4u) {
                    memcpy(out_ip, response + off, 4);
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
    s->snd_una = isn;
    s->snd_nxt = isn + 1u;
    s->rcv_nxt = 0u;
    s->state = NET_TCP_STATE_SYN_SENT;
    s->connect_result = NET_TCP_ERR_TIMEOUT;

    if (net_tcp_send_segment(dst_ip, local_port, dst_port, isn, 0u, NET_TCP_FLAG_SYN, NULL, 0u) != 0) {
        net_tcp_socket_release(sid);
        return NET_TCP_ERR_ARP;
    }
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

        if (chunk > 1200u) chunk = 1200u;
        if (net_tcp_send_segment(s->remote_ip,
                                 s->local_port,
                                 s->remote_port,
                                 s->snd_nxt,
                                 s->rcv_nxt,
                                 NET_TCP_FLAG_ACK,
                                 bytes + sent,
                                 chunk) != 0) {
            return NET_TCP_ERR_TX;
        }

        s->snd_nxt += chunk;
        wait_start_ms = (uint32_t)pit_get_uptime_ms();
        for (;;) {
            if (s->state == NET_TCP_STATE_RESET) return NET_TCP_ERR_RESET;
            if (s->snd_una >= s->snd_nxt) break;
            wait_ms = (uint32_t)pit_get_uptime_ms() - wait_start_ms;
            if (wait_ms >= timeout) return NET_TCP_ERR_TIMEOUT;
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
            return NET_TCP_OK;
        }

        if (s->state == NET_TCP_STATE_RESET) return NET_TCP_ERR_RESET;
        if (s->peer_closed || s->state == NET_TCP_STATE_CLOSED) return NET_TCP_ERR_CLOSED;
        if (timeout == 0u) return NET_TCP_ERR_WOULD_BLOCK;
        if (elapsed >= timeout) return NET_TCP_ERR_WOULD_BLOCK;

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
        if (net_tcp_send_segment(s->remote_ip,
                                 s->local_port,
                                 s->remote_port,
                                 s->snd_nxt,
                                 s->rcv_nxt,
                                 NET_TCP_FLAG_FIN | NET_TCP_FLAG_ACK,
                                 NULL,
                                 0u) != 0) {
            return NET_TCP_ERR_TX;
        }
        s->snd_nxt += 1u;
        s->state = NET_TCP_STATE_FIN_WAIT_1;
    } else if (s->state == NET_TCP_STATE_CLOSE_WAIT) {
        if (net_tcp_send_segment(s->remote_ip,
                                 s->local_port,
                                 s->remote_port,
                                 s->snd_nxt,
                                 s->rcv_nxt,
                                 NET_TCP_FLAG_FIN | NET_TCP_FLAG_ACK,
                                 NULL,
                                 0u) != 0) {
            return NET_TCP_ERR_TX;
        }
        s->snd_nxt += 1u;
        s->state = NET_TCP_STATE_LAST_ACK;
    }

    start_ms = (uint32_t)pit_get_uptime_ms();
    while (((uint32_t)pit_get_uptime_ms() - start_ms) < timeout) {
        if (s->state == NET_TCP_STATE_CLOSED) {
            net_tcp_socket_release(socket_id);
            return NET_TCP_OK;
        }
        if (s->state == NET_TCP_STATE_RESET) {
            int rc = s->connect_result;
            net_tcp_socket_release(socket_id);
            return rc;
        }
        pit_sleep(10u);
    }

    net_tcp_socket_release(socket_id);
    return NET_TCP_ERR_TIMEOUT;
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

