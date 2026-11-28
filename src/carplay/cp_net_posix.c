/* nbt_carplay host tests: the cp_net interface on the host's own sockets (IPv4 loopback), so the AirPlay part can be
 * tested on a PC against a test client (scripts/carplay_test.py). Not part of the car's program.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "cp.h"

static void timeout(int s, int ms)
{
    struct timeval tv;
    if (ms < 0)
        ms = 0;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static void from_sockaddr(const struct sockaddr_in* a, struct net_addr* out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->ip, &a->sin_addr, 4);
    out->v4 = 1;
    out->port = ntohs(a->sin_port);
}

int net_init(void)
{
    return 0;
}

static int bind_any(int s, unsigned short port, unsigned short* bound)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    const int one = 1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) < 0)
        return -1;
    if (bound) {
        if (getsockname(s, (struct sockaddr*)&a, &n) < 0)
            return -1;
        *bound = ntohs(a.sin_port);
    }
    return 0;
}

int net_tcp_listen(unsigned short port, unsigned short* bound)
{
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    if (bind_any(s, port, bound) || listen(s, 4) < 0) {
        close(s);
        return -1;
    }
    return s;
}

int net_accept(int s, struct net_addr* peer, int timeout_ms)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    int c;
    const int one = 1;
    timeout(s, timeout_ms);
    c = accept(s, (struct sockaddr*)&a, &n);
    if (c < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? -2 : -1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (peer)
        from_sockaddr(&a, peer);
    return c;
}

int net_recv(int s, void* b, int n, int timeout_ms)
{
    int r;
    timeout(s, timeout_ms);
    r = (int)recv(s, b, (size_t)n, 0);
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
        const int r = (int)send(s, p, (size_t)n, MSG_NOSIGNAL);
        if (r <= 0)
            return -1;
        p += r;
        n -= r;
    }
    return 0;
}

int net_udp(unsigned short port, unsigned short* bound)
{
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;
    if (bind_any(s, port, bound)) {
        close(s);
        return -1;
    }
    return s;
}

int net_recvfrom(int s, void* b, int n, int timeout_ms, struct net_addr* from)
{
    struct sockaddr_in a;
    socklen_t k = sizeof(a);
    int r;
    timeout(s, timeout_ms);
    r = (int)recvfrom(s, b, (size_t)n, 0, (struct sockaddr*)&a, &k);
    if (r < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? -2 : -1;
    if (from)
        from_sockaddr(&a, from);
    return r;
}

int net_sendto(int s, const void* b, int n, const struct net_addr* to)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(to->port);
    memcpy(&a.sin_addr, to->ip, 4);
    return sendto(s, b, (size_t)n, 0, (struct sockaddr*)&a, sizeof(a)) == n ? 0 : -1;
}

int net_peer(int s, struct net_addr* out)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    if (getpeername(s, (struct sockaddr*)&a, &n) < 0)
        return -1;
    from_sockaddr(&a, out);
    return 0;
}

void net_close(int s)
{
    if (s < 0)
        return;
    shutdown(s, SHUT_RDWR);
    close(s);
}

const char* net_text(const struct net_addr* a, char* buf, unsigned size)
{
    char ip[64];
    inet_ntop(AF_INET, a->ip, ip, sizeof(ip));
    snprintf(buf, size, "%s:%u", ip, a->port);
    return buf;
}
