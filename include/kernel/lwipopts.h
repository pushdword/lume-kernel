#ifndef LUME_LWIP_OPTIONS_H
#define LUME_LWIP_OPTIONS_H
#include "lume/net_limits.h"
#define NO_SYS 1
#define LWIP_NO_CTYPE_H 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 1
#define LWIP_DHCP 1
#define LWIP_DNS 1
#define LWIP_UDP 1
#define LWIP_TCP 1
#define LWIP_STATS 0
#define LWIP_DEBUG 0
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETIF_LOOPBACK 1
#define LWIP_HAVE_LOOPIF 1
#define LWIP_NETIF_LOOPBACK_MULTITHREADING 0
#define LWIP_LOOPBACK_MAX_PBUFS 64
#define LWIP_HOOK_FILENAME "lume/lwip_hooks.h"
#define LWIP_HOOK_IP4_ROUTE_SRC(source, destination) lume_ip4_route_src(source, destination)
#define MEM_ALIGNMENT 8
#define MEM_SIZE LUME_NET_HEAP_BYTES
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) ||              \
    defined(LUME_ARCH_I686)
extern void *lume_net_heap;
#define LWIP_RAM_HEAP_POINTER lume_net_heap
#endif
#define PBUF_POOL_SIZE LUME_NET_PBUF_COUNT
#define PBUF_POOL_BUFSIZE 1536
#define MEMP_NUM_TCP_PCB LUME_NET_TCP_PCBS
#define MEMP_NUM_TCP_PCB_LISTEN LUME_NET_TCP_LISTENERS
#define MEMP_NUM_TCP_SEG LUME_NET_TCP_SEGMENTS
#define SO_REUSE 1
#define LWIP_TCP_KEEPALIVE 1
#define MEMP_NUM_UDP_PCB LUME_NET_UDP_PCBS
#define MEMP_NUM_RAW_PCB 2
#define TCP_MSS 1460
#define LWIP_WND_SCALE LUME_NET_WINDOW_SCALE
#define TCP_RCV_SCALE LUME_NET_RECEIVE_SCALE
#define TCP_WND LUME_NET_TCP_WINDOW
#define TCP_SND_BUF LUME_NET_TCP_SEND_BYTES
#define TCP_SNDLOWAT (4 * TCP_MSS)
#define DNS_TABLE_SIZE 4
#define DNS_MAX_NAME_LENGTH 128
#define IP_REASSEMBLY 0
#define IP_FRAG 0
#define LWIP_DHCP_CHECK_LINK_UP 1
#define DHCP_DOES_ARP_CHECK 1
#endif
