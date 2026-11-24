/* Carlinkit CPC200-CCPA dongle session (see ccpa_core.h).
 *
 * Video: VideoData = 20-byte header (width, height, flags, length, unknown) + H.264
 * Annex-B access unit. It goes to av_push() exactly like Android Auto video, so the
 * existing /cicaa_live ring -> cicaalive -> DM642 path is reused unchanged
 * (aa_video_native.c stamps its own arrival PTS).
 *
 * Audio: by default the dongle is told audioTransferOn: the phone streams its audio
 * over the car's own Bluetooth (A2DP/HFP), because the DM642 refused PCM in the car
 * (only AAC plays). With ccpa_usbaudio.on the PCM is written to the /cicaa_audio ring
 * instead (experimental, codec0 = PCM).
 *
 * Input: the plugin's iDrive events (cicaa_input queue) become CarPlay knob/button
 * commands. No touch emulation yet. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include "aa_core.h"
#include "aa_video.h"
#include "aa_aring.h"
#include "aap_paths.h"
#include "ccpa_core.h"

#define CCPA_MAGIC 0x55AA55AAu
#define CCPA_MAXPAY (1024u * 1024u)

enum {
    T_OPEN = 0x01, T_PLUGGED = 0x02, T_PHASE = 0x03, T_UNPLUGGED = 0x04, T_TOUCH = 0x05,
    T_VIDEO = 0x06, T_AUDIO = 0x07, T_COMMAND = 0x08, T_DISCONNECT_PHONE = 0x0f,
    T_MANUFACTURER = 0x14, T_BOXSETTINGS = 0x19, T_MEDIA = 0x2a, T_SENDFILE = 0x99,
    T_HEARTBEAT = 0xaa, T_SWVERSION = 0xcc
};
/* Command values (node-CarPlay CommandMapping) */
enum {
    C_FRAME = 12, C_MIC = 7, C_WIFI5G = 25, C_AUDIO_TRANSFER_ON = 22, C_AUDIO_TRANSFER_OFF = 23,
    C_LEFT = 100, C_RIGHT = 101, C_SELECT_DOWN = 104, C_SELECT_UP = 105, C_BACK = 106,
    C_UP = 113, C_DOWN = 114, C_HOME = 200, C_PLAYPAUSE = 203, C_NEXT = 204, C_PREV = 205,
    C_WIFI_ENABLE = 1000, C_WIFI_CONNECT = 1002
};
/* Plugged phone types */
enum { P_CARPLAY = 3, P_ANDROIDAUTO = 5 };

typedef struct {
    aa_transport_t *t;
    aa_log_fn lg;
    aa_status_fn st;
    unsigned w, h, fps, dpi;
    int usbaudio;
    int phone;               /* plugged phone type, 0 = none */
    int vopen;
    unsigned long vframes, vbytes, vdrop;
    unsigned long long last_hb, last_frame_req, opened_at;
    int wifi_connect_sent;
    unsigned char *pay;
    unsigned paycap;
} cc_t;

static unsigned long long mono_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000ULL + (unsigned long long)t.tv_nsec / 1000000ULL;
}
static void put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static unsigned get32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static int cc_send(cc_t *c, unsigned type, const void *pay, unsigned n)
{
    unsigned char b[16 + 1024];
    if (n > sizeof b - 16) return -1;
    put32(b, CCPA_MAGIC); put32(b + 4, n); put32(b + 8, type); put32(b + 12, ~type);
    if (n) memcpy(b + 16, pay, n);
    if (c->t->send(c->t->ctx, b, (int)(16 + n)) != (int)(16 + n)) {
        c->lg("CCPA tx type=0x%02x len=%u FAILED", type, n);
        return -1;
    }
    return 0;
}
static int cc_cmd(cc_t *c, unsigned v)
{
    unsigned char p[4]; put32(p, v);
    return cc_send(c, T_COMMAND, p, 4);
}
/* SendFile: u32 name_len (incl. NUL) | name\0 | u32 content_len | content */
static int cc_file(cc_t *c, const char *name, const void *data, unsigned n)
{
    unsigned char p[512];
    unsigned nl = (unsigned)strlen(name) + 1;
    if (8 + nl + n > sizeof p) return -1;
    put32(p, nl); memcpy(p + 4, name, nl); put32(p + 4 + nl, n); memcpy(p + 8 + nl, data, n);
    return cc_send(c, T_SENDFILE, p, 8 + nl + n);
}
static int cc_file_u32(cc_t *c, const char *name, unsigned v)
{
    unsigned char p[4]; put32(p, v);
    return cc_file(c, name, p, 4);
}

/* Read exactly n bytes. Returns n, 0 if nothing arrived within first_ms (only when
 * nothing of this item was read yet), <0 on error or a stall in the middle. */
static int cc_read(cc_t *c, unsigned char *b, unsigned n, int first_ms)
{
    unsigned got = 0;
    unsigned long long t0 = mono_ms();
    while (got < n) {
        int r = c->t->recv(c->t->ctx, b + got, (int)(n - got), got ? 500 : first_ms);
        if (r < 0) return -1;
        if (r == 0) {
            if (!got) return 0;
            if (mono_ms() - t0 > 3000) { c->lg("CCPA rx stall %u/%u", got, n); return -2; }
            continue;
        }
        got += (unsigned)r;
    }
    return (int)n;
}

/* --- optional PCM ring (ccpa_usbaudio.on) --- */
static aring_hdr *g_ar;
static void ar_open(cc_t *c)
{
    int fd; void *m;
    if (g_ar) return;
    fd = shm_open(ARING_NAME, O_RDWR | O_CREAT, 0666);
    if (fd < 0) { c->lg("CCPA audio ring open failed"); return; }
    if (ftruncate(fd, (off_t)ARING_BYTES)) { close(fd); return; }
    m = mmap(0, ARING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); close(fd);
    if (m == MAP_FAILED) return;
    g_ar = (aring_hdr *)m;
    if (g_ar->magic != ARING_MAGIC || g_ar->chsize != ARING_CH) {
        memset(g_ar, 0, sizeof *g_ar); g_ar->chsize = ARING_CH; g_ar->magic = ARING_MAGIC;
    }
}
static void ar_put(int ch, const unsigned char *b, unsigned n)
{
    uint8_t *d; uint32_t w;
    if (!g_ar) return;
    d = aring_data(g_ar, ch); w = g_ar->wpos[ch];
    if (n > ARING_CH) { b += n - ARING_CH; w += n - ARING_CH; n = ARING_CH; }
    while (n) {
        uint32_t o = w & (ARING_CH - 1), k = ARING_CH - o;
        if (k > n) k = n;
        memcpy(d + o, b, k); b += k; n -= k; w += k;
    }
    __asm__ __volatile__("" ::: "memory");
    g_ar->wpos[ch] = w;
}
static void ar_active(int ch, int on)
{
    if (!g_ar) return;
    if (on && !g_ar->active[ch]) g_ar->gen[ch]++;
    g_ar->active[ch] = on ? 1 : 0;
}

/* Optional settings: ccpa.txt "width height [fps [dpi]]" */
static void cc_config(cc_t *c)
{
    char b[64];
    int fd = open(AAP_CCPA_TXT, O_RDONLY), n;
    unsigned w = 0, h = 0, f = 0, d = 0;
    c->w = 800; c->h = 480; c->fps = 30; c->dpi = AAP_DPI;
    c->usbaudio = access(AAP_CCPA_USBAUDIO_ON, F_OK) == 0;
    if (fd < 0) return;
    n = read(fd, b, sizeof b - 1); close(fd);
    if (n <= 0) return;
    b[n] = 0;
    if (sscanf(b, "%u %u %u %u", &w, &h, &f, &d) >= 2 && w >= 320 && w <= 1920 && h >= 240 && h <= 1080) {
        c->w = w; c->h = h;
        if (f >= 10 && f <= 60) c->fps = f;
        if (d >= 80 && d <= 400) c->dpi = d;
    }
}

static int cc_start(cc_t *c)
{
    unsigned char o[28];
    char js[160];
    int r = 0;
    put32(o, c->w); put32(o + 4, c->h); put32(o + 8, c->fps);
    put32(o + 12, 5);        /* format */
    put32(o + 16, 49152);    /* packetMax */
    put32(o + 20, 2);        /* iBoxVersion */
    put32(o + 24, 2);        /* phoneWorkMode */
    r |= cc_file_u32(c, "/tmp/screen_dpi", c->dpi);
    r |= cc_send(c, T_OPEN, o, sizeof o);
    r |= cc_file_u32(c, "/tmp/night_mode", 0);
    r |= cc_file_u32(c, "/tmp/hand_drive_mode", 0);     /* LHD */
    r |= cc_file_u32(c, "/tmp/charge_mode", 1);
    r |= cc_file(c, "/etc/box_name", "BMW CIC", 7);
    snprintf(js, sizeof js, "{\"mediaDelay\":300,\"syncTime\":%lu,\"androidAutoSizeW\":%u,\"androidAutoSizeH\":%u}",
             (unsigned long)time(0), c->w, c->h);
    r |= cc_send(c, T_BOXSETTINGS, js, (unsigned)strlen(js));
    r |= cc_cmd(c, C_WIFI_ENABLE);
    r |= cc_cmd(c, C_WIFI5G);
    r |= cc_cmd(c, C_MIC);                                /* "car mic": we send none */
    r |= cc_cmd(c, c->usbaudio ? C_AUDIO_TRANSFER_OFF : C_AUDIO_TRANSFER_ON);
    r |= cc_file_u32(c, "/etc/android_work_mode", 1);     /* allow Android Auto too */
    c->lg("CCPA open sent: %ux%u@%u dpi=%u audio=%s rc=%d", c->w, c->h, c->fps, c->dpi,
          c->usbaudio ? "USB PCM ring" : "phone Bluetooth", r);
    c->opened_at = mono_ms();
    return r;
}

static void cc_input(cc_t *c, int *bye)
{
    int kind, code, val;
    while (cicaa_input_pop(&kind, &code, &val)) {
        if (kind == CICAA_IN_BYEBYE) { cc_send(c, T_DISCONNECT_PHONE, 0, 0); *bye = 1; continue; }
        if (kind == CICAA_IN_REL) {
            int n = val < 0 ? -val : val, i;
            if (n > 8) n = 8;
            for (i = 0; i < n; ++i) cc_cmd(c, val < 0 ? C_LEFT : C_RIGHT);
            continue;
        }
        if (kind != CICAA_IN_KEY) continue;
        /* Android keycodes from the plugin keymap */
        switch (code) {
        case 23: case 66: cc_cmd(c, val ? C_SELECT_DOWN : C_SELECT_UP); break;
        case 19: if (val) cc_cmd(c, C_UP); break;
        case 20: if (val) cc_cmd(c, C_DOWN); break;
        case 21: if (val) cc_cmd(c, C_LEFT); break;
        case 22: if (val) cc_cmd(c, C_RIGHT); break;
        case 4:  if (val) cc_cmd(c, C_BACK); break;
        case 3:  if (val) cc_cmd(c, C_HOME); break;
        case 85: if (val) cc_cmd(c, C_PLAYPAUSE); break;
        case 87: if (val) cc_cmd(c, C_NEXT); break;
        case 88: if (val) cc_cmd(c, C_PREV); break;
        default: break;
        }
    }
}

static int cc_video(cc_t *c, const unsigned char *p, unsigned n)
{
    unsigned w, h, len;
    unsigned char *b;
    unsigned long long now;
    int i, rc;
    if (n <= 20) return 0;
    w = get32(p); h = get32(p + 4);
    len = n - 20;
    if (!c->vopen) {
        if (!w || !h) { w = c->w; h = c->h; }
        if (av_open(c->lg, w, h)) { c->lg("CCPA video: av_open %ux%u failed", w, h); return -1; }
        c->vopen = 1;
        c->lg("CCPA video open %ux%u flags=0x%x", w, h, get32(p + 8));
        cc_cmd(c, C_FRAME);
        c->last_frame_req = mono_ms();
    }
    b = malloc(len + 8);
    if (!b) return -1;
    now = mono_ms() * 1000ULL;
    for (i = 0; i < 8; ++i) b[i] = (unsigned char)(now >> (56 - 8 * i));
    memcpy(b + 8, p + 20, len);
    rc = av_push(0, b, len + 8);
    free(b);
    if (rc) {
        /* typically P-frames before the first IDR: ask for a key frame (rate limited) */
        ++c->vdrop;
        if (mono_ms() - c->last_frame_req > 500) { cc_cmd(c, C_FRAME); c->last_frame_req = mono_ms(); }
        return 0;
    }
    ++c->vframes; c->vbytes += len;
    if (c->vframes % 60 == 1)
        c->lg("CCPA video frames=%lu bytes=%lu dropped=%lu", c->vframes, c->vbytes, c->vdrop);
    return 0;
}

static void cc_video_close(cc_t *c)
{
    if (!c->vopen) return;
    av_close();
    c->vopen = 0;
    c->lg("CCPA video closed (frames=%lu)", c->vframes);
}

/* AudioData: decodeType u32 | volume f32 | audioType u32 | (1 byte command | 4 byte
 * volume duration | S16LE PCM) */
static void cc_audio(cc_t *c, const unsigned char *p, unsigned n)
{
    unsigned dt, at;
    int ch;
    if (n < 12) return;
    dt = get32(p); at = get32(p + 8);
    if (n == 13) {
        int cmd = (signed char)p[12];
        c->lg("CCPA audio cmd=%d decode=%u type=%u", cmd, dt, at);
        if (!c->usbaudio) return;
        ch = (cmd == 10 || cmd == 11 || cmd == 1 || cmd == 2) ? 0 : 1;
        if (cmd == 1 || cmd == 10 || cmd == 6 || cmd == 12 || cmd == 4 || cmd == 8) ar_active(ch, 1);
        if (cmd == 2 || cmd == 11 || cmd == 7 || cmd == 13 || cmd == 5 || cmd == 9) ar_active(ch, 0);
        return;
    }
    if (n == 16 || !c->usbaudio) return;
    /* 48k/44.1k stereo -> media ring 0, everything else (16k/8k/24k voice) -> ring 1 */
    ch = (dt == 1 || dt == 2 || dt == 4) ? 0 : 1;
    if (g_ar && ch == 0) g_ar->codec0 = 1;
    ar_put(ch, p + 12, n - 12);
}

static void cc_log_text(cc_t *c, const char *tag, const unsigned char *p, unsigned n)
{
    char b[200];
    unsigned i, k = n < sizeof b - 1 ? n : (unsigned)sizeof b - 1;
    for (i = 0; i < k; ++i) b[i] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '.';
    b[k] = 0;
    c->lg("CCPA %s: %s", tag, b);
}

int ccpa_session(aa_transport_t *t, aa_log_fn lg, aa_status_fn st, char *summary, size_t sn)
{
    cc_t cc, *c = &cc;
    unsigned char hdr[16];
    int stage = 0, bye = 0, errs = 0;
    memset(c, 0, sizeof cc);
    c->t = t; c->lg = lg; c->st = st;
    cc_config(c);
    if (c->usbaudio) ar_open(c);
    snprintf(summary, sn, "dongle not answering");
    if (cc_start(c)) { snprintf(summary, sn, "CCPA open failed (USB write)"); return 0; }
    stage = 1;
    st("CarPlay dongle: waiting for phone");
    snprintf(summary, sn, "dongle open, no phone");
    c->last_hb = mono_ms();
    while (!bye) {
        unsigned len, type;
        int r;
        unsigned long long now = mono_ms();
        if (now - c->last_hb >= 2000) { if (cc_send(c, T_HEARTBEAT, 0, 0)) break; c->last_hb = now; }
        if (!c->wifi_connect_sent && now - c->opened_at >= 1000) { cc_cmd(c, C_WIFI_CONNECT); c->wifi_connect_sent = 1; }
        /* CarPlay needs a periodic key frame request (node-CarPlay: every 5 s) and the
           DSP reader may attach late (av_native_want_idr) */
        if (c->vopen && ((c->phone == P_CARPLAY && now - c->last_frame_req >= 5000) || av_native_want_idr())) {
            cc_cmd(c, C_FRAME); c->last_frame_req = now;
        }
        if (av_failed()) { snprintf(summary, sn, "video backend failure (see log)"); break; }
        cc_input(c, &bye);
        r = cc_read(c, hdr, 16, 100);
        if (r == 0) continue;
        if (r < 0) { snprintf(summary, sn, "USB read error (dongle removed?)"); break; }
        if (get32(hdr) != CCPA_MAGIC || get32(hdr + 12) != ~get32(hdr + 8)) {
            c->lg("CCPA bad header %08x %08x %08x %08x", get32(hdr), get32(hdr + 4), get32(hdr + 8), get32(hdr + 12));
            if (++errs >= 5) { snprintf(summary, sn, "protocol desync"); break; }
            continue;
        }
        len = get32(hdr + 4); type = get32(hdr + 8);
        if (len > CCPA_MAXPAY) { c->lg("CCPA oversize type=0x%x len=%u", type, len); snprintf(summary, sn, "oversize message"); break; }
        if (len > c->paycap) {
            unsigned char *nb = realloc(c->pay, len);
            if (!nb) { snprintf(summary, sn, "out of memory"); break; }
            c->pay = nb; c->paycap = len;
        }
        if (len && cc_read(c, c->pay, len, 3000) != (int)len) { snprintf(summary, sn, "USB read error in payload"); break; }
        switch (type) {
        case T_VIDEO:
            if (cc_video(c, c->pay, len)) { snprintf(summary, sn, "video adapter error"); bye = 2; break; }
            if (c->vframes && stage < 3) { stage = 3; st(c->phone == P_ANDROIDAUTO ? "Android Auto (dongle) running" : "CarPlay running"); }
            break;
        case T_AUDIO: cc_audio(c, c->pay, len); break;
        case T_PLUGGED:
            c->phone = len >= 4 ? (int)get32(c->pay) : 0;
            c->lg("CCPA plugged phoneType=%d wifi=%d", c->phone, len >= 8 ? (int)get32(c->pay + 4) : -1);
            st(c->phone == P_CARPLAY ? "CarPlay: phone connected" :
               c->phone == P_ANDROIDAUTO ? "Android Auto (dongle): phone connected" : "Dongle: phone connected");
            if (stage < 2) stage = 2;
            break;
        case T_UNPLUGGED:
            c->lg("CCPA unplugged");
            c->phone = 0;
            cc_video_close(c);
            st("CarPlay dongle: waiting for phone");
            break;
        case T_PHASE: c->lg("CCPA phase=%u", len >= 4 ? get32(c->pay) : 0); break;
        case T_COMMAND: c->lg("CCPA rx command=%u", len >= 4 ? get32(c->pay) : 0); break;
        case T_OPEN: c->lg("CCPA opened %ux%u fps=%u", len >= 12 ? get32(c->pay) : 0,
                           len >= 12 ? get32(c->pay + 4) : 0, len >= 12 ? get32(c->pay + 8) : 0); break;
        case T_SWVERSION: cc_log_text(c, "software", c->pay, len); break;
        case T_BOXSETTINGS: cc_log_text(c, "box", c->pay, len); break;
        case T_MANUFACTURER: c->lg("CCPA manufacturer %u %u", len >= 8 ? get32(c->pay) : 0, len >= 8 ? get32(c->pay + 4) : 0); break;
        case T_MEDIA: break;   /* now-playing metadata / album art: not shown yet */
        default: c->lg("CCPA rx type=0x%02x len=%u", type, len); break;
        }
    }
    cc_video_close(c);
    if (g_ar) { ar_active(0, 0); ar_active(1, 0); }
    free(c->pay);
    if (bye == 1) snprintf(summary, sn, "disconnected by user (video frames %lu)", c->vframes);
    lg("CCPA session end stage=%d: %s", stage, summary);
    return stage;
}
