/* nbt_carplay host test: the NCM link (NTB16) and lwIP. Two processes, each with its own lwIP, joined by two pipes that carries NTB16 blocks in random pieces (as USB transfers may cut them): the "car" (as nbt_carplay) and
 * the "phone", which finds the car by its link-local address (neighbour discovery), sends 2 MB over TCP and gets them
 * back, and a UDP datagram.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include "lwip/sockets.h"
#include "cp.h"

struct cp_options g_opt;
volatile int g_stop;
void cp_reached(int step) { (void)step; }

static int g_fd, g_rd;

static int send_block(void* ctx, const unsigned char* b, unsigned n)
{
    (void)ctx;
    while (n) {   /* in random pieces */
        unsigned k = 1 + (unsigned)rand() % n;
        if (write(g_fd, b, k) != (ssize_t)k)
            return -1;
        b += k;
        n -= k;
    }
    return 0;
}

static void* rx(void* a)
{
    unsigned char b[3000];
    (void)a;
    for (;;) {
        const ssize_t n = read(g_rd, b, 1 + (unsigned)rand() % sizeof(b));
        if (n <= 0)
            return 0;
        ncm_input(b, (unsigned)n);
    }
}

static void up(const unsigned char mac[6])
{
    pthread_t t;
    net_init();
    ncm_start(mac, send_block, 0);
    pthread_create(&t, 0, rx, 0);
}

static const unsigned char CAR[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55}, PHONE[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};

static int car(void)
{
    unsigned short port;
    int l, c, u, total = 0;
    unsigned char b[8192];
    struct net_addr from;
    up(CAR);
    l = net_tcp_listen(7000, &port);
    u = net_udp(7001, 0);
    c = net_accept(l, 0, 20000);
    if (c < 0)
        return say("car: no connection"), 1;
    for (;;) {
        const int n = net_recv(c, b, sizeof(b), 10000);
        if (n <= 0)
            break;
        net_send(c, b, n);
        total += n;
    }
    say("car: echoed %d bytes", total);
    {
        const int n = net_recvfrom(u, b, sizeof(b), 10000, &from);
        if (n > 0)
            net_sendto(u, b, n, &from);
    }
    sleep(1);
    return 0;
}

static int phone(void)
{
    struct sockaddr_in6 a;
    int s, sent = 0, got = 0, i;
    static unsigned char out[2 << 20], in[2 << 20];
    up(PHONE);
    memset(&a, 0, sizeof(a));
    a.sin6_len = sizeof(a);
    a.sin6_family = AF_INET6;
    a.sin6_port = lwip_htons(7000);
    /* fe80::0011:22ff:fe33:4455 from the car's MAC (EUI-64, universal/local bit flipped) */
    {
        unsigned char ip[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x02 ^ 0x02, 0x11, 0x22, 0xff, 0xfe, 0x33, 0x44, 0x55};
        memcpy(&a.sin6_addr, ip, 16);
    }
    sleep(1);
    s = lwip_socket(AF_INET6, SOCK_STREAM, 0);
    if (lwip_connect(s, (struct sockaddr*)&a, sizeof(a)) < 0)
        return say("phone: connect failed"), 1;
    say("phone: connected to the car");
    for (i = 0; i < (int)sizeof(out); ++i)
        out[i] = (unsigned char)rand();
    while (sent < (int)sizeof(out) || got < (int)sizeof(out)) {
        if (sent < (int)sizeof(out)) {
            const int k = (int)sizeof(out) - sent < 30000 ? (int)sizeof(out) - sent : 30000;
            const int n = lwip_send(s, out + sent, (size_t)k, MSG_DONTWAIT);
            if (n > 0)
                sent += n;
        }
        {
            const int n = lwip_recv(s, in + got, sizeof(in) - (size_t)got, MSG_DONTWAIT);
            if (n > 0)
                got += n;
            else
                usleep(1000);
        }
    }
    lwip_close(s);
    if (memcmp(in, out, sizeof(out)))
        return say("phone: echo differs"), 1;
    say("phone: 2 MB echoed over TCP, identical");
    {
        int u = lwip_socket(AF_INET6, SOCK_DGRAM, 0);
        char b[64];
        struct timeval tv = {5, 0};
        a.sin6_port = lwip_htons(7001);
        lwip_setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        lwip_sendto(u, "datagram", 8, 0, (struct sockaddr*)&a, sizeof(a));
        if (lwip_recv(u, b, sizeof(b), 0) != 8 || memcmp(b, "datagram", 8))
            return say("phone: no UDP echo"), 1;
        say("phone: UDP echoed");
    }
    return 0;
}

int main(void)
{
    int ab[2], ba[2], st = 1;
    pid_t pid;
    setvbuf(stdout, 0, _IOLBF, 0);
    cp_random_init();
    if (pipe(ab) || pipe(ba))   /* car -> phone, phone -> car */
        return 1;
    pid = fork();
    if (!pid) {
        g_fd = ba[1];
        g_rd = ab[0];
        _exit(phone());
    }
    g_fd = ab[1];
    g_rd = ba[0];
    alarm(60);
    {
        const int c = car();
        waitpid(pid, &st, 0);
        printf("%s\n", !c && WIFEXITED(st) && !WEXITSTATUS(st) ? "NCM TEST OK" : "NCM TEST FAILED");
        return !(!c && WIFEXITED(st) && !WEXITSTATUS(st));
    }
}
