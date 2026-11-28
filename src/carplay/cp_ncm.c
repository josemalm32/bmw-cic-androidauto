/* nbt_carplay: the iPhone's CDC NCM network link (step a): NTB16 blocks <-> Ethernet frames <-> lwIP.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 *
 * In the CarPlay USB configuration the iPhone offers a CDC NCM function. CarPlay runs over IPv6 link-local
 * addresses on it: ours is made from the MAC address (the NCM descriptor's iMACAddress, as xcertplay uses it), and
 * we tell the iPhone that address in CarPlayStartSession. Frames travel in NTB16 transfer blocks (one Ethernet frame
 * per block from us, as xcertplay / LIVI; a block of a multiple of 512 bytes gets one pad byte so it ends with a
 * short USB packet). Incoming blocks are cut into frames and handed to lwIP. --pcap FILE writes every frame both
 * ways (Wireshark).
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "lwip/etharp.h"
#include "lwip/ethip6.h"
#include "lwip/ip6_addr.h"
#include "lwip/mld6.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "netif/ethernet.h"
#include "cp.h"

#define NTH16 0x484d434eu   /* "NCMH" */
#define NDP16 0x304d434eu   /* "NCM0" */

static struct netif g_netif;
static unsigned char g_mac[6];
static ncm_send_fn g_send;
static void* g_ctx;
static pthread_mutex_t g_tx = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_seq;
static char g_address[64];
static unsigned char g_rx[65536 + 2048];
static unsigned g_rxn;
static unsigned long long g_frames_in, g_frames_out;
static FILE* g_pcap;
static pthread_mutex_t g_pcap_m = PTHREAD_MUTEX_INITIALIZER;

void ncm_pcap(const char* path)
{
    unsigned char h[24];
    g_pcap = fopen(path, "wb");
    if (!g_pcap) {
        say("ncm: %s could not be written", path);
        return;
    }
    put32le(h, 0xa1b2c3d4u);
    put16le(h + 4, 2);
    put16le(h + 6, 4);
    put32le(h + 8, 0);
    put32le(h + 12, 0);
    put32le(h + 16, 65535);
    put32le(h + 20, 1);   /* Ethernet */
    fwrite(h, 1, sizeof(h), g_pcap);
    fflush(g_pcap);
}

static void pcap_frame(const unsigned char* f, unsigned n)
{
    unsigned char h[16];
    const long long t = ms_now();
    if (!g_pcap)
        return;
    put32le(h, (unsigned)(t / 1000));
    put32le(h + 4, (unsigned)(t % 1000) * 1000);
    put32le(h + 8, n);
    put32le(h + 12, n);
    pthread_mutex_lock(&g_pcap_m);
    fwrite(h, 1, sizeof(h), g_pcap);
    fwrite(f, 1, n, g_pcap);
    fflush(g_pcap);
    pthread_mutex_unlock(&g_pcap_m);
}

/* lwIP -> USB: one frame in one NTB16 block */
static err_t link_output(struct netif* netif, struct pbuf* p)
{
    unsigned char block[28 + 1600];
    const unsigned n = p->tot_len;
    unsigned len;
    int r;
    (void)netif;
    if (n > 1600)
        return ERR_BUF;
    pbuf_copy_partial(p, block + 28, (u16_t)n, 0);
    pcap_frame(block + 28, n);
    len = 28 + n;
    pthread_mutex_lock(&g_tx);
    put32le(block, NTH16);
    put16le(block + 4, 12);
    put16le(block + 6, g_seq++ & 0xffff);
    put16le(block + 8, len);
    put16le(block + 10, 12);
    put32le(block + 12, NDP16);
    put16le(block + 16, 16);
    put16le(block + 18, 0);
    put16le(block + 20, 28);
    put16le(block + 22, n);
    put16le(block + 24, 0);
    put16le(block + 26, 0);
    if (len % 512 == 0)
        block[len++] = 0;
    r = g_send(g_ctx, block, len);
    ++g_frames_out;
    pthread_mutex_unlock(&g_tx);
    if (g_frames_out == 1)
        say("ncm: first frame out (%u bytes)", n);
    return r ? ERR_IF : ERR_OK;
}

static err_t netif_setup(struct netif* netif)
{
    netif->name[0] = 'n';
    netif->name[1] = 'c';
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, g_mac, 6);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHERNET | NETIF_FLAG_MLD6 | NETIF_FLAG_LINK_UP;
    netif->output_ip6 = ethip6_output;
    netif->linkoutput = link_output;
    return ERR_OK;
}

int ncm_start(const unsigned char mac[6], ncm_send_fn send, void* ctx)
{
    memcpy(g_mac, mac, 6);
    g_send = send;
    g_ctx = ctx;
    LOCK_TCPIP_CORE();
    if (!netif_add_noaddr(&g_netif, 0, netif_setup, tcpip_input)) {
        UNLOCK_TCPIP_CORE();
        say("ncm: no network interface");
        return -1;
    }
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    netif_set_link_up(&g_netif);
    netif_create_ip6_linklocal_address(&g_netif, 1);
    netif_ip6_addr_set_state(&g_netif, 0, IP6_ADDR_PREFERRED);
    snprintf(g_address, sizeof(g_address), "%s", ip6addr_ntoa(netif_ip6_addr(&g_netif, 0)));
    UNLOCK_TCPIP_CORE();
    {   /* lower case, as xcertplay writes it */
        char* c;
        for (c = g_address; *c; ++c)
            if (*c >= 'A' && *c <= 'F')
                *c = (char)(*c - 'A' + 'a');
    }
    say("ncm: interface up, MAC %02x:%02x:%02x:%02x:%02x:%02x, address %s", mac[0], mac[1], mac[2], mac[3], mac[4],
        mac[5], g_address);
    return 0;
}

void ncm_counts(unsigned long long* in, unsigned long long* out)
{
    *in = g_frames_in;
    *out = g_frames_out;
}

const char* ncm_address(void)
{
    return g_address;
}

static void frame_in(const unsigned char* f, unsigned n)
{
    struct pbuf* p;
    if (n < 14 || n > 1600)
        return;
    pcap_frame(f, n);
    if (++g_frames_in == 1)
        say("ncm: first frame in (%u bytes, from %02x:%02x:%02x:%02x:%02x:%02x, type %04x)", n, f[6], f[7], f[8], f[9],
            f[10], f[11], get16(f + 12));
    p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
    if (!p)
        return;
    pbuf_take(p, f, (u16_t)n);
    if (g_netif.input(p, &g_netif) != ERR_OK)
        pbuf_free(p);
}

/* the frames of one block */
static void block_in(const unsigned char* b, unsigned n)
{
    unsigned ndp = get16le(b + 10), hops = 0;
    while (ndp && hops++ < 16) {
        unsigned len, next, e;
        if (ndp + 12 > n || (get32le(b + ndp) & 0x00ffffffu) != (NDP16 & 0x00ffffffu))
            return;
        len = get16le(b + ndp + 4);
        next = get16le(b + ndp + 6);
        for (e = ndp + 8; e + 4 <= ndp + len && e + 4 <= n; e += 4) {
            const unsigned at = get16le(b + e), dl = get16le(b + e + 2);
            if (!at || !dl)
                break;
            if (at + dl <= n)
                frame_in(b + at, dl);
        }
        ndp = next;
    }
}

/* bytes from the bulk IN pipe (one or more blocks, maybe a pad byte after one of a multiple of 512 bytes) */
void ncm_input(const unsigned char* data, unsigned n)
{
    if (n > sizeof(g_rx) - g_rxn) {
        say("ncm: receive buffer overflow, %u bytes dropped", g_rxn);
        g_rxn = 0;
        if (n > sizeof(g_rx))
            return;
    }
    memcpy(g_rx + g_rxn, data, n);
    g_rxn += n;
    for (;;) {
        unsigned len, used;
        if (g_rxn < 12)
            return;
        if (get32le(g_rx) != NTH16) {   /* not at a block: on to the next "NCMH" */
            unsigned i;
            for (i = 1; i + 4 <= g_rxn && get32le(g_rx + i) != NTH16; ++i) {
            }
            if (i > 1 || g_rx[0])
                say("ncm: %u bytes outside a block skipped", i);
            memmove(g_rx, g_rx + i, g_rxn - i);
            g_rxn -= i;
            continue;
        }
        len = get16le(g_rx + 8);
        if (len < 12 || len > sizeof(g_rx)) {
            say("ncm: block length %u: buffer reset", len);
            g_rxn = 0;
            return;
        }
        if (g_rxn < len)
            return;
        block_in(g_rx, len);
        used = len;
        if (len % 512 == 0 && g_rxn > len && g_rx[len] == 0)
            used = len + 1;   /* the short-packet pad byte */
        memmove(g_rx, g_rx + used, g_rxn - used);
        g_rxn -= used;
    }
}
