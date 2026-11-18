/*
 * lwIP configuration for the bsdsockets system module.
 *
 * How lwIP is used here:
 *   - NO_SYS: there is no lwIP thread and no lwIP locking. Every call into
 *     lwIP is made with the module's stack lock held (stack/bsd_stack.hpp),
 *     and LWIP_ASSERT_CORE_LOCKED checks that.
 *   - Callback ("raw") API only. The socket semantics clients see are
 *     implemented in socket/, on tcp_pcb and udp_pcb.
 *   - IPv4 only, as on the Switch.
 *
 * Anything not set here keeps the default in lwip/opt.h.
 */
#pragma once

/* ---- System ---- */
#define NO_SYS                          1
#define LWIP_SOCKET                     0
#define LWIP_NETCONN                    0
#define SYS_LIGHTWEIGHT_PROT            0
#define LWIP_TIMERS                     1
#define LWIP_ASSERT_CORE_LOCKED()       hz_lwip_assert_core_locked()

/* ---- Memory ---- */
#define MEM_ALIGNMENT                   8
/* The heap holds outgoing data: TCP send queues (a copy of what clients write) and UDP datagrams. */
#define MEM_SIZE                        (8 * 1024 * 1024)
/* Incoming frames. One pool buffer holds a full Ethernet frame. */
#define PBUF_POOL_SIZE                  2048
#define PBUF_POOL_BUFSIZE               1600
#define MEMP_NUM_PBUF                   1024
#define MEMP_NUM_UDP_PCB                128
#define MEMP_NUM_TCP_PCB                256
#define MEMP_NUM_TCP_PCB_LISTEN         64
#define MEMP_NUM_TCP_SEG                4096
#define MEMP_NUM_REASSDATA              16
#define MEMP_NUM_FRAG_PBUF              64
#define MEMP_NUM_ARP_QUEUE              64
#define MEMP_NUM_SYS_TIMEOUT            (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8)

/* ---- Link and IP ---- */
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define ARP_TABLE_SIZE                  64
#define ARP_QUEUEING                    1
#define ETHARP_SUPPORT_STATIC_ENTRIES   1
#define IP_REASSEMBLY                   1
#define IP_FRAG                         1
#define IP_REASS_MAX_PBUFS              64
#define LWIP_ICMP                       1
#define LWIP_RAW                        0
#define LWIP_IGMP                       0
#define LWIP_AUTOIP                     0

/* ---- Interfaces ---- */
#define LWIP_SINGLE_NETIF               0
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_HOSTNAME             1
/* 127.0.0.1, and delivery of packets a host sends to its own address. Queued packets are */
/* delivered by netif_poll_all() from the network thread. */
#define LWIP_HAVE_LOOPIF                1
#define LWIP_NETIF_LOOPBACK             1
#define LWIP_LOOPBACK_MAX_PBUFS         1024
#define LWIP_NETIF_LOOPBACK_MULTITHREADING 0

/* ---- DHCP and DNS ---- */
#define LWIP_DHCP                       1
#define LWIP_DHCP_DOES_ACD_CHECK        0
#define LWIP_ACD                        0
/* The resolver is not exposed yet (sfdnsres is a later step). It is enabled so that DHCP records */
/* the DNS servers it is given. */
#define LWIP_DNS                        1
#define DNS_MAX_SERVERS                 2
#define LWIP_DHCP_MAX_DNS_SERVERS       DNS_MAX_SERVERS

/* ---- UDP ---- */
#define LWIP_UDP                        1

/* ---- TCP ---- */
#define LWIP_TCP                        1
#define TCP_MSS                         1460
#define LWIP_WND_SCALE                  1
#define TCP_RCV_SCALE                   2
#define TCP_WND                         (128 * 1024)
#define TCP_SND_BUF                     (128 * 1024)
/* Only lwIP's own sockets layer reads this, but its sanity check wants it below 64 KiB. */
#define TCP_SNDLOWAT                    (8 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define TCP_QUEUE_OOSEQ                 1
#define TCP_LISTEN_BACKLOG              1
#define TCP_DEFAULT_LISTEN_BACKLOG      16
#define LWIP_TCP_KEEPALIVE              1
#define LWIP_TCP_SACK_OUT               1
#define LWIP_TCP_TIMESTAMPS             0
#define TCP_OVERSIZE                    TCP_MSS
#define SO_REUSE                        1
#define LWIP_SO_RCVBUF                  0

/* ---- Checksums: all in software; the drivers negotiate no offloads. ---- */
#define LWIP_CHECKSUM_ON_COPY           1

/* ---- Statistics and debugging ---- */
#define LWIP_STATS                      0
#define MEM_STATS                       0
#define MEMP_STATS                      0
