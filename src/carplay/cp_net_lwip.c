/* nbt_carplay: sockets on the iPhone's NCM link, through lwIP (in this process; the car's own network stack is not
 * touched).
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 */
#include <string.h>
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "cp.h"

#define MAX_SOCKETS 64

static int g_timeout[MAX_SOCKETS];   /* the receive timeout set on each socket (ms + 1; 0 = none set yet) */

static void timeout(int s, int ms)
{
    struct timeval tv;
    if (ms < 0)
        ms = 0;   /* 0 = wait for ever */
    if (s < 0 || s >= MAX_SOCKETS || g_timeout[s] == ms + 1)
        return;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    g_timeout[s] = ms + 1;
}

static void from_sockaddr(const struct sockaddr_in6* a, struct net_addr* out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->ip, &a->sin6_addr, 16);
    out->port = lwip_ntohs(a->sin6_port);
}

static void to_sockaddr(const struct net_addr* a, struct sockaddr_in6* out)
{
    memset(out, 0, sizeof(*out));
    out->sin6_len = sizeof(*out);
    out->sin6_family = AF_INET6;
    out->sin6_port = lwip_htons(a->port);
    memcpy(&out->sin6_addr, a->ip, 16);
}

static void done(void* arg)
{
    *(volatile int*)arg = 1;
}

int net_init(void)
{
    static volatile int ready;
    tcpip_init(done, (void*)&ready);
    while (!ready)
        sleep_ms(5);
    return 0;
}

static int bind_any(int s, unsigned short port, unsigned short* bound)
{
    struct sockaddr_in6 a;
    socklen_t n = sizeof(a);
    const int one = 1;
    memset(&a, 0, sizeof(a));
    a.sin6_len = sizeof(a);
    a.sin6_family = AF_INET6;
    a.sin6_port = lwip_htons(port);
    lwip_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (lwip_bind(s, (struct sockaddr*)&a, sizeof(a)) < 0)
        return -1;
    if (bound) {
        if (lwip_getsockname(s, (struct sockaddr*)&a, &n) < 0)
            return -1;
        *bound = lwip_ntohs(a.sin6_port);
    }
    return 0;
}

int net_tcp_listen(unsigned short port, unsigned short* bound)
{
    const int s = lwip_socket(AF_INET6, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    if (s >= MAX_SOCKETS || bind_any(s, port, bound) || lwip_listen(s, 4) < 0) {
        lwip_close(s);
        return -1;
    }
    g_timeout[s] = 0;
    return s;
}

int net_accept(int s, struct net_addr* peer, int timeout_ms)
{
    struct sockaddr_in6 a;
    socklen_t n = sizeof(a);
    int c;
    timeout(s, timeout_ms);
    c = lwip_accept(s, (struct sockaddr*)&a, &n);
    if (c < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? -2 : -1;
    if (c >= MAX_SOCKETS) {
        lwip_close(c);
        return -1;
    }
    g_timeout[c] = 0;
    {
        const int one = 1;
        lwip_setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    if (peer)
        from_sockaddr(&a, peer);
    return c;
}

int net_recv(int s, void* b, int n, int timeout_ms)
{
    int r;
    timeout(s, timeout_ms);
    r = lwip_recv(s, b, (size_t)n, 0);
    if (r < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? -2 : -1;
    return r;
}

int net_recv_all(int s, void* b, int n, int timeout_ms)
{
    unsigned char* p = (unsigned char*)b;
    while (n > 0) {
        const int r = net_recv(s, p, n, timeout_ms);
        if (r <= 0)
            return -1;
        p += r;
        n -= r;
    }
    return 0;
}

int net_send(int s, const void* b, int n)
{
    const unsigned char* p = (const unsigned char*)b;
    while (n > 0) {
        const int r = lwip_send(s, p, (size_t)n, 0);
        if (r <= 0)
            return -1;
        p += r;
        n -= r;
    }
    return 0;
}

int net_udp(unsigned short port, unsigned short* bound)
{
    const int s = lwip_socket(AF_INET6, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;
    if (s >= MAX_SOCKETS || bind_any(s, port, bound)) {
        lwip_close(s);
        return -1;
    }
    g_timeout[s] = 0;
    return s;
}

int net_recvfrom(int s, void* b, int n, int timeout_ms, struct net_addr* from)
{
    struct sockaddr_in6 a;
    socklen_t k = sizeof(a);
    int r;
    timeout(s, timeout_ms);
    r = lwip_recvfrom(s, b, (size_t)n, 0, (struct sockaddr*)&a, &k);
    if (r < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? -2 : -1;
    if (from)
        from_sockaddr(&a, from);
    return r;
}

int net_sendto(int s, const void* b, int n, const struct net_addr* to)
{
    struct sockaddr_in6 a;
    to_sockaddr(to, &a);
    return lwip_sendto(s, b, (size_t)n, 0, (struct sockaddr*)&a, sizeof(a)) == n ? 0 : -1;
}

int net_peer(int s, struct net_addr* out)
{
    struct sockaddr_in6 a;
    socklen_t n = sizeof(a);
    if (lwip_getpeername(s, (struct sockaddr*)&a, &n) < 0)
        return -1;
    from_sockaddr(&a, out);
    return 0;
}

void net_close(int s)
{
    if (s < 0)
        return;
    lwip_shutdown(s, SHUT_RDWR);
    lwip_close(s);
}

const char* net_text(const struct net_addr* a, char* buf, unsigned size)
{
    char ip[64];
    if (!lwip_inet_ntop(AF_INET6, a->ip, ip, sizeof(ip)))
        strcpy(ip, "?");
    snprintf(buf, size, "[%s]:%u", ip, a->port);
    return buf;
}
