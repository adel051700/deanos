#include "ksock_tcp.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "kernel/net_lwip.h"
#include "kernel/kheap.h"
#include "kernel/task.h"
#include "kernel/pit.h"
#include <string.h>

#define TCP_RXBUF 8192
#define ACCEPT_Q  4

struct ksock_tcp {
    struct tcp_pcb* pcb;
    uint8_t  rx[TCP_RXBUF];
    uint32_t rx_head, rx_len;     /* ring fill */
    volatile int connected;
    volatile int peer_closed;
    volatile int err;
    /* listener accept queue */
    struct ksock_tcp* acc_q[ACCEPT_Q];
    volatile uint32_t acc_head, acc_tail;
    int is_listener;
};

static err_t on_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t e);
static err_t on_sent(void* arg, struct tcp_pcb* pcb, u16_t len);
static void  on_err(void* arg, err_t e);
static err_t on_connected(void* arg, struct tcp_pcb* pcb, err_t e);

static ksock_tcp_t* alloc_sock(struct tcp_pcb* pcb) {
    ksock_tcp_t* s = (ksock_tcp_t*)kmalloc(sizeof(*s));
    if (!s) return 0;
    memset(s, 0, sizeof(*s));
    s->pcb = pcb;
    if (pcb) {
        tcp_arg(pcb, s);
        tcp_recv(pcb, on_recv);
        tcp_sent(pcb, on_sent);
        tcp_err(pcb, on_err);
    }
    return s;
}

static err_t on_connected(void* arg, struct tcp_pcb* pcb, err_t e) {
    ksock_tcp_t* s = (ksock_tcp_t*)arg; (void)pcb;
    if (e == ERR_OK) s->connected = 1; else s->err = 1;
    return ERR_OK;
}

static err_t on_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t e) {
    ksock_tcp_t* s = (ksock_tcp_t*)arg;
    if (e != ERR_OK) { if (p) pbuf_free(p); s->err = 1; return ERR_OK; }
    if (!p) { s->peer_closed = 1; return ERR_OK; }   /* FIN */
    {
        uint16_t avail = (uint16_t)(TCP_RXBUF - s->rx_len);
        /* All-or-nothing: if the whole pbuf doesn't fit, refuse it untouched
         * (return ERR_MEM). lwIP stores the pbuf as pcb->refused_data
         * (tcp_in.c) and retries delivery via tcp_process_refused_data() on
         * a later tcp_fasttmr() tick (tcp.c), i.e. the next net_service_tick()
         * pump after the app has drained the ring via ksock_tcp_recv(). Since
         * we neither call tcp_recved() nor advance rcv_nxt for the refused
         * data, rcv_wnd stays closed and the peer will not send data we can't
         * yet buffer. We must NOT copy partial data, call tcp_recved(), or
         * pbuf_free() on this path. */
        if (p->tot_len > avail) {
            return ERR_MEM;
        }
        {
            uint16_t off = 0, wpos = (uint16_t)((s->rx_head + s->rx_len) % TCP_RXBUF);
            uint16_t take = p->tot_len;
            while (off < take) {
                uint16_t chunk = (uint16_t)(TCP_RXBUF - wpos);
                if (chunk > (uint16_t)(take - off)) chunk = (uint16_t)(take - off);
                pbuf_copy_partial(p, s->rx + wpos, chunk, off);
                wpos = (uint16_t)((wpos + chunk) % TCP_RXBUF);
                off += chunk;
            }
            s->rx_len += take;
            tcp_recved(pcb, take);
            pbuf_free(p);
        }
    }
    return ERR_OK;
}

static err_t on_sent(void* arg, struct tcp_pcb* pcb, u16_t len) {
    (void)arg; (void)pcb; (void)len; return ERR_OK;   /* writable polled via tcp_sndbuf */
}

static void on_err(void* arg, err_t e) {
    ksock_tcp_t* s = (ksock_tcp_t*)arg; (void)e;
    if (s) { s->err = 1; s->pcb = 0; }   /* pcb already freed by lwIP */
}

ksock_tcp_t* ksock_tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms) {
    ip_addr_t dst; struct tcp_pcb* pcb; ksock_tcp_t* s;
    uint32_t start = (uint32_t)pit_get_uptime_ms();
    IP_ADDR4(&dst, ip[0], ip[1], ip[2], ip[3]);
    pcb = tcp_new();
    if (!pcb) return 0;
    s = alloc_sock(pcb);
    if (!s) { tcp_abort(pcb); return 0; }
    if (tcp_connect(pcb, &dst, port, on_connected) != ERR_OK) { ksock_tcp_close(s); return 0; }
    while (!s->connected && !s->err) {
        if (timeout_ms && (uint32_t)pit_get_uptime_ms() - start >= timeout_ms) { ksock_tcp_close(s); return 0; }
        net_service_tick();
        task_yield();
    }
    if (s->err) { ksock_tcp_close(s); return 0; }
    return s;
}

int ksock_tcp_readable(ksock_tcp_t* s) { return s && (s->rx_len > 0 || s->peer_closed); }
int ksock_tcp_writable(ksock_tcp_t* s) { return s && s->pcb && tcp_sndbuf(s->pcb) > 0; }

int ksock_tcp_accept_ready(ksock_tcp_t* lst) {
    return lst && lst->is_listener && (lst->acc_tail != lst->acc_head);
}

void ksock_tcp_set_nodelay(ksock_tcp_t* s, int on) {
    if (!s || !s->pcb) return;
    if (on) tcp_nagle_disable(s->pcb); else tcp_nagle_enable(s->pcb);
}

int ksock_tcp_send(ksock_tcp_t* s, const void* buf, uint16_t len, uint32_t timeout_ms, int nonblock) {
    uint32_t start = (uint32_t)pit_get_uptime_ms();
    uint16_t sent = 0;
    const uint8_t* p = (const uint8_t*)buf;
    if (!s || !s->pcb) return -1;
    while (sent < len) {
        if (s->err) return -1;
        u16_t space = tcp_sndbuf(s->pcb);
        if (space == 0) {
            if (nonblock) return sent > 0 ? (int)sent : KSOCK_TCP_WOULDBLOCK;
            if (timeout_ms && (uint32_t)pit_get_uptime_ms() - start >= timeout_ms) return sent > 0 ? (int)sent : KSOCK_TCP_WOULDBLOCK;
            net_service_tick(); task_yield(); continue;
        }
        {
            u16_t chunk = (uint16_t)((len - sent) < space ? (len - sent) : space);
            err_t e = tcp_write(s->pcb, p + sent, chunk, TCP_WRITE_FLAG_COPY);
            if (e == ERR_MEM) { net_service_tick(); task_yield(); continue; }
            if (e != ERR_OK) return -1;
            sent += chunk;
            tcp_output(s->pcb);
        }
    }
    return (int)sent;
}

int ksock_tcp_recv(ksock_tcp_t* s, void* buf, uint16_t cap, uint16_t* out_len,
                   uint32_t timeout_ms, int nonblock) {
    uint32_t start = (uint32_t)pit_get_uptime_ms();
    if (!s) return -1;
    for (;;) {
        if (s->rx_len > 0) {
            uint16_t n = s->rx_len < cap ? (uint16_t)s->rx_len : cap;
            uint16_t off = 0;
            while (off < n) {
                uint16_t chunk = (uint16_t)(TCP_RXBUF - s->rx_head);
                if (chunk > (uint16_t)(n - off)) chunk = (uint16_t)(n - off);
                memcpy((uint8_t*)buf + off, s->rx + s->rx_head, chunk);
                s->rx_head = (uint16_t)((s->rx_head + chunk) % TCP_RXBUF);
                off += chunk;
            }
            s->rx_len -= n;
            if (out_len) *out_len = n;
            return (int)n;
        }
        if (s->peer_closed) { if (out_len) *out_len = 0; return 0; }
        if (s->err) return -1;
        if (nonblock) return KSOCK_TCP_WOULDBLOCK;
        if (timeout_ms && (uint32_t)pit_get_uptime_ms() - start >= timeout_ms) { if (out_len) *out_len = 0; return KSOCK_TCP_WOULDBLOCK; }
        net_service_tick(); task_yield();
    }
}

static err_t on_accept(void* arg, struct tcp_pcb* newpcb, err_t e) {
    ksock_tcp_t* lst = (ksock_tcp_t*)arg;
    uint32_t next;
    if (e != ERR_OK || !newpcb) return ERR_VAL;
    next = (lst->acc_head + 1u) % ACCEPT_Q;
    /* Per tcp.h accept_fn doc + tcp_in.c: "Only return ERR_ABRT if you have
     * called tcp_abort from within the callback function!" tcp_in.c's
     * caller does `if (err != ERR_ABRT) tcp_abort(pcb);` — so returning
     * ERR_ABRT here without aborting ourselves would tell lwIP we already
     * handled it, leaking/orphaning the established pcb. Return ERR_MEM
     * instead so lwIP calls tcp_abort(newpcb) for us and RSTs the peer. */
    if (next == lst->acc_tail) return ERR_MEM;      /* backlog full */
    {
        ksock_tcp_t* s = alloc_sock(newpcb);
        if (!s) return ERR_MEM;
        s->connected = 1;
        lst->acc_q[lst->acc_head] = s;
        lst->acc_head = next;
    }
    return ERR_OK;
}

ksock_tcp_t* ksock_tcp_listen(uint16_t port, int backlog) {
    struct tcp_pcb* pcb = tcp_new();
    struct tcp_pcb* bound_pcb;
    ksock_tcp_t* s;
    if (!pcb) return 0;
    if (tcp_bind(pcb, IP_ANY_TYPE, port) != ERR_OK) { tcp_abort(pcb); return 0; }
    /* tcp_listen_with_backlog() frees the original pcb and returns a new one
     * on success, but on failure (NULL) it does NOT free the original pcb -
     * capture it beforehand so we can abort it ourselves. */
    bound_pcb = pcb;
    pcb = tcp_listen_with_backlog(pcb, (u8_t)(backlog > 0 ? backlog : 1));
    if (!pcb) { tcp_abort(bound_pcb); return 0; }
    s = (ksock_tcp_t*)kmalloc(sizeof(*s));
    if (!s) { tcp_abort(pcb); return 0; }
    memset(s, 0, sizeof(*s));
    s->pcb = pcb; s->is_listener = 1;
    tcp_arg(pcb, s);
    tcp_accept(pcb, on_accept);
    return s;
}

ksock_tcp_t* ksock_tcp_accept(ksock_tcp_t* lst, uint32_t timeout_ms, int nonblock) {
    uint32_t start = (uint32_t)pit_get_uptime_ms();
    if (!lst || !lst->is_listener) return 0;
    for (;;) {
        if (lst->acc_tail != lst->acc_head) {
            ksock_tcp_t* s = lst->acc_q[lst->acc_tail];
            lst->acc_tail = (lst->acc_tail + 1u) % ACCEPT_Q;
            return s;
        }
        if (nonblock) return 0;
        if (timeout_ms && (uint32_t)pit_get_uptime_ms() - start >= timeout_ms) return 0;
        net_service_tick(); task_yield();
    }
}

int ksock_tcp_peer(ksock_tcp_t* s, uint8_t out_ip[4], uint16_t* out_port) {
    if (!s || !s->pcb) return -1;
    {
        uint32_t v = ip4_addr_get_u32(ip_2_ip4(&s->pcb->remote_ip));
        out_ip[0]=(uint8_t)v; out_ip[1]=(uint8_t)(v>>8); out_ip[2]=(uint8_t)(v>>16); out_ip[3]=(uint8_t)(v>>24);
        if (out_port) *out_port = s->pcb->remote_port;
    }
    return 0;
}

void ksock_tcp_close(ksock_tcp_t* s) {
    if (!s) return;
    if (s->is_listener) {
        /* Drain any established-but-never-accepted sockets sitting in the
         * accept queue: they are already alloc_sock'd with live pcbs, so
         * closing the listener without draining them would orphan those
         * pcbs. Each queue slot holds a distinct ksock_tcp_t with its own
         * pcb (never the listener's pcb and never aliased with another
         * queued entry), so recursing into ksock_tcp_close() per entry is
         * safe and cannot double-free or touch a pcb freed elsewhere. */
        while (s->acc_tail != s->acc_head) {
            ksock_tcp_t* q = s->acc_q[s->acc_tail];
            s->acc_tail = (s->acc_tail + 1u) % ACCEPT_Q;
            ksock_tcp_close(q);
        }
    }
    if (s->pcb) {
        tcp_arg(s->pcb, NULL);
        tcp_recv(s->pcb, NULL); tcp_sent(s->pcb, NULL); tcp_err(s->pcb, NULL);
        if (tcp_close(s->pcb) != ERR_OK) { tcp_abort(s->pcb); }
    }
    kfree(s);
}
