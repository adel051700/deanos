#ifndef _KERNEL_NET_H
#define _KERNEL_NET_H

/*
 * Legacy return-code macros from the hand-rolled network stack.
 * The stack itself (net.c/dns.c/dhcp.c) has been deleted and replaced
 * by lwIP (see net_lwip.h), but these NET_* codes remain part of the
 * userland-visible syscall ABI: kernel/syscall.c maps ksock backend
 * results onto them.
 */

#define NET_UDP_ERR_WOULD_BLOCK     -8
#define NET_UDP_ERR_MSG_TRUNC       -9
#define NET_UDP_ERR_TX              -7

#define NET_DNS_ERR_INVALID         -1
#define NET_DNS_ERR_NOT_FOUND       -6

#define NET_TCP_ERR_WOULD_BLOCK     -8
#define NET_TCP_ERR_CLOSED          -9
#define NET_TCP_ERR_ALREADY         -10
#define NET_TCP_ERR_TIMEOUT         -5

#endif
