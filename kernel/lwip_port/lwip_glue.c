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
