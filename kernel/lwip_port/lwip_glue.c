/*
 * lwip_glue.c — lwIP platform hooks for DeanOS
 *
 * Provides:
 *   sys_now()             — milliseconds since boot (for lwIP timeouts)
 *   lwip_port_diag()      — diagnostic printf → TTY via libc vprintf
 *   lwip_port_assert_fail() — log assert, then halt
 *   net_lwip_core_init()  — call lwip_init() to bring up the core stack
 *
 * Real kernel symbols used:
 *   pit_get_uptime_ms()   — uint64_t uptime counter (kernel/pit.h)
 *   klog()                — string → kernel log ring + TTY (kernel/log.h)
 *   vprintf() / printf()  — formatted output → TTY (libc stdio.h)
 */

#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "arch/cc.h"        /* found via -Ikernel/lwip_port */
#include "kernel/pit.h"     /* found via -Ikernel/include  */
#include "kernel/log.h"     /* found via -Ikernel/include  */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

/* sys_now(): return milliseconds since boot.
 * pit_get_uptime_ms() returns uint64_t; truncating to u32_t is correct —
 * lwIP only needs relative differences (wraps every ~49 days). */
u32_t sys_now(void) {
    return (u32_t)pit_get_uptime_ms();
}

/* lwip_port_diag(): route LWIP_PLATFORM_DIAG output to TTY.
 * klog_vprintf does not exist; vprintf (libc) writes formatted text to TTY. */
void lwip_port_diag(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* lwip_port_assert_fail(): log assertion and halt the CPU.
 * No panic() exists in this kernel; use the inline hlt-loop pattern. */
void lwip_port_assert_fail(const char* msg, const char* file, int line) {
    klog("lwIP assert failed");
    klog(msg);
    printf("lwIP assert: %s at %s:%d\n", msg, file, line);
    while (1) {
        __asm__ __volatile__("hlt");
    }
}

/* net_lwip_core_init(): initialise the lwIP core (no netif yet — Task 4). */
void net_lwip_core_init(void) {
    lwip_init();
}

/* lwip_port_rand(): simple LCG pseudo-random for DNS txids etc.
 * Declared in lwipopts.h so LWIP_RAND() expands to this.
 * Returns unsigned int (== u32_t on i686). */
static unsigned int lwip_rand_state = 0xDEAD1234U;
unsigned int lwip_port_rand(void) {
    lwip_rand_state = lwip_rand_state * 1664525U + 1013904223U;
    return lwip_rand_state;
}

/* ---- Task 5: bring-up, tick, accessors ---- */

#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/ip_addr.h"
#include "deanos_netif.h"
#include "kernel/net_lwip.h"

static int g_started = 0;

/* Format an unsigned decimal number into buf (no null terminator written).
 * Returns pointer one past the last digit written. */
static char* fmt_uint(char* buf, unsigned v) {
    if (v >= 100u) { *buf++ = (char)('0' + v / 100u); v %= 100u; *buf++ = (char)('0' + v / 10u); }
    else if (v >= 10u) { *buf++ = (char)('0' + v / 10u); }
    *buf++ = (char)('0' + v % 10u);
    return buf;
}

/* Logs the bound IPv4 to serial (klog mirrors to COM1) so headless QEMU
   verification has an observable "DHCP got a lease" signal.
   No snprintf exists in this kernel, so we format the four octets manually. */
static void netif_status_cb(struct netif* netif) {
    uint32_t v = ip4_addr_get_u32(netif_ip4_addr(netif));
    if (v == 0u) return;  /* still 0.0.0.0: not bound yet */
    /* Build "lwip: bound IP a.b.c.d" in a fixed buffer */
    char line[64];
    const char prefix[] = "lwip: bound IP ";
    char* p = line;
    const char* s = prefix;
    while (*s) *p++ = *s++;
    p = fmt_uint(p, (unsigned)(v & 0xffu));
    *p++ = '.';
    p = fmt_uint(p, (unsigned)((v >> 8) & 0xffu));
    *p++ = '.';
    p = fmt_uint(p, (unsigned)((v >> 16) & 0xffu));
    *p++ = '.';
    p = fmt_uint(p, (unsigned)((v >> 24) & 0xffu));
    *p = '\0';
}

int net_lwip_start(void) {
    ip4_addr_t any; ip4_addr_set_zero(&any);
    lwip_init();
    if (deanos_netif_bind_driver() != 0) return -1;
    netif_add(deanos_netif_default(), &any, &any, &any, NULL,
              deanos_netif_init, netif_input);
    netif_set_default(deanos_netif_default());
    netif_set_status_callback(deanos_netif_default(), netif_status_cb);
    netif_set_up(deanos_netif_default());
    netif_set_link_up(deanos_netif_default());
    dhcp_start(deanos_netif_default());
    g_started = 1;
    return 0;
}

/*
 * Local IRQ save/restore via EFLAGS (pushf/popf).
 * The kernel exposes only bare interrupts_enable()/interrupts_disable()
 * (idt.h); those are unsafe here because an unconditional sti on return
 * from an IRQ-context call would re-enable interrupts prematurely.
 * A save/restore pair preserves the IF bit exactly as the caller left it.
 */
static inline uint32_t local_irq_save(void) {
    uint32_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void local_irq_restore(uint32_t flags) {
    __asm__ volatile("push %0; popf" :: "r"(flags) : "memory");
}

/* net_lock()/net_unlock(): shared "big net lock" API — see kernel/net_lwip.h
 * for the full rationale. These simply expose the local IRQ save/restore
 * pair above so other translation units (ksock_tcp.c, ksock_udp.c,
 * ksock_dns.c) can serialize their direct lwIP raw-API calls against the
 * PIT-driven pump below. pushf/popf save the exact IF bit, so nested
 * net_lock()/net_unlock() pairs (e.g. a callback invoked from inside the
 * locked pump calling a helper that itself takes the lock) compose safely:
 * each nested acquire just re-saves IF==0 and the matching release restores
 * that same state. */
uint32_t net_lock(void) { return local_irq_save(); }
void net_unlock(uint32_t flags) { local_irq_restore(flags); }

void net_service_tick(void) {
    uint32_t flags;
    if (!g_started) return;
    /*
     * net_service_tick() is called from BOTH task context (sys_poll,
     * kernel/syscall.c, ksock_* blocking loops — interrupts enabled) AND IRQ
     * context (scheduler_tick, kernel/task.c, PIT IRQ 0, interrupts already
     * disabled). lwIP's raw API is non-reentrant — concurrent entry
     * corrupts pcb lists, the timeout list, and memp/mem pools. Running the
     * ENTIRE body under net_lock() makes this a complete critical section on
     * this uniprocessor kernel: the PIT cannot fire mid-pump (no IRQ = no
     * reentrant pump call) and no task switch can occur mid-pump either, so
     * the previous in_tick test-and-set flag is no longer needed — it only
     * ever protected the pump against itself, and full-body IRQ-off now
     * does that plus far more (it also serializes against every direct
     * task-context raw-API caller, which in_tick never covered).
     *
     * The IRQ-side call is intentionally kept: the shell busy-spins (hlt+nop
     * while READY) so the idle thread never runs, making the IRQ pump the
     * only thing that drives DHCP/ARP/TCP timeouts in the background.
     */
    flags = net_lock();
    net_lwip_rx_pump();
    sys_check_timeouts();
    net_unlock(flags);
}

int net_lwip_is_ready(void) {
    return g_started && netif_is_up(deanos_netif_default());
}
const char* net_lwip_driver_name(void) { return deanos_netif_driver_name(); }

static void copy_ip4(uint8_t out[4], const ip4_addr_t* a) {
    uint32_t v = ip4_addr_get_u32(a);
    out[0]=(uint8_t)(v); out[1]=(uint8_t)(v>>8); out[2]=(uint8_t)(v>>16); out[3]=(uint8_t)(v>>24);
}
void net_lwip_get_ipv4(uint8_t o[4])         { copy_ip4(o, netif_ip4_addr(deanos_netif_default())); }
void net_lwip_get_ipv4_netmask(uint8_t o[4]) { copy_ip4(o, netif_ip4_netmask(deanos_netif_default())); }
void net_lwip_get_ipv4_gateway(uint8_t o[4]) { copy_ip4(o, netif_ip4_gw(deanos_netif_default())); }
void net_lwip_get_mac(uint8_t o[6])          { memcpy(o, deanos_netif_default()->hwaddr, 6); }

/* ---- Task 10: minimal blocking ICMP echo (ping) via lwIP raw API ---- */

#include "lwip/raw.h"
#include "lwip/icmp.h"
#include "lwip/prot/icmp.h"
#include "lwip/inet_chksum.h"
#include "kernel/task.h"

/* Shared between raw_sendto() (this stack frame) and the raw_recv callback
 * (invoked synchronously from net_lwip_rx_pump() -> netif_input() while we
 * spin below) — no locking needed since both run on the same task, never
 * concurrently with each other. */
typedef struct {
    volatile int done;
    volatile int ok;
    uint16_t id;    /* network byte order, matches what we put on the wire */
    uint16_t seq;   /* network byte order */
} ping_wait_t;

static u8_t ping_recv_cb(void* arg, struct raw_pcb* pcb, struct pbuf* p, const ip_addr_t* addr) {
    ping_wait_t* w = (ping_wait_t*)arg;
    (void)pcb; (void)addr;
    if (!p) return 0;
    {
        /* Raw IPv4 pcbs receive the pbuf with the IP header still attached
         * (ip4_input() calls raw_input() before pbuf_remove_header()) —
         * skip past it via the current-packet's header length rather than
         * assuming a fixed 20 bytes (IP options are legal, if rare). */
        u16_t iphdr_len = ip_current_header_tot_len();
        struct icmp_echo_hdr hdr;
        if (p->tot_len >= (u16_t)(iphdr_len + sizeof(struct icmp_echo_hdr)) &&
            pbuf_copy_partial(p, &hdr, sizeof(hdr), iphdr_len) == sizeof(hdr) &&
            hdr.type == ICMP_ER && hdr.id == w->id && hdr.seqno == w->seq) {
            w->ok = 1;
            w->done = 1;
            pbuf_free(p);
            return 1;   /* consumed */
        }
    }
    return 0;   /* not ours; let lwIP keep looking / deliver elsewhere */
}

int net_lwip_ping(const uint8_t ip[4], uint16_t seq, uint32_t timeout_ms) {
    static uint16_t ping_id_ctr = 0xDE00;
    struct raw_pcb* pcb;
    struct pbuf* p;
    struct icmp_echo_hdr* echo;
    ip_addr_t dst;
    ping_wait_t w;
    uint32_t start;
    uint32_t f;
    int rc;

    if (!net_lwip_is_ready()) return -1;

    f = net_lock();
    pcb = raw_new(IP_PROTO_ICMP);
    if (!pcb) { net_unlock(f); return -1; }
    if (raw_bind(pcb, IP_ADDR_ANY) != ERR_OK) { raw_remove(pcb); net_unlock(f); return -1; }
    net_unlock(f);

    IP_ADDR4(&dst, ip[0], ip[1], ip[2], ip[3]);

    memset(&w, 0, sizeof(w));
    w.id  = lwip_htons(ping_id_ctr++);
    w.seq = lwip_htons(seq);

    f = net_lock();
    raw_recv(pcb, ping_recv_cb, &w);
    net_unlock(f);

    /* pbuf_alloc/raw_sendto/pbuf_free all touch lwIP's shared pbuf pool and
     * pcb state — same critical section, no yield inside it. */
    f = net_lock();
    p = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr), PBUF_RAM);
    if (!p) { raw_remove(pcb); net_unlock(f); return -1; }
    echo = (struct icmp_echo_hdr*)p->payload;
    memset(echo, 0, sizeof(*echo));
    echo->type   = ICMP_ECHO;
    echo->code   = 0;
    echo->id     = w.id;
    echo->seqno  = w.seq;
    echo->chksum = 0;
    echo->chksum = inet_chksum(echo, sizeof(*echo));

    if (raw_sendto(pcb, p, &dst) != ERR_OK) {
        pbuf_free(p);
        raw_remove(pcb);
        net_unlock(f);
        return -1;
    }
    pbuf_free(p);
    net_unlock(f);

    start = (uint32_t)pit_get_uptime_ms();
    for (;;) {
        int done;
        f = net_lock();
        done = w.done;
        net_unlock(f);
        if (done) break;
        if (timeout_ms && (uint32_t)pit_get_uptime_ms() - start >= timeout_ms) break;
        net_service_tick();
        task_yield();
    }

    f = net_lock();
    rc = w.ok ? 0 : -1;
    raw_remove(pcb);
    net_unlock(f);
    return rc;
}
