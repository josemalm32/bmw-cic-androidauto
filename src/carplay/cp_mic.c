/* nbt_carplay: the car's microphone to the iPhone (Siri, calls).
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 *
 * The iPhone gives its microphone port in the main audio stream's SETUP and takes RTP packets as the audio it sends:
 * 12-byte header (payload type 100), big-endian LPCM sealed with ChaCha20-Poly1305 (key "DataStream-Input-Encryption-
 * Key", AAD = header bytes 4..11, nonce = 4 zero bytes + a 64-bit little-endian counter, which follows the tag).
 * Captured as Android Auto's aa_mic does: A2I endpoint /output_multich_0, 22050 Hz, 4 channels (the plain NBT's
 * microphone bus); channels 0 and 1 averaged, resampled to the iPhone's rate, amplified (NBT_MIC_GAIN, default 4).
 * /fs/ram3/nbt-aa-mic exists while it runs (nbt_knob: the car's own source is muted meanwhile).
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cp.h"
#include "cp_a2i.h"
#include "cp_crypto.h"

#define MIC_FLAG "/fs/ram3/nbt-aa-mic"

static pthread_t g_thread;
static int g_on;
static volatile int g_closing;
static struct net_addr g_to;
static unsigned char g_key[32];
static int g_rate, g_channels, g_frames;

static void* mic_thread(void* arg)
{
    struct a2i a;
    struct a2i_config c;
    void* h = 0;
    const char* endpoint = getenv("NBT_MIC_ENDPOINT") ? getenv("NBT_MIC_ENDPOINT") : "/output_multich_0";
    const unsigned in_rate = getenv("NBT_MIC_RATE") ? (unsigned)atoi(getenv("NBT_MIC_RATE")) : 22050;
    const unsigned in_ch = getenv("NBT_MIC_CHANNELS") ? (unsigned)atoi(getenv("NBT_MIC_CHANNELS")) : 4;
    const int gain = getenv("NBT_MIC_GAIN") ? atoi(getenv("NBT_MIC_GAIN")) : 4;
    const unsigned per_packet = g_frames > 0 ? (unsigned)g_frames : (unsigned)g_rate / 50;   /* 20 ms */
    short* pkt = (short*)malloc(per_packet * 2 * (unsigned)g_channels);
    unsigned char* wire = (unsigned char*)malloc(12 + per_packet * 2 * (unsigned)g_channels + 16 + 8);
    unsigned n = 0, pos = 0, seq = 0, ts = 0;
    unsigned long long counter = 0, sent = 0;
    int prev = 0, s = -1, r;
    (void)arg;
    if (!pkt || !wire || in_ch < 1 || in_rate < 8000 || a2i_load(&a)) {
        say("mic: no microphone (A2I library or memory)");
        goto out;
    }
    s = net_udp(0, 0);
    if (s < 0)
        goto out;
    memset(&c, 0, sizeof(c));
    c.magic = 0x02000000;
    c.direction = 1;
    c.one = 1;
    c.buffers = 8;
    c.buffer_bytes = 2048;
    c.channels = in_ch;
    c.bytes_per_sample = 2;
    c.rate = in_rate;
    c.endpoint = endpoint;
    c.interleaved = in_ch > 1;
    c.cookie = 0x0ABABABA;
    c.timeout_ms = 2000;
    r = a.open(&h, &c);
    if (r || !h) {
        say("mic: %s: open failed (%d)", endpoint, r);
        goto out;
    }
    r = a.start(h);
    if (r) {
        say("mic: start failed (%d)", r);
        a.close(&h);
        goto out;
    }
    flag_set(MIC_FLAG, 0);
    say("mic: %s %u Hz %u ch -> %d Hz %d ch, %u frames per packet, gain %d", endpoint, in_rate, in_ch, g_rate,
        g_channels, per_packet, gain);
    while (!g_closing && !g_stop) {
        void* buf = 0;
        size_t size = 0;
        const short* sm;
        size_t count, f;
        if (a.notification(h)) {
            sleep_ms(1);
            continue;
        }
        if (a.buffer(h, &buf, &size) || !buf || !size)
            continue;
        sm = (const short*)buf;
        count = size / (2 * in_ch);
        for (f = 0; f < count; ++f, sm += in_ch) {
            const int cur = in_ch > 1 ? (sm[0] + sm[1]) / 2 : sm[0];
            while (pos < (unsigned)g_rate) {   /* outputs between prev and cur */
                int v = (prev + (int)((long long)(cur - prev) * pos / g_rate)) * gain, k;
                if (v > 32767)
                    v = 32767;
                if (v < -32768)
                    v = -32768;
                for (k = 0; k < g_channels; ++k)
                    pkt[n * g_channels + k] = (short)v;
                if (++n == per_packet) {
                    unsigned char nonce[12];
                    unsigned i;
                    const unsigned bytes = per_packet * 2 * (unsigned)g_channels;
                    unsigned char* body = (unsigned char*)pkt;
                    for (i = 0; i + 1 < bytes; i += 2) {   /* to big-endian */
                        const unsigned char t = body[i];
                        body[i] = body[i + 1];
                        body[i + 1] = t;
                    }
                    wire[0] = 0x80;
                    wire[1] = 100 & 0x7f;
                    put16(wire + 2, seq & 0xffff);
                    put32(wire + 4, ts);
                    memset(wire + 8, 0, 4);
                    nonce_counter(nonce, counter);
                    aead_seal(g_key, nonce, wire + 4, 8, body, bytes, wire + 12);
                    put64le(wire + 12 + bytes + 16, counter);
                    net_sendto(s, wire, (int)(12 + bytes + 16 + 8), &g_to);
                    ++counter;
                    ++seq;
                    ts += per_packet;
                    ++sent;
                    n = 0;
                }
                pos += in_rate;
            }
            pos -= (unsigned)g_rate;
            prev = cur;
        }
    }
    a.stop(h);
    a.close(&h);
    say("mic: closed after %llu packets", sent);
out:
    flag_clear(MIC_FLAG);
    net_close(s);
    free(pkt);
    free(wire);
    return 0;
}

void mic_start(const struct net_addr* to, const unsigned char key[32], int rate, int channels, int payload_type,
               int frames_per_packet)
{
    (void)payload_type;
    mic_stop();
    if (rate < 8000 || channels < 1 || channels > 2) {
        say("mic: the iPhone's format %d Hz %d ch is not taken", rate, channels);
        return;
    }
    g_to = *to;
    memcpy(g_key, key, 32);
    g_rate = rate;
    g_channels = channels;
    g_frames = frames_per_packet > 0 && frames_per_packet <= 4096 ? frames_per_packet : 0;
    g_closing = 0;
    g_on = !pthread_create(&g_thread, 0, mic_thread, 0);
}

void mic_stop(void)
{
    if (!g_on)
        return;
    g_closing = 1;
    pthread_join(g_thread, 0);
    g_on = 0;
}
