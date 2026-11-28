/* nbt_carplay: the screen stream (step c): H.264 from the iPhone into nbt_decoder's FIFO.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 *
 * The iPhone connects to the port we gave in SETUP (stream type 110) and sends messages of a 128-byte header (u32 LE
 * body size at 0, opcode at 4) and a body: opcode 1 = the codec configuration (avcC: SPS and PPS), not encrypted;
 * opcode 0 = one frame, ChaCha20-Poly1305 with the header as AAD and the frame counter as nonce, NAL units with
 * 4-byte (avcC) lengths. Written to the FIFO as Annex B (start codes), SPS / PPS before each key frame, and an access
 * unit delimiter after each frame, so nbt_decoder decodes it at once (as oneplus_native does for Android Auto).
 * --dump FILE keeps a copy of what goes into the FIFO (plays with ffplay).
 * A still screen gets no frames (car 2026-11-23: gaps of several seconds; only opcode 2, an empty message, every
 * second), and nbt_decoder hides its window after decoder.stale ms (5000) without a picture: after 3 s without a frame
 * a key frame is asked for (forceKeyFrame), at most every 3 s.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cp.h"
#include "cp_crypto.h"

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static int g_on, g_listen = -1, g_conn = -1;
static volatile int g_closing;
static unsigned char g_key[32];
#include <sys/mman.h>
#include "aa_ring.h"

static ring_hdr *cic_ring;
static int cic_need_idr;

static void cic_config(const unsigned char *cfg, unsigned size)
{
    if (size < 7 || size > RING_CFGMAX || (cfg[4] & 3) != 3) {
        if (cic_ring)
            cic_ring->cfg_len = 0;
        cic_need_idr = 1;
        return;
    }
    if (!cic_ring) {
        int fd = shm_open(CARPLAY_RING_NAME, O_RDWR | O_CREAT, 0600);
        void *mem;
        if (fd < 0)
            return;
        if (ftruncate(fd, RING_BYTES)) {
            close(fd);
            return;
        }
        mem = mmap(0, RING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (mem == MAP_FAILED)
            return;
        cic_ring = mem;
        if (cic_ring->magic != RING_MAGIC) {
            memset(cic_ring, 0, sizeof(*cic_ring));
            cic_ring->size = RING_DATA;
            cic_ring->magic = RING_MAGIC;
        }
    }
    cic_ring->width = g_opt.width;
    cic_ring->height = g_opt.height;
    memcpy(cic_ring->cfg, cfg, size);
    cic_ring->cfg_len = size;
    __asm__ __volatile__("" ::: "memory");
    ++cic_ring->cfg_gen;
    cic_need_idr = 1;
}

static void cic_frame(const unsigned char *frame, unsigned size, int idr)
{
    ring_hdr *r = cic_ring;
    ring_rec record;
    unsigned need, w, rd;
    unsigned char *data;

    if (!r || !r->cfg_len || !r->reader_pid || size > RING_DATA / 2)
        return;
    if (r->want_idr) {
        r->want_idr = 0;
        airplay_key_frame();
        cic_need_idr = 1;
    }
    if (cic_need_idr && !idr)
        return;
    need = (sizeof(record) + size + 3u) & ~3u;
    w = r->wpos;
    rd = r->rpos;
    data = (unsigned char *)(r + 1);
    if (w >= rd && need + (rd == 0 ? 4u : 0u) > RING_DATA - w) {
        uint32_t wrap = REC_WRAP;
        if (need + 4u > rd)
            goto drop;
        memcpy(data + w, &wrap, sizeof(wrap));
        w = 0;
    } else if (w < rd && need + 4u > rd - w) {
        goto drop;
    }
    memset(&record, 0, sizeof(record));
    record.kind = REC_FRAME;
    record.len = size;
    record.idr = idr;
    record.pts_us = (uint64_t)ms_now() * 1000u;
    memcpy(data + w, &record, sizeof(record));
    memcpy(data + w + sizeof(record), frame, size);
    __asm__ __volatile__("" ::: "memory");
    r->wpos = (w + need) % RING_DATA;
    ++r->frames_in;
    cic_need_idr = 0;
    return;
drop:
    ++r->frames_drop;
    if (!cic_need_idr)
        airplay_key_frame();
    cic_need_idr = 1;
}

static int g_fifo = -1;
static FILE* g_dump;
static unsigned char g_params[4096];   /* SPS and PPS in Annex B */
static unsigned g_params_n, g_nal_len = 4;
static unsigned long long g_frames;

static long long g_fifo_retry;

static int fifo_write(const unsigned char* b, unsigned n)
{
    if (g_dump)
        fwrite(b, 1, n, g_dump);
    if (!g_opt.fifo || !g_opt.fifo[0])
        return 0;
    if (g_fifo < 0) {
        if (ms_now() < g_fifo_retry)
            return -1;
        /* not blocking while nobody reads (no nbt_decoder: frames dropped, tried again every second) */
        g_fifo = open(g_opt.fifo, O_WRONLY | O_NONBLOCK);
        if (g_fifo < 0) {
            if (!g_fifo_retry)
                say("screen: %s: %s (frames dropped until the decoder reads it)", g_opt.fifo, strerror(errno));
            g_fifo_retry = ms_now() + 1000;
            return -1;
        }
        fcntl(g_fifo, F_SETFL, fcntl(g_fifo, F_GETFL) & ~O_NONBLOCK);
        g_fifo_retry = 0;
        say("screen: writing into %s", g_opt.fifo);
        if (g_params_n && b != g_params) {   /* a decoder that comes late: parameters, and a key frame soon */
            const unsigned char* keep = g_params;
            if (write(g_fifo, keep, g_params_n) < 0) {
            }
            airplay_key_frame();
        }
    }
    while (n) {
        const ssize_t w = write(g_fifo, b, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            say("screen: FIFO: %s (the decoder went away)", strerror(errno));
            close(g_fifo);
            g_fifo = -1;
            g_fifo_retry = ms_now() + 1000;
            return -1;
        }
        b += w;
        n -= (unsigned)w;
    }
    return 0;
}

/* the avcC record in a configuration body: its SPS and PPS as Annex B */
static void config(const unsigned char* b, unsigned n)
{
    unsigned i, at, count, k, out = 0;
    const unsigned char* c = 0;
    unsigned cn = 0;
    for (i = 0; i + 4 <= n; ++i)
        if (!memcmp(b + i, "avcC", 4)) {
            c = b + i + 4;
            cn = n - i - 4;
            break;
        }
    if (!c) {   /* the record itself */
        c = b;
        cn = n;
    }
    if (cn < 7 || c[0] != 1) {
        say("screen: configuration without an avcC record (%u bytes); HEVC is not offered", n);
        hexdump("configuration", b, n < 64 ? n : 64);
        return;
    }
    g_nal_len = (c[4] & 3) + 1u;
    at = 5;
    for (k = 0; k < 2 && at < cn; ++k) {   /* SPS list, then PPS list */
        count = k == 0 ? c[at] & 0x1f : c[at];
        ++at;
        for (i = 0; i < count && at + 2 <= cn; ++i) {
            const unsigned len = get16(c + at);
            at += 2;
            if (at + len > cn || out + 4 + len > sizeof(g_params))
                break;
            memcpy(g_params + out, "\0\0\0\1", 4);
            memcpy(g_params + out + 4, c + at, len);
            out += 4 + len;
            at += len;
        }
    }
    g_params_n = out;
    if (out && at <= cn)
        cic_config(c, at);
    say("screen: H.264 profile %u level %u, %u-byte NAL lengths, %u bytes of SPS/PPS", c[1], c[3], g_nal_len, out);
    fifo_write(g_params, g_params_n);
}

/* length-prefixed NAL units to Annex B (into out, up to 4 bytes per NAL unit longer for 1/2-byte lengths) */
static unsigned annexb(const unsigned char* in, unsigned n, unsigned char* out, int* key)
{
    unsigned at = 0, o = 0;
    *key = 0;
    while (at + g_nal_len <= n) {
        unsigned len = 0, i;
        for (i = 0; i < g_nal_len; ++i)
            len = len << 8 | in[at + i];
        at += g_nal_len;
        if (!len || at + len > n)
            break;
        if ((in[at] & 0x1f) == 5)
            *key = 1;
        memcpy(out + o, "\0\0\0\1", 4);
        memcpy(out + o + 4, in + at, len);
        o += 4 + len;
        at += len;
    }
    return o;
}

static int read_all(int s, unsigned char* b, unsigned n)
{
    while (n) {
        const int r = net_recv(s, b, (int)n, 1000);
        if (r == -2) {
            if (g_closing || g_stop)
                return -1;
            continue;
        }
        if (r <= 0)
            return -1;
        b += r;
        n -= (unsigned)r;
    }
    return 0;
}

static void* screen_thread(void* arg)
{
    unsigned char head[128];
    unsigned char *body = 0, *plain = 0, *out = 0;
    unsigned cap = 0;
    unsigned long long counter = 0, bytes = 0, last_frames = 0;
    long long last = ms_now(), first = 0, last_frame = ms_now(), asked = 0;
    int heartbeats = 0;
    struct net_addr peer;
    int c = -1;
    static const unsigned char aud[6] = {0, 0, 0, 1, 0x09, 0xf0};
    (void)arg;
    while (!g_closing && !g_stop) {
        c = net_accept(g_listen, &peer, 1000);
        if (c >= 0 || c == -1)
            break;
    }
    if (c < 0)
        return 0;
    pthread_mutex_lock(&g_m);
    g_conn = c;
    pthread_mutex_unlock(&g_m);
    say("screen: the iPhone connected");
    while (!g_closing && !g_stop) {
        unsigned size, op;
        if (read_all(c, head, sizeof(head)))
            break;
        size = get32le(head);
        op = head[4];
        if (size > 8 * 1024 * 1024) {
            say("screen: message of %u bytes: stopping", size);
            break;
        }
        if (size + 64 > cap) {
            cap = size + 65536;
            free(body);
            free(plain);
            free(out);
            body = (unsigned char*)malloc(cap);
            plain = (unsigned char*)malloc(cap);
            out = (unsigned char*)malloc(cap * 2);
            if (!body || !plain || !out)
                break;
        }
        if (size && read_all(c, body, size))
            break;
        if (op != 0 && ms_now() - last_frame >= 3000 && ms_now() - asked >= 3000) {
            asked = ms_now();   /* a still screen: a key frame keeps nbt_decoder's window */
            airplay_key_frame();
        }
        if (op == 1) {
            config(body, size);
            continue;
        }
        if (op != 0) {
            if (op != 2 || !heartbeats++)
                say("screen: message type %u (%u bytes) ignored%s", op, size, op == 2 ? " (comes every second)" : "");
            continue;
        }
        last_frame = ms_now();
        {
            unsigned char nonce[12];
            const unsigned char* frame = body;
            unsigned fn = size, on;
            int key;
            if (size >= 16) {
                nonce_counter(nonce, counter++);
                if (aead_open(g_key, nonce, head, sizeof(head), body, size, plain)) {
                    say("screen: frame %llu not authentic: stopping", counter - 1);
                    break;
                }
                frame = plain;
                fn = size - 16;
            }
            on = annexb(frame, fn, out, &key);
            if (on && g_nal_len == 4)
                cic_frame(frame, fn, key);
            if (key && g_params_n)
                fifo_write(g_params, g_params_n);
            if (on)
                fifo_write(out, on);
            fifo_write(aud, sizeof(aud));
            ++g_frames;
            bytes += on;
            if (!first) {
                first = ms_now();
                say("screen: first frame (%u bytes, %s)", on, key ? "key frame" : "not a key frame");
            }
        }
        if (ms_now() - last >= 10000) {
            say("screen: %llu frames (%.1f/s), %.1f Mbit/s", g_frames,
                (double)(g_frames - last_frames) * 1000.0 / (double)(ms_now() - last),
                (double)bytes * 8 / 1000.0 / (double)(ms_now() - last));
            last = ms_now();
            last_frames = g_frames;
            bytes = 0;
        }
        if (g_opt.stop_after == STOP_VIDEO && g_frames >= 60)
            cp_reached(STOP_VIDEO);
    }
    say("screen: stream ended after %llu frames", g_frames);
    free(body);
    free(plain);
    free(out);
    pthread_mutex_lock(&g_m);
    g_conn = -1;
    pthread_mutex_unlock(&g_m);
    net_close(c);
    return 0;
}

int screen_open(const unsigned char key[32], unsigned short* port)
{
    screen_close();
    memcpy(g_key, key, 32);
    g_listen = net_tcp_listen(0, port);
    if (g_listen < 0) {
        say("screen: no port");
        return -1;
    }
    if (g_opt.dump && !g_dump) {
        g_dump = fopen(g_opt.dump, "wb");
        if (!g_dump)
            say("screen: %s: %s", g_opt.dump, strerror(errno));
    }
    signal(SIGPIPE, SIG_IGN);
    g_closing = 0;
    g_frames = 0;
    g_params_n = 0;
    g_nal_len = 4;
    if (pthread_create(&g_thread, 0, screen_thread, 0)) {
        net_close(g_listen);
        g_listen = -1;
        return -1;
    }
    g_on = 1;
    return 0;
}

void screen_close(void)
{
    if (!g_on)
        return;
    g_closing = 1;
    pthread_join(g_thread, 0);
    g_on = 0;
    net_close(g_listen);
    g_listen = -1;
    if (cic_ring) {
        cic_ring->cfg_len = 0;
        munmap(cic_ring, RING_BYTES);
        cic_ring = 0;
    }
    if (g_fifo >= 0) {   /* nbt_decoder sees the end of this stream and waits for the next writer */
        close(g_fifo);
        g_fifo = -1;
    }
    if (g_dump)
        fflush(g_dump);
}
