/* cicaalive: live Android Auto H.264 + AAC -> TI DM642 DSP, own dspipc writer.
 *
 * Started by the AAPlugin plugin once the video/audio path is routed (source 8 ->
 * sink 1, layer 26). Usage: cicaalive live [logfile]. Without logfile nothing is
 * written. Stops when /dev/shmem/aap.stop exists or the parent (HmiMain) exits.
 *
 * Protocol = BMW writer_dm642.so: dspipc_create dm642.0, alloc 0x840c shm, command
 * channels 0x15/0x14, stream channel 0x20, 0x8400-byte 'MPEG' packets of MRAE
 * entries, one in flight, buffer-return ack. Input = shm rings /cicaa_live
 * (avcC + length-prefixed frames) and /cicaa_audio (phone AAC), filled by libcicaa. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "aa_ring.h"
#include "aap_paths.h"
#include "aac_sil.h"

/* libdspipc.so (car /lib/libdspipc.so), prototypes from decompile */
/* v48e: real call site libdspipc 0x3388..0x339a: cb(r4=channel, r5=size, r6=data, r7=user);
 * buffer-return 0x3438..0x3446: cb(r4=channel, r5=x, r6=user). (Ghidra had dropped r4.) */
typedef int (*cmd_cb)(void *ch, unsigned size, const unsigned char *data, void *user);
typedef int (*buf_cb)(void *ch, unsigned x, void *user);
typedef struct { int fd; unsigned char *ptr; int id; } dsp_mem;
int dspipc_create(const char *path, void **h);
int dspipc_destroy(void **h);
int dspipc_alloc(void *h, unsigned size, dsp_mem *m);
int dspipc_free(void *h, dsp_mem *m);
int dspipc_opencommandchannel_prio(void *h, int chan, cmd_cb cb, void *user, void **ch, int prio);
int dspipc_openstreamchannel_prio(void *h, int chan, cmd_cb msg, buf_cb buf, void *user, void **ch, int prio);
int dspipc_closechannel(void *ch);
int dspipc_sendmessage(void *ch, unsigned size, void *msg);
int dspipc_senddata(void *ch, void *desc);
int dspipc_drain(void *ch);

#define PKT 0x8400
#define DATA_MAX 0x7ff8
#define ENT_MAX 123

static FILE *g_log; static struct timespec g_t0;
static void lg(const char *f, ...)
{
    struct timespec t; va_list a; double s;
    if (!g_log) return;
    clock_gettime(CLOCK_MONOTONIC, &t);
    s = (t.tv_sec - g_t0.tv_sec) + (t.tv_nsec - g_t0.tv_nsec) / 1e9;
    fprintf(g_log, "%9.3f ", s); va_start(a, f); vfprintf(g_log, f, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}
static int stop_requested(void) { return access(AAP_STOP, F_OK) == 0; }
static uint64_t now_us(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec*1000000u + t.tv_nsec/1000u; }

/* ---------------- DSP side ---------------- */
static struct {
    void *h, *dbg, *cmd, *strm; dsp_mem mem; int have_mem;
    pthread_mutex_t mu; pthread_cond_t cv;
    volatile int buf_back, started, stopped, errors, fatal;
    uint32_t mra;
} d = { 0, 0, 0, 0, {-1, 0, 0}, 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER };

/* v48d: DSP callbacks run on libdspipc's own threads (small stack, unknown size
 * semantics; writer_dm642 clamps size to 0xfe). Callback = copy <=256 bytes to a
 * queue + state flags only. No stdio here. Logger thread formats later. */
#define MQ_N 64
#define MQ_SZ 256
static struct { unsigned n, size; unsigned char b[MQ_SZ]; } mq[MQ_N];
static volatile unsigned mq_w, mq_r, mq_lost, strm_msgs;
static pthread_mutex_t mqmu = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_quit, g_resync;
static int on_cmd(void *ch, unsigned size, const unsigned char *m, void *user)
{
    (void)ch;
    unsigned n = size > MQ_SZ ? MQ_SZ : size; uint32_t type = 0, code = 0;
    (void)user;
    if (!m) return -1;
    if (n >= 0x14) { memcpy(&type, m + 0x0c, 4); memcpy(&code, m + 0x10, 4); }
    pthread_mutex_lock(&mqmu);
    if (mq_w - mq_r < MQ_N) { unsigned i = mq_w % MQ_N; mq[i].n = n; mq[i].size = size; memcpy(mq[i].b, m, n); mq_w++; }
    else mq_lost++;
    pthread_mutex_unlock(&mqmu);
    if (n > 6 && m[6] == 0xe7) {
        pthread_mutex_lock(&d.mu);
        if (type == 4) d.started = 1;
        if (type == 5) d.stopped = 1;
        if (type == 3) { d.errors++; if (code == 3) d.fatal = 1; else g_resync = 1; }
        pthread_cond_broadcast(&d.cv);
        pthread_mutex_unlock(&d.mu);
    }
    return 0;
}
static int on_strm(void *ch, unsigned size, const unsigned char *m, void *u) { (void)ch; (void)size; (void)m; (void)u; strm_msgs++; return 0; }
static int on_buf(void *ch, unsigned x, void *u)
{ (void)ch; (void)x; (void)u; pthread_mutex_lock(&d.mu); d.buf_back = 1; pthread_cond_broadcast(&d.cv); pthread_mutex_unlock(&d.mu); return 0; }
/* v48f: DSP starts its A/V clock only when the video ES pipe is full (no audio
 * stream) -> ~30 s with tiny AA P-frames. We prefill with H.264 filler NALs
 * (type 12, ignored by decoders) and, once "StartRefClk" is seen, give frames
 * PTS = arrival - clock_start + 150 ms so they show right after arrival. */
static volatile int g_clk, g_afull;
static volatile uint64_t g_clk_us;
static void mq_drain(void)
{
    for (;;) {
        unsigned char b[MQ_SZ]; unsigned n, size, i, k = 0; char txt[MQ_SZ + 1]; uint32_t type = 0, code = 0;
        pthread_mutex_lock(&mqmu);
        if (mq_r == mq_w) { pthread_mutex_unlock(&mqmu); break; }
        i = mq_r % MQ_N; n = mq[i].n; size = mq[i].size; memcpy(b, mq[i].b, n); mq_r++;
        pthread_mutex_unlock(&mqmu);
        if (n >= 0x14) { memcpy(&type, b + 0x0c, 4); memcpy(&code, b + 0x10, 4); }
        for (i = 0x10; i < n; i++)
            if (b[i] >= 32 && b[i] < 127) txt[k++] = (char)b[i]; else if (k && txt[k-1] != ' ') txt[k++] = ' ';
        txt[k] = 0;
        if (!g_clk && (strstr(txt, "StartRefClk") || strstr(txt, "AVPipeReady"))) {
            g_clk_us = now_us(); g_clk = 1; lg("CLOCK STARTED (DSP pipe ready) -> live PTS");
        }
        if (!g_afull && strstr(txt, "Audio Pipe ES buffer is Full")) { g_afull = 1; lg("AUDIO PIPE FULL seen"); }
        if (n > 6 && b[6] == 0xe7 && type == 3) lg("DSP AVP_EVENT_ERROR code=%u", (unsigned)code);
        else if (n > 6 && b[6] == 0xe7 && type == 4) lg("DSP AVP_EVENT_STARTED");
        else if (n > 6 && b[6] == 0xe7 && type == 5) lg("DSP AVP_EVENT_STOPPED");
        else if (k > 3) lg("DSP(%02x/%u size=%u) %s", n > 6 ? b[6] : 0, (unsigned)type, size, txt);
        else lg("DSP msg id=0x%02x type=%u size=%u", n > 6 ? b[6] : 0, (unsigned)type, size);
    }
    if (mq_lost) { lg("DSP msgs lost=%u", (unsigned)mq_lost); mq_lost = 0; }
}
static void *logger(void *u) { (void)u; while (!g_quit) { mq_drain(); usleep(50000); } mq_drain(); return 0; }
/* SIGSEGV/SIGBUS: async-safe line with fault address, then exit 139. */
static int g_logfd = -1;
static void hexw(char *o, unsigned v) { int i; for (i = 7; i >= 0; i--) { o[i] = "0123456789abcdef"[v & 15]; v >>= 4; } }
static void on_fault(int sig, siginfo_t *si, void *ctx)
{
    char b[64] = "CRASH sig=00 addr=0x00000000 pc?\n"; (void)ctx;
    b[10] = (char)('0' + sig / 10); b[11] = (char)('0' + sig % 10);
    hexw(b + 20, si ? (unsigned)si->si_addr : 0);
    if (g_logfd >= 0) write(g_logfd, b, 33);
    _exit(139);
}

/* 128-byte command block, checksum = sum of words over header+payload (4-aligned) */
static int send_cmd(const char *name, unsigned id, const void *payload, unsigned len)
{
    uint32_t b[32]; unsigned char *c = (unsigned char *)b; unsigned al = (len + 3) & ~3u, i; uint32_t sum = 0; int rc;
    memset(b, 0, sizeof b);
    c[2] = 0xff; c[3] = 0xff; c[4] = 0xad; c[6] = (unsigned char)id; c[7] = (unsigned char)(id >> 8);
    c[8] = 2; c[10] = (unsigned char)len; c[11] = (unsigned char)(len >> 8);
    if (len) memcpy(c + 12, payload, len);
    for (i = 0; i < (al + 12) / 4; i++) sum += b[i];
    memcpy(c + 12 + al, &sum, 4);
    rc = dspipc_sendmessage(d.cmd, al + 0x10, b);
    lg("CMD %s id=0x%x len=%u rc=%d", name, id, len, rc);
    return rc;
}
static int cmd_u32(const char *name, unsigned id, uint32_t v) { return send_cmd(name, id, &v, 4); }

static int dsp_open(void)
{
    int rc; unsigned char lvl[4] = {10, 0, 0, 0};
    rc = dspipc_create("/dev/dspipc/dm642.0/", &d.h); lg("dspipc_create rc=%d h=%p", rc, d.h); if (rc) return -1;
    rc = dspipc_alloc(d.h, 0x840c, &d.mem); lg("dspipc_alloc rc=%d ptr=%p id=%d", rc, (void *)d.mem.ptr, d.mem.id); if (rc) return -1;
    d.have_mem = 1; memset(d.mem.ptr, 0xff, 0x840c);
    rc = dspipc_opencommandchannel_prio(d.h, 0x15, on_cmd, &d, &d.dbg, 10); lg("open dbg 0x15 rc=%d", rc);
    rc = dspipc_opencommandchannel_prio(d.h, 0x14, on_cmd, &d, &d.cmd, 10); lg("open cmd 0x14 rc=%d", rc); if (rc) return -1;
    { int t; for (t = 0; t < 20; t++) {   /* v54b: previous session may still hold stream 0x20 */
        rc = dspipc_openstreamchannel_prio(d.h, 0x20, on_strm, on_buf, &d, &d.strm, 10); lg("open stream 0x20 rc=%d try=%d", rc, t);
        if (!rc) break; d.strm = 0; usleep(250000); } }
    if (rc) return -1;
    send_cmd("SetDbgLvl", 0x82, lvl, 4);
    send_cmd("GetBinInfo", 0x80, 0, 0);
    return 0;
}
static int dsp_start(void)
{
    struct timespec t;
    cmd_u32("MuteRampUp", 0xa2, 0x40000);
    cmd_u32("MuteRampDown", 0xa3, 10);
    cmd_u32("MuteTrigger", 0xa4, 0);
    send_cmd("Start", 0xe1, 0, 0);
    clock_gettime(CLOCK_REALTIME, &t); t.tv_sec += 2;
    pthread_mutex_lock(&d.mu);
    while (!d.started) if (pthread_cond_timedwait(&d.cv, &d.mu, &t)) break;
    pthread_mutex_unlock(&d.mu);
    lg("START wait started=%d", d.started);
    cmd_u32("SetPlaySpeed", 0xed, 100);
    send_cmd("Resume", 0xe8, 0, 0);
    return 0;
}
static void dsp_close(void)
{
    if (d.cmd) { send_cmd("Stop", 0xe2, 0, 0); usleep(300000); if (d.strm) dspipc_drain(d.strm); }
    if (d.strm) dspipc_closechannel(d.strm);
    if (d.cmd) dspipc_closechannel(d.cmd);
    if (d.dbg) dspipc_closechannel(d.dbg);
    if (d.have_mem) dspipc_free(d.h, &d.mem);
    if (d.h) dspipc_destroy(&d.h);
    d.h = d.dbg = d.cmd = d.strm = 0; d.have_mem = 0;
    lg("DSP closed");
}

/* ---------------- packet builder (MmInp* layout) ---------------- */
static struct { uint32_t dlen, n; } pk;
static unsigned char *P(void) { return d.mem.ptr; }
static void pkt_reset(void)
{
    unsigned char *p = P();
    memset(p, 0xff, PKT);
    memcpy(p, "MPEG", 4);
    memset(p + 0x8000, 0, 0x24);
    pk.dlen = pk.n = 0;
}
static int pkt_flush(void)
{
    unsigned char *p = P(); uint32_t v; struct { uint32_t n, flags; struct { uint32_t size, off; int id; } r[8]; } desc;
    struct timespec t; int rc, ok;
    if (!pk.n) return 0;
    v = pk.dlen; memcpy(p + 4, &v, 4);
    v = ENT_MAX; memcpy(p + 0x800c, &v, 4);
    memcpy(p + 0x8010, &pk.n, 4);
    v = ++d.mra; memcpy(p + 0x8014, &v, 4);
    memset(&desc, 0, sizeof desc);
    desc.n = 2; desc.flags = 1;
    desc.r[0].size = 0x8000; desc.r[0].off = 0; desc.r[0].id = d.mem.id;
    desc.r[1].size = 0x400;  desc.r[1].off = 0x8000; desc.r[1].id = d.mem.id;
    pthread_mutex_lock(&d.mu); d.buf_back = 0; pthread_mutex_unlock(&d.mu);
    rc = dspipc_senddata(d.strm, &desc);
    if (rc) { lg("senddata mra=%u rc=%d", (unsigned)d.mra, rc); return -1; }
    clock_gettime(CLOCK_REALTIME, &t); t.tv_sec += 10;
    pthread_mutex_lock(&d.mu);
    while (!d.buf_back) if (pthread_cond_timedwait(&d.cv, &d.mu, &t)) break;
    ok = d.buf_back; pthread_mutex_unlock(&d.mu);
    if (!ok) { lg("buffer return TIMEOUT mra=%u", (unsigned)d.mra); return -1; }
    if (d.mra <= 3 || d.mra % 300 == 0) lg("PKT mra=%u entries=%u data=%u", (unsigned)d.mra, (unsigned)pk.n, (unsigned)pk.dlen);
    pkt_reset();
    return 0;
}
static int ent(uint32_t a, uint32_t b)
{
    unsigned char *p = P();
    if (pk.n >= ENT_MAX && pkt_flush()) return -1;
    memcpy(p + 0x8024 + 8 * pk.n, &a, 4); memcpy(p + 0x8024 + 8 * pk.n + 4, &b, 4); pk.n++;
    return 0;
}
static int prop(unsigned type, unsigned sub, uint32_t val) { return ent(val, 0x80000000u | (type << 24) | (sub & 0xffffff)); }
static int data(const unsigned char *s, uint32_t len)
{
    while (len) {
        uint32_t room = DATA_MAX - pk.dlen, c;
        if (!room || pk.n >= ENT_MAX) { if (pkt_flush()) return -1; continue; }
        c = len < room ? len : room;
        memcpy(P() + 8 + pk.dlen, s, c);
        if (ent(pk.dlen, c)) return -1;      /* ent may flush only before writing; entry ok */
        pk.dlen += c; s += c; len -= c;
    }
    return 0;
}

/* video stream header, as writer_dm642 emitted for cic_f2 (car_logs/v46 capture) */
static int send_header(const uint8_t *avcc, uint32_t n, unsigned w, unsigned h)
{
    /* exact 92-byte blob from capture (words LE); host pointers at 0x00/0x58 kept */
    static const uint32_t tpl[23] = {
        0x0805f258, 0, 0x00010000, 0, 0, 0, 0, 0x01e00320, 0x00480000, 0x00480000, 0, 1,
        0, 0, 0, 0, 0, 0, 0, 0x00180000, 0x0000ffff, 0x20, 0x0805e1a8 };
    unsigned char blob[92];
    memcpy(blob, tpl, sizeof blob);
    blob[0x1c] = (unsigned char)w; blob[0x1d] = (unsigned char)(w >> 8);
    blob[0x1e] = (unsigned char)h; blob[0x1f] = (unsigned char)(h >> 8);
    memcpy(blob + 0x54, &n, 4);
    if (prop(121, 3, 0) || prop(121, 0x20, 0x80000002u) || prop(121, 0x21, 0x41564331u) ||
        prop(121, 0x24, 0x1f) || prop(30, 0, 2) || data(blob, sizeof blob) ||
        prop(121, 3, 0) || data(avcc, n)) return -1;
    lg("HEADER avcC=%u %ux%u", (unsigned)n, w, h);
    return 0;
}

/* v52: v51 car: SEI prefill never starts the clock (DSP sits in waitForAudioData).
 * DVD sessions start via "Audio Pipe ES buffer is Full" -> AVPipeReady -> StartRefClk Audio.
 * So we add a silent AAC-LC 48 kHz stereo track (format exactly as writer_dm642 capture
 * car_logs/v46: 0x79/4 select, 0x79/0x20=0x80000004, 0x21='M4AC', 0x22=2, 0x23=48000,
 * 0x1e=1 + 5-byte ASC; frames 0x79/4 + 0x0f PTS us). Frames padded with FIL to 546 B. */
#include "live.inc"

int main(int argc, char **argv)
{
    const char *logp = argc > 2 && argv[2][0] ? argv[2] : 0; int rc;
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    g_log = logp ? fopen(logp, "a") : 0;
    signal(SIGPIPE, SIG_IGN);
    if (g_log) g_logfd = fileno(g_log);
    { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO;
      sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); }
    { pthread_t lt; pthread_create(&lt, 0, logger, 0); }
    lg("cicaalive " AAP_VERSION " start pid=%d", getpid());
    rc = live(0);
    g_quit = 1; usleep(120000); mq_drain();
    lg("cicaalive exit rc=%d", rc);
    if (g_log) fclose(g_log);
    return rc;
}
