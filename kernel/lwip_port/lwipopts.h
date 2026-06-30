#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

/* ---- Core mode: bare-metal, single-threaded raw API ---- */
#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_NETCONN                0   /* no sequential API */
#define LWIP_SOCKET                 0   /* BSD layer implemented by syscall.c */
#define LWIP_NETIF_API              0

/* ---- Memory: static lwIP heap, isolated from kernel kheap ---- */
#define MEM_LIBC_MALLOC             0
#define MEMP_MEM_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (64 * 1024)

#define MEMP_NUM_PBUF               32
#define MEMP_NUM_UDP_PCB            8
#define MEMP_NUM_TCP_PCB            12
#define MEMP_NUM_TCP_PCB_LISTEN     4
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          16
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 4)
#define PBUF_POOL_SIZE              48
#define PBUF_POOL_BUFSIZE           1536

/* ---- Protocols ---- */
#define LWIP_ARP                    1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   1
#define LWIP_DNS                    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1

/* DHCP needs ARP check disabled to be simple; allow broadcast. */
#define LWIP_DHCP_DOES_ACD_CHECK    0

/* ---- TCP tuning (small footprint) ---- */
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * TCP_SND_BUF) / TCP_MSS)
#define LWIP_TCP_KEEPALIVE          1

/* ---- Checksums computed in software (no NIC offload) ---- */
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_GEN_ICMP           1
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_TCP          1
#define CHECKSUM_CHECK_ICMP         1

/* ---- Stats / debug (off by default; flip on locally when needed) ---- */
#define LWIP_STATS                  0
#define LWIP_DEBUG                  0

/* No OS threads to defer into. */
#define LWIP_TCPIP_CORE_LOCKING     0

#endif /* LWIP_LWIPOPTS_H */
