/* lwIP options for nbt_carplay: IPv6 only, one Ethernet interface (the iPhone's NCM link), sockets from several threads.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 */
#ifndef NBT_CARPLAY_LWIPOPTS_H
#define NBT_CARPLAY_LWIPOPTS_H

#define NO_SYS 0
#define SYS_LIGHTWEIGHT_PROT 1
#define LWIP_TCPIP_CORE_LOCKING 1
#define LWIP_SINGLE_NETIF 1

/* memory from the C library (no fixed pools to size) */
#define MEM_LIBC_MALLOC 1
#define MEMP_MEM_MALLOC 1
#define MEM_ALIGNMENT 4

#define LWIP_IPV4 0
#define LWIP_IPV6 1
#define LWIP_ARP 0
#define LWIP_ETHERNET 1
#define LWIP_ICMP6 1
#define LWIP_IPV6_MLD 1
#define LWIP_IPV6_AUTOCONFIG 0
#define LWIP_IPV6_DHCP6 0
#define LWIP_IPV6_SEND_ROUTER_SOLICIT 0
#define LWIP_IPV6_DUP_DETECT_ATTEMPTS 0
#define LWIP_ND6_ALLOW_RA_UPDATES 0
#define LWIP_IPV6_FRAG 1
#define LWIP_IPV6_REASS 1
#define IPV6_FRAG_COPYHEADER 1
#define LWIP_IPV6_NUM_ADDRESSES 2
#define LWIP_DNS 0
#define LWIP_DHCP 0
#define LWIP_AUTOIP 0
#define LWIP_IGMP 0
#define LWIP_RAW 0
#define LWIP_UDP 1
#define LWIP_TCP 1
#define LWIP_NETIF_LOOPBACK 0
#define LWIP_HAVE_LOOPIF 0
#define LWIP_STATS 0

/* TCP: one fast link (USB), video in one direction */
#define TCP_MSS 1440
#define TCP_WND 65535
#define TCP_SND_BUF 65535
#define TCP_SND_QUEUELEN 128
#define TCP_QUEUE_OOSEQ 1
#define LWIP_TCP_KEEPALIVE 1
#define MEMP_NUM_TCP_PCB 16
#define MEMP_NUM_TCP_PCB_LISTEN 8
#define MEMP_NUM_UDP_PCB 16
#define MEMP_NUM_NETCONN 32
#define PBUF_POOL_SIZE 256
#define PBUF_POOL_BUFSIZE 1536

/* threads and sockets */
#define TCPIP_THREAD_NAME "lwip"
#define TCPIP_THREAD_STACKSIZE 65536
#define TCPIP_THREAD_PRIO 0
#define TCPIP_MBOX_SIZE 256
#define DEFAULT_THREAD_STACKSIZE 65536
#define DEFAULT_RAW_RECVMBOX_SIZE 32
#define DEFAULT_UDP_RECVMBOX_SIZE 128
#define DEFAULT_TCP_RECVMBOX_SIZE 256
#define DEFAULT_ACCEPTMBOX_SIZE 8
#define LWIP_NETCONN 1
#define LWIP_SOCKET 1
#define LWIP_COMPAT_SOCKETS 0
#define LWIP_POSIX_SOCKETS_IO_NAMES 0
#define LWIP_SOCKET_SELECT 0
#define LWIP_SOCKET_POLL 0
#define LWIP_SO_RCVTIMEO 1
#define LWIP_SO_SNDTIMEO 1
#define LWIP_SO_RCVBUF 0
#define SO_REUSE 1
#define LWIP_NETCONN_SEM_PER_THREAD 1
#define LWIP_NETCONN_FULLDUPLEX 1
#define LWIP_TCP_TIMESTAMPS 0

#define LWIP_CHECKSUM_ON_COPY 0
#define LWIP_NETIF_STATUS_CALLBACK 0
#define LWIP_NETIF_LINK_CALLBACK 0


#endif
