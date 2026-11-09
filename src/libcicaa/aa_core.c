/* Android Auto head-unit protocol core v13 (isolated adapter trial) - see aa_core.h. mbedTLS 3.6.7.
 *
 * Message layer as implemented by the open-source aasdk/openauto head units [I]:
 *  - Each frame payload is encrypted separately (TLS application data) once
 *    AUTH_COMPLETE is sent; multi-frame messages are FIRST (+u32 total) /
 *    MIDDLE / LAST, decrypted per frame and concatenated.
 *  - flags 0x04 (CONTROL) marks channel-control messages (channel open) on
 *    service channels; messages on channel 0 do not use it.
 * Channels declared here (openauto numbering): 1 input, 2 sensor, 3 video.
 * PCM media/speech/system channels use a bounded diagnostic sink (no speakers).
 * Video payloads are written raw to the dump file (Annex-B H.264 expected).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include "aa_core.h"
#include "aa_video.h"
#include "aap_paths.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"

enum { CH_CONTROL = 0, CH_INPUT = 1, CH_SENSOR = 2, CH_VIDEO = 3 };
enum { F_FIRST = 1, F_LAST = 2, F_COMPLETE = 3, F_CONTROL = 4, F_ENC = 8 };
enum { M_VER_REQ = 1, M_VER_RESP = 2, M_SSL = 3, M_AUTH = 4, M_SD_REQ = 5, M_SD_RESP = 6,
       M_CH_OPEN_REQ = 7, M_CH_OPEN_RESP = 8, M_PING_REQ = 0x0b, M_PING_RESP = 0x0c,
       M_NAV_FOCUS_REQ = 0x0d, M_NAV_FOCUS_RESP = 0x0e, M_SHUTDOWN_REQ = 0x0f, M_SHUTDOWN_RESP = 0x10,
       M_AUDIO_FOCUS_REQ = 0x12, M_AUDIO_FOCUS_RESP = 0x13 };
/* service-channel message ids */
enum { AV_MEDIA_TS = 0x0000, AV_MEDIA = 0x0001, AV_SETUP_REQ = 0x8000, AV_START = 0x8001,
       AV_STOP = 0x8002, AV_SETUP_RESP = 0x8003, AV_ACK = 0x8004, AV_VIDEO_FOCUS_REQ = 0x8007,
       AV_VIDEO_FOCUS_IND = 0x8008,
       SENSOR_START_REQ = 0x8001, SENSOR_START_RESP = 0x8002, SENSOR_EVENT = 0x8003,
       INPUT_BINDING_REQ = 0x8002, INPUT_BINDING_RESP = 0x8003 };

#define RXCAP 262144
typedef struct {
    aa_transport_t *t;
    aa_log_fn lg;
    mbedtls_ssl_context *ssl;       /* set after TLS: frames then encrypted */
    int enc;
    unsigned char rx[RXCAP]; size_t rxn;
    unsigned char msg[RXCAP]; size_t msgn;     /* reassembled (decrypted) message */
    int msg_ch, msg_flags;
    unsigned char tin[65536]; size_t tin_r, tin_w;
    unsigned char tout[65536]; size_t tout_n;
    /* video */
    int vfd; unsigned long vbytes, vframes; int vsession;
    int cfd; unsigned long cbytes;   /* v26 CAP1 framed capture */
} aa_t;

static aa_t g_aa;

static const char *ctlname(int id)
{
    switch (id) {
    case 1: return "VERSION_REQUEST"; case 2: return "VERSION_RESPONSE";
    case 3: return "SSL_HANDSHAKE"; case 4: return "AUTH_COMPLETE";
    case 5: return "SERVICE_DISCOVERY_REQUEST"; case 6: return "SERVICE_DISCOVERY_RESPONSE";
    case 7: return "CHANNEL_OPEN_REQUEST"; case 8: return "CHANNEL_OPEN_RESPONSE";
    case 0x0b: return "PING_REQUEST"; case 0x0c: return "PING_RESPONSE";
    case 0x0d: return "NAVIGATION_FOCUS_REQUEST"; case 0x0f: return "SHUTDOWN_REQUEST";
    case 0x11: return "VOICE_SESSION_REQUEST"; case 0x12: return "AUDIO_FOCUS_REQUEST";
    default: return "?";
    }
}

static void hexline(aa_t *a, const char *tag, const unsigned char *p, size_t n)
{
    char h[3 * 48 + 1];
    size_t i, m = n < 48 ? n : 48;
    for (i = 0; i < m; ++i) snprintf(h + 3 * i, 4, "%02x ", p[i]);
    h[3 * m] = 0;
    a->lg("%s len=%u: %s%s", tag, (unsigned)n, h, n > m ? "..." : "");
}

/* ---------------- protobuf writer ---------------- */
typedef struct { unsigned char b[2048]; size_t n; } pb_t;
static void pb_var(pb_t *p, unsigned long v) { do { unsigned char c = v & 0x7f; v >>= 7; p->b[p->n++] = c | (v ? 0x80 : 0); } while (v); }
static void pb_u(pb_t *p, int f, unsigned long v) { pb_var(p, (unsigned long)f << 3); pb_var(p, v); }
static void pb_bytes(pb_t *p, int f, const void *d, size_t n) { pb_var(p, ((unsigned long)f << 3) | 2); pb_var(p, n); memcpy(p->b + p->n, d, n); p->n += n; }
static void pb_str(pb_t *p, int f, const char *s) { pb_bytes(p, f, s, strlen(s)); }
static void pb_msg(pb_t *p, int f, const pb_t *m) { pb_bytes(p, f, m->b, m->n); }

/* ---------------- TLS BIO ---------------- */
static int bio_send(void *ctx, const unsigned char *b, size_t n)
{
    aa_t *a = (aa_t *)ctx;
    if (a->tout_n + n > sizeof a->tout) n = sizeof a->tout - a->tout_n;
    if (!n) return MBEDTLS_ERR_SSL_WANT_WRITE;
    memcpy(a->tout + a->tout_n, b, n);
    a->tout_n += n;
    return (int)n;
}
static int bio_recv(void *ctx, unsigned char *b, size_t n)
{
    aa_t *a = (aa_t *)ctx;
    size_t av = a->tin_w - a->tin_r;
    if (!av) return MBEDTLS_ERR_SSL_WANT_READ;
    if (n > av) n = av;
    memcpy(b, a->tin + a->tin_r, n);
    a->tin_r += n;
    if (a->tin_r == a->tin_w) a->tin_r = a->tin_w = 0;
    return (int)n;
}
static void tin_push(aa_t *a, const unsigned char *p, size_t n)
{
    if (a->tin_r) { memmove(a->tin, a->tin + a->tin_r, a->tin_w - a->tin_r); a->tin_w -= a->tin_r; a->tin_r = 0; }
    if (a->tin_w + n > sizeof a->tin) n = sizeof a->tin - a->tin_w;
    memcpy(a->tin + a->tin_w, p, n);
    a->tin_w += n;
}

/* ---------------- frame I/O ---------------- */
static int put_frame(aa_t *a, int ch, int flags, const unsigned char *p, size_t n)
{
    unsigned char f[4 + 20000];
    if (n > 20000) return -1;
    f[0] = (unsigned char)ch; f[1] = (unsigned char)flags;
    f[2] = (unsigned char)(n >> 8); f[3] = (unsigned char)n;
    memcpy(f + 4, p, n);
    return a->t->send(a->t->ctx, f, (int)(4 + n)) == (int)(4 + n) ? 0 : -1;
}

/* send msgid+body; encrypted after AUTH (single frame, body <= 16 KB) */
static int send_msg(aa_t *a, int ch, int ctrl, int msgid, const unsigned char *body, size_t n)
{
    unsigned char m[16384 + 2];
    int flags = F_COMPLETE | (ctrl ? F_CONTROL : 0);
    if (n > 16384) return -1;
    m[0] = (unsigned char)(msgid >> 8); m[1] = (unsigned char)msgid;
    if (n) memcpy(m + 2, body, n);
    if (!a->enc) return put_frame(a, ch, flags, m, n + 2);
    {
        int w = mbedtls_ssl_write(a->ssl, m, n + 2);
        if (w != (int)(n + 2)) { a->lg("AA ssl_write %d", w); return -1; }
        w = put_frame(a, ch, flags | F_ENC, a->tout, a->tout_n);
        a->tout_n = 0;
        return w;
    }
}

#include "aa_audio.h"

/* Read one complete message, decrypting per frame. 1 ok, 0 timeout, <0 err */
static int read_msg(aa_t *a, int timeout_ms)
{
    int waited = 0;
    a->msgn = 0;
    for (;;) {
        while (a->rxn >= 4) {
            int ch = a->rx[0], fl = a->rx[1];
            size_t sz = ((size_t)a->rx[2] << 8) | a->rx[3];
            size_t hdr = ((fl & F_COMPLETE) == F_FIRST) ? 8 : 4;
            if (a->rxn < hdr + sz) break;
            if (fl & F_ENC) {
                if (!a->ssl) { a->lg("AA encrypted frame before TLS"); return -3; }
                tin_push(a, a->rx + hdr, sz);
                for (;;) {
                    int r = mbedtls_ssl_read(a->ssl, a->msg + a->msgn, sizeof a->msg - a->msgn);
                    if (r > 0) { a->msgn += (size_t)r; continue; }
                    if (r == MBEDTLS_ERR_SSL_WANT_READ) break;
                    { char e[96]; mbedtls_strerror(r, e, sizeof e); a->lg("AA decrypt -0x%04x %s", -r, e); }
                    return -4;
                }
            } else {
                if (a->msgn + sz > sizeof a->msg) { a->lg("AA rx overflow"); return -2; }
                memcpy(a->msg + a->msgn, a->rx + hdr, sz);
                a->msgn += sz;
            }
            memmove(a->rx, a->rx + hdr + sz, a->rxn - hdr - sz);
            a->rxn -= hdr + sz;
            a->msg_ch = ch; a->msg_flags = fl;
            if (fl & F_LAST) return 1;
        }
        if (waited >= timeout_ms) return 0;
        {
            int r = a->t->recv(a->t->ctx, a->rx + a->rxn, (int)(sizeof a->rx - a->rxn), 100);
            if (r < 0) { a->lg("AA transport recv error %d", r); return -1; }
            if (r == 0) waited += 100; else a->rxn += (size_t)r;
        }
    }
}

static int verify_any(void *p, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    aa_t *a = (aa_t *)p;
    char subj[128];
    mbedtls_x509_dn_gets(subj, sizeof subj, &crt->subject);
    a->lg("AA phone cert depth=%d flags=0x%x subject=%s", depth, (unsigned)*flags, subj);
    *flags = 0;
    return 0;
}

extern int cicaa_entropy(void *d, unsigned char *o, size_t n, size_t *olen);

/* ---------------- service discovery response ---------------- */
/* ---- v49: input + projection control from the plugin (any thread) ----
 * Only the AA session thread writes to TLS: plugin calls enqueue, the main
 * loop drains every <=100 ms. kind 1 = key (value 1 down / 0 up),
 * kind 2 = relative (rotary, value = signed steps). */
#include <pthread.h>
static pthread_mutex_t g_inmu = PTHREAD_MUTEX_INITIALIZER;
static struct { int kind, code, val; } g_inq[32];
static unsigned g_inw, g_inr;
static volatile int g_proj = 1;        /* 1 projected (AA on screen), 0 native */
static volatile int g_proj_req;        /* 1 = plugin asks projection back */
int cicaa_input(int kind, int code, int value)
{
    int ok = 0;
    pthread_mutex_lock(&g_inmu);
    if (g_inw - g_inr < 32) { unsigned i = g_inw++ % 32; g_inq[i].kind = kind; g_inq[i].code = code; g_inq[i].val = value; ok = 1; }
    pthread_mutex_unlock(&g_inmu);
    return ok ? 0 : -1;
}
int cicaa_projection(void) { return g_proj; }
void cicaa_project(void) { g_proj_req = 1; }
static void pb_s32(pb_t *p, int f, int v)          /* int32 varint, negative = 10 bytes */
{
    unsigned long lo = (unsigned long)v; int i;
    pb_var(p, (unsigned long)f << 3);
    if (v >= 0) { pb_var(p, lo); return; }
    for (i = 0; i < 4; ++i) { p->b[p->n++] = (unsigned char)((lo & 0x7f) | 0x80); lo >>= 7; }
    /* bits 28..31 of v plus sign-extension ones */
    p->b[p->n++] = (unsigned char)((lo & 0x0f) | 0x70 | 0x80);
    for (i = 0; i < 4; ++i) p->b[p->n++] = 0xff;
    p->b[p->n++] = 0x01;
}
static unsigned long long mono_us(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000ULL + (unsigned long long)t.tv_nsec / 1000ULL;
}
static void input_drain(aa_t *a);
static int send_sd_response(aa_t *a)
{
    pb_t sd, ch, sub, cfg, item;
    memset(&sd, 0, sizeof sd);
    /* channel 2: sensors DRIVING_STATUS(13) + NIGHT_DATA(10) */
    memset(&ch, 0, sizeof ch); memset(&sub, 0, sizeof sub);
    memset(&item, 0, sizeof item); pb_u(&item, 1, 13); pb_msg(&sub, 1, &item);
    memset(&item, 0, sizeof item); pb_u(&item, 1, 10); pb_msg(&sub, 1, &item);
    pb_u(&ch, 1, CH_SENSOR); pb_msg(&ch, 2, &sub); pb_msg(&sd, 1, &ch);
    /* channel 3: video H.264 800x480, 30 fps (v19), dpi 140 */
    memset(&ch, 0, sizeof ch); memset(&sub, 0, sizeof sub); memset(&cfg, 0, sizeof cfg);
    /* v19: field 2 frame_rate 2 = 30 fps (1 = 60 fps, what v13-v18 requested). */
    /* v47: margin_width (default 80, file margin_w.txt overrides): DSP shows only 720 px. */
    pb_u(&cfg, 1, 1); pb_u(&cfg, 2, 2); pb_u(&cfg, 3, AAP_MARGIN_W); pb_u(&cfg, 4, AAP_MARGIN_H); pb_u(&cfg, 5, 140);
    pb_u(&sub, 1, 3 /* VIDEO */); pb_msg(&sub, 4, &cfg); pb_u(&sub, 5, 1);
    pb_u(&ch, 1, CH_VIDEO); pb_msg(&ch, 3, &sub); pb_msg(&sd, 1, &ch);
    /* channel 1: input - rotary/DPAD keycodes, touch 800x480 */
    memset(&ch, 0, sizeof ch); memset(&sub, 0, sizeof sub); memset(&cfg, 0, sizeof cfg);
    {   static const unsigned kc[] = {3, 4, 19, 20, 21, 22, 23, 65536, 84, 85, 87, 88, 126, 127};
        unsigned i; for (i = 0; i < sizeof kc / sizeof kc[0]; ++i) pb_u(&sub, 1, kc[i]); }
    /* v57b: no touchscreen_config -> AA uses rotary (non-touch) UI. */
    pb_u(&ch, 1, CH_INPUT); pb_msg(&ch, 4, &sub); pb_msg(&sd, 1, &ch);
    audio_discovery(&sd);
    pb_str(&sd, 2, "CIC");            /* head_unit_name */
    pb_str(&sd, 3, "BMW CIC");        /* car_model */
    pb_str(&sd, 4, "2010");           /* car_year */
    pb_str(&sd, 5, "0000000001");     /* car_serial */
    pb_u(&sd, 6, 1);                  /* left_hand_drive */
    pb_str(&sd, 7, "CIC lab");        /* headunit_manufacturer */
    pb_str(&sd, 8, "CIC HIGH");       /* headunit_model */
    pb_str(&sd, 9, "1");              /* sw_build */
    pb_str(&sd, 10, "1.0");           /* sw_version */
    pb_u(&sd, 11, 0);
    pb_u(&sd, 12, 0);
    if (send_msg(a, CH_CONTROL, 0, M_SD_RESP, sd.b, sd.n)) {
        a->lg("AA SERVICE_DISCOVERY_RESPONSE send failed");return -1;
    }
    a->lg("AA tx SERVICE_DISCOVERY_RESPONSE %u bytes (input/sensor/video + %s%sspeech/system + mic descriptor; capture unavailable)", (unsigned)sd.n, g_usb_media ? (g_aac ? "AAC-LC media(v56)/" : "PCM media/") : "", "PCM ");
    a->lg("V17 BT1 media %s", g_usb_media ? "over USB ch4 -> DSP (default v54)" : "NOT announced (usb_media.off): phone uses Bluetooth A2DP");
    return 0;
}

static void send_sensor_events(aa_t *a)
{
    pb_t ev, s;
    memset(&ev, 0, sizeof ev); memset(&s, 0, sizeof s);
    pb_u(&s, 1, 0);                    /* driving status UNRESTRICTED (parked) */
    pb_msg(&ev, 13, &s);
    send_msg(a, CH_SENSOR, 0, SENSOR_EVENT, ev.b, ev.n);
    memset(&ev, 0, sizeof ev); memset(&s, 0, sizeof s);
    pb_u(&s, 1, 0);                    /* is_night = false */
    pb_msg(&ev, 10, &s);
    send_msg(a, CH_SENSOR, 0, SENSOR_EVENT, ev.b, ev.n);
    a->lg("AA tx sensor events (driving=parked, night=false)");
}

static int on_video(aa_t *a, int id, const unsigned char *p, size_t n)
{
    size_t off = (id == AV_MEDIA_TS) ? 8 : 0;   /* u64 timestamp first */
    if (n < off || av_push(id, p, n)) {
        a->lg("V13 VIDEO_REJECTED id=%d bytes=%u; no ACK",id,(unsigned)n);
        return -1;
    }
    if (a->vfd >= 0 && a->vbytes < 16UL * 1024 * 1024) {
        write(a->vfd, p + off, n - off);
        a->vbytes += n - off;
    }
    /* v26 CAP1: one record per AA message, keeps boundaries + timestamp.
       record = 'V' id(1) ts(8, raw BE, 0 if none) len(4 BE) payload */
    if (a->cfd >= 0 && a->cbytes < 32UL * 1024 * 1024) {
        unsigned char h[14]; size_t len = n - off;
        memset(h, 0, sizeof h);
        h[0] = 'V'; h[1] = (unsigned char)id;
        if (off) memcpy(h + 2, p, 8);
        h[10] = (unsigned char)(len >> 24); h[11] = (unsigned char)(len >> 16);
        h[12] = (unsigned char)(len >> 8);  h[13] = (unsigned char)len;
        if (write(a->cfd, h, sizeof h) == (int)sizeof h && write(a->cfd, p + off, len) == (int)len)
            a->cbytes += sizeof h + len;
        else { close(a->cfd); a->cfd = -1; }
    }
    if (a->vframes < 3) hexline(a, "AA video", p + off, n - off);
    ++a->vframes;
    if (a->vframes % 30 == 1)
        a->lg("AA video frames=%lu bytes=%lu", a->vframes, a->vbytes);
    {   pb_t ack; memset(&ack, 0, sizeof ack);
        pb_u(&ack, 1, (unsigned long)a->vsession); pb_u(&ack, 2, 1);
        if (send_msg(a, CH_VIDEO, 0, AV_ACK, ack.b, ack.n)) return -1; }
    /* v47 LIVE1: reader (cicaalive) attached late -> force a fresh IDR by
       toggling video focus native(2) -> projected(1). */
    if (av_native_want_idr()) {
        pb_t f; memset(&f, 0, sizeof f); pb_u(&f, 1, 2); pb_u(&f, 2, 0);
        send_msg(a, CH_VIDEO, 0, AV_VIDEO_FOCUS_IND, f.b, f.n);
        memset(&f, 0, sizeof f); pb_u(&f, 1, 1); pb_u(&f, 2, 0);
        send_msg(a, CH_VIDEO, 0, AV_VIDEO_FOCUS_IND, f.b, f.n);
        a->lg("V47 IDR request: video focus toggled for late reader");
    }
    return 0;
}

/* dispatch one decrypted message; returns 1 to stop */
/* v49: send queued iDrive input as InputEventIndication (msg 0x8001, ch 1):
 * {1 timestamp_us, 4 KeyEvent{1 Key{1 keycode, 2 down, 3 meta, 4 longpress}},
 *  6 RelativeEvent{1 Rel{1 keycode, 2 delta}}}. Also projection re-request. */
static void input_drain(aa_t *a)
{
    for (;;) {
        int kind, code, val; pb_t ev, outer, item; unsigned long long ts;
        pthread_mutex_lock(&g_inmu);
        if (g_inr == g_inw) { pthread_mutex_unlock(&g_inmu); break; }
        { unsigned i = g_inr++ % 32; kind = g_inq[i].kind; code = g_inq[i].code; val = g_inq[i].val; }
        pthread_mutex_unlock(&g_inmu);
        memset(&ev, 0, sizeof ev); memset(&outer, 0, sizeof outer); memset(&item, 0, sizeof item);
        ts = mono_us();
        pb_var(&ev, (1UL << 3)); pb_var(&ev, (unsigned long)(ts & 0xffffffffUL));   /* 32-bit us, wraps after 71 min */
        pb_u(&item, 1, (unsigned long)code);
        if (kind == 1) { pb_u(&item, 2, val ? 1 : 0); pb_u(&item, 3, 0); pb_u(&item, 4, 0); pb_msg(&outer, 1, &item); pb_msg(&ev, 4, &outer); }
        else { pb_s32(&item, 2, val); pb_msg(&outer, 1, &item); pb_msg(&ev, 6, &outer); }
        send_msg(a, CH_INPUT, 0, 0x8001, ev.b, ev.n);
        a->lg("V49 input kind=%d code=%d val=%d", kind, code, val);
    }
    if (g_proj_req) {
        pb_t f; g_proj_req = 0; g_proj = 1;
        memset(&f, 0, sizeof f); pb_u(&f, 1, 1); pb_u(&f, 2, 0);
        send_msg(a, CH_VIDEO, 0, AV_VIDEO_FOCUS_IND, f.b, f.n);
        a->lg("V49 projection requested by plugin -> focus PROJECTED");
    }
}
static int dispatch(aa_t *a, int *stage, char *summary, size_t sn)
{
    int ch = a->msg_ch, id;
    const unsigned char *b;
    size_t n;
    pb_t r;
    if (a->msgn < 2) return 0;
    id = (a->msg[0] << 8) | a->msg[1];
    b = a->msg + 2; n = a->msgn - 2;
    memset(&r, 0, sizeof r);
    if (!((ch == CH_VIDEO || (ch >= 4 && ch <= 6)) &&
          (id == AV_MEDIA || id == AV_MEDIA_TS)))
        a->lg("AA rx ch=%d flags=0x%02x msg=0x%04x %s len=%u", ch, a->msg_flags, id,
              ch == CH_CONTROL || (a->msg_flags & F_CONTROL) ? ctlname(id) : "service", (unsigned)n);
    if ((a->msg_flags & F_CONTROL) && id == M_CH_OPEN_REQ) {     /* channel open on service channel */
        if (ch >= 4 && ch <= 6 && audio_open(a,ch)) {
            pb_u(&r,1,1);send_msg(a,ch,1,M_CH_OPEN_RESP,r.b,r.n);return 1;
        }
        pb_u(&r, 1, 0);                                         /* STATUS OK */
        send_msg(a, ch, 1, M_CH_OPEN_RESP, r.b, r.n);
        a->lg("AA channel %d opened", ch);
        if (ch == CH_SENSOR) send_sensor_events(a);
        return 0;
    }
    if (ch == CH_CONTROL) {
        switch (id) {
        case M_SD_REQ:
            hexline(a, "AA SD request", b, n);
            if (send_sd_response(a)) return 1;
            if (*stage < 4) *stage = 4;
            snprintf(summary, sn, "SD respondido; esperando canales");
            return 0;
        case M_PING_REQ:
            pb_u(&r, 1, 0);   /* timestamp */
            send_msg(a, CH_CONTROL, 0, M_PING_RESP, r.b, r.n);
            return 0;
        case M_AUDIO_FOCUS_REQ: {
            unsigned long focus;int present;unsigned state;
            if(audio_uint(b,n,1,&focus,&present) || !present || focus<1 || focus>4) return 1;
            /* RELEASE=4 -> LOSS=3, GAIN_TRANSIENT=2, GAIN_NAVI=3 -> guidance=7. */
            state=focus==4?3:focus==3?7:(unsigned)focus;
            pb_u(&r,1,state);
            a->lg("AA audio focus request=%lu state=%u",focus,state);
            return send_msg(a,CH_CONTROL,0,M_AUDIO_FOCUS_RESP,r.b,r.n)!=0;
        }
        case M_NAV_FOCUS_REQ:
            pb_u(&r, 1, 2);   /* PROJECTED */
            send_msg(a, CH_CONTROL, 0, M_NAV_FOCUS_RESP, r.b, r.n);
            return 0;
        case M_SHUTDOWN_REQ:
            send_msg(a, CH_CONTROL, 0, M_SHUTDOWN_RESP, 0, 0);
            a->lg("AA phone requested shutdown");
            return 1;
        default:
            hexline(a, "AA ctrl body", b, n);
            return 0;
        }
    }
    if (ch >= 4 && ch <= 6) return audio_dispatch(a,ch,id,b,n);
    if (ch == 7) return microphone_dispatch(a,id,b,n);
    if (ch == CH_SENSOR && id == SENSOR_START_REQ) {
        pb_u(&r, 1, 0);
        send_msg(a, CH_SENSOR, 0, SENSOR_START_RESP, r.b, r.n);
        send_sensor_events(a);
        return 0;
    }
    if (ch == CH_INPUT && id == INPUT_BINDING_REQ) {
        pb_u(&r, 1, 0);
        send_msg(a, CH_INPUT, 0, INPUT_BINDING_RESP, r.b, r.n);
        if (*stage < 5) *stage = 5;
        return 0;
    }
    if (ch == CH_VIDEO) {
        if (id == AV_SETUP_REQ) {
            hexline(a, "AA video setup", b, n);
            pb_u(&r, 1, 2);   /* MEDIA_STATUS READY [I] */
            pb_u(&r, 2, 1);   /* max_unacked */
            pb_u(&r, 3, 0);   /* configs: index 0 */
            send_msg(a, CH_VIDEO, 0, AV_SETUP_RESP, r.b, r.n);
            memset(&r, 0, sizeof r);
            pb_u(&r, 1, 1);   /* VIDEO_FOCUS_MODE FOCUSED */
            pb_u(&r, 2, 0);   /* unrequested=false */
            send_msg(a, CH_VIDEO, 0, AV_VIDEO_FOCUS_IND, r.b, r.n);
            a->lg("AA video setup answered + focus given");
            return 0;
        }
        if (id == AV_START) {
            hexline(a, "AA video start", b, n);
            /* field 1 = session id */
            if (n >= 2 && b[0] == 0x08) a->vsession = b[1] & 0x7f;
            if (av_open(a->lg, 800, 480)) { snprintf(summary,sn,"V13 adaptador no disponible"); return 1; }
            if (*stage < 6) *stage = 6;
            snprintf(summary, sn, "VIDEO START sesion %d", a->vsession);
            return 0;
        }
        if (id == AV_STOP) { av_close(); a->lg("AA video stop + V13 adapter closed"); return 0; }
        if (id == AV_VIDEO_FOCUS_REQ) {
            /* v49: VideoFocusRequest {1 disp, 2 mode, 3 reason}; mode 2 = NATIVE =
             * user pressed the AA "exit to car" button. */
            unsigned long mode = 0; int has = 0;
            hexline(a, "AA video focus request", b, n);
            if (audio_uint(b, n, 2, &mode, &has) || !has) mode = 1;
            g_proj = (mode == 2) ? 0 : 1;
            pb_u(&r, 1, g_proj ? 1 : 2); pb_u(&r, 2, 0);
            send_msg(a, CH_VIDEO, 0, AV_VIDEO_FOCUS_IND, r.b, r.n);
            a->lg("V49 video focus -> %s", g_proj ? "PROJECTED" : "NATIVE (exit to car)");
            return 0;
        }
        if (id == AV_MEDIA || id == AV_MEDIA_TS) {
            if (on_video(a, id, b, n)) { snprintf(summary,sn,"V13 error adaptador video (ver log)"); return 1; }
            if (*stage < 7) *stage = 7;
            snprintf(summary, sn, "VIDEO H.264 recibido: %lu tramas, %lu bytes", a->vframes, a->vbytes);
            return 0;
        }
    }
    hexline(a, "AA unhandled body", b, n);
    return 0;
}

int aa_session(aa_transport_t *t, const aa_pki_t *pki, aa_log_fn lg, aa_status_fn st,
               char *summary, size_t sn)
{
    aa_t *a = &g_aa;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context rng;
    mbedtls_x509_crt crt;
    mbedtls_pk_context key;
    mbedtls_ssl_config cfg;
    mbedtls_ssl_context ssl;
    unsigned char body[8];
    int stage = 0, rc, i, idle = 0;
    char e[100];

    memset(a, 0, sizeof *a);
    audio_reset();
    a->t = t; a->lg = lg; a->vfd = -1; a->cfd = -1;
    mbedtls_entropy_init(&ent); mbedtls_ctr_drbg_init(&rng);
    mbedtls_x509_crt_init(&crt); mbedtls_pk_init(&key);
    mbedtls_ssl_config_init(&cfg); mbedtls_ssl_init(&ssl);
    snprintf(summary, sn, "sin respuesta del movil");

    /* 1. version */
    st("AA: VERSION_REQUEST");
    body[0] = 0; body[1] = 1; body[2] = 0; body[3] = 1;
    if (send_msg(a, CH_CONTROL, 0, M_VER_REQ, body, 4)) { lg("AA send version failed"); goto out; }
    rc = read_msg(a, 5000);
    if (rc != 1) { lg("AA no VERSION_RESPONSE rc=%d", rc); goto out; }
    hexline(a, "AA rx", a->msg, a->msgn);
    if (a->msgn >= 8 && ((a->msg[0] << 8) | a->msg[1]) == M_VER_RESP) {
        int maj = (a->msg[2] << 8) | a->msg[3], min = (a->msg[4] << 8) | a->msg[5];
        int stt = (a->msg[6] << 8) | a->msg[7];
        lg("AA VERSION_RESPONSE %d.%d status=%d (%s)", maj, min, stt, stt == 0 ? "MATCH" : "MISMATCH");
        snprintf(summary, sn, "version %d.%d %s", maj, min, stt == 0 ? "OK" : "MISMATCH");
        if (stt != 0) goto out;
        stage = 1;
    } else { lg("AA unexpected message instead of version"); goto out; }

    /* 2. TLS (head unit = client) */
    st("AA: TLS handshake");
    mbedtls_entropy_add_source(&ent, cicaa_entropy, 0, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
    if ((rc = mbedtls_ctr_drbg_seed(&rng, mbedtls_entropy_func, &ent, (const unsigned char *)"cicaa", 5))) { lg("AA drbg -0x%04x", -rc); goto out; }
    if ((rc = mbedtls_x509_crt_parse(&crt, pki->crt, pki->crt_len))) { mbedtls_strerror(rc, e, sizeof e); lg("AA HU cert parse -0x%04x %s", -rc, e); goto out; }
    if ((rc = mbedtls_pk_parse_key(&key, pki->key, pki->key_len, 0, 0, mbedtls_ctr_drbg_random, &rng))) { mbedtls_strerror(rc, e, sizeof e); lg("AA HU key parse -0x%04x %s", -rc, e); goto out; }
    {   char subj[160]; mbedtls_x509_dn_gets(subj, sizeof subj, &crt.subject);
        lg("AA HU cert (%s): %s", pki->origin, subj); }
    mbedtls_ssl_config_defaults(&cfg, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_min_tls_version(&cfg, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&cfg, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_verify(&cfg, verify_any, a);
    mbedtls_ssl_conf_rng(&cfg, mbedtls_ctr_drbg_random, &rng);
    {   static const int suites[] = {MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
                                     MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
                                     MBEDTLS_TLS_RSA_WITH_AES_128_GCM_SHA256,
                                     MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA, 0};
        mbedtls_ssl_conf_ciphersuites(&cfg, suites); }
    if ((rc = mbedtls_ssl_conf_own_cert(&cfg, &crt, &key))) { lg("AA own cert -0x%04x", -rc); goto out; }
    if ((rc = mbedtls_ssl_setup(&ssl, &cfg))) { lg("AA ssl setup -0x%04x", -rc); goto out; }
    mbedtls_ssl_set_bio(&ssl, a, bio_send, bio_recv, 0);
    for (i = 0; i < 60; ++i) {
        rc = mbedtls_ssl_handshake(&ssl);
        if (a->tout_n) {
            lg("AA tx SSL_HANDSHAKE %u bytes", (unsigned)a->tout_n);
            if (send_msg(a, CH_CONTROL, 0, M_SSL, a->tout, a->tout_n)) { lg("AA send ssl failed"); goto out; }
            a->tout_n = 0;
        }
        if (rc == 0) break;
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            mbedtls_strerror(rc, e, sizeof e);
            lg("AA TLS FAIL -0x%04x %s (alerta del movil = cert HU no aceptado?)", -rc, e);
            snprintf(summary, sn, "version OK, TLS fallo -0x%04x", -rc);
            goto out;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
            int r = read_msg(a, 8000);
            if (r != 1) { lg("AA TLS waiting phone rc=%d", r); snprintf(summary, sn, "version OK, TLS sin respuesta"); goto out; }
            if (a->msgn >= 2 && ((a->msg[0] << 8) | a->msg[1]) == M_SSL) {
                lg("AA rx SSL_HANDSHAKE %u bytes", (unsigned)(a->msgn - 2));
                tin_push(a, a->msg + 2, a->msgn - 2);
            } else hexline(a, "AA rx (no SSL) ", a->msg, a->msgn);
        }
    }
    if (rc != 0) { lg("AA TLS did not finish"); goto out; }
    lg("AA TLS_OK %s %s verify=0x%x", mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl),
       (unsigned)mbedtls_ssl_get_verify_result(&ssl));
    stage = 2;
    snprintf(summary, sn, "TLS OK %s", mbedtls_ssl_get_ciphersuite(&ssl));

    /* 3. AUTH_COMPLETE in clear; afterwards everything is encrypted */
    body[0] = 0x08; body[1] = 0x00;
    if (send_msg(a, CH_CONTROL, 0, M_AUTH, body, 2)) goto out;
    lg("AA tx AUTH_COMPLETE");
    stage = 3;
    a->ssl = &ssl;
    a->enc = 1;
#ifdef CICAA_HOST_TEST
    a->vfd = open("aa_video.h264", O_WRONLY | O_CREAT | O_TRUNC | 0x8000, 0644);
#else
    a->vfd = -1;   /* v49: no 16 MB HDD dump during live use */
#endif
#ifdef CICAA_HOST_TEST
    a->cfd = open("aa_video.cap", O_WRONLY | O_CREAT | O_TRUNC | 0x8000, 0644);
#else
    a->cfd = -1;
#endif
    if (a->cfd >= 0 && write(a->cfd, "CICCAP01", 8) == 8) a->cbytes = 8;
    else if (a->cfd >= 0) { close(a->cfd); a->cfd = -1; }
    st("AA: sesion activa");

    /* 4.. main loop: up to 10 min or 60 s of silence */
    g_proj = 1; g_proj_req = 0;
    for (i = 0; idle < 600; ++i) {
        int r;
        if (av_failed()) { snprintf(summary,sn,"V13 fallo backend video (ver log)"); break; }
        input_drain(a);
        r = read_msg(a, 100);
        if (r == 0) { ++idle; continue; }
        if (r < 0) { lg("AA session read rc=%d", r); break; }
        idle = 0;
        if (dispatch(a, &stage, summary, sn)) break;
        if (stage >= 7 && a->vframes % 60 == 0) {
            char m[96];
            snprintf(m, sizeof m, "AA: video %lu tramas %lu KB", a->vframes, a->vbytes / 1024);
            st(m);
        }
    }
out:
    audio_close();
    av_close();
    if (a->vfd >= 0) close(a->vfd);
    if (a->cfd >= 0) close(a->cfd);
    lg("AA stage=%d summary=%s video_frames=%lu video_bytes=%lu cap_bytes=%lu", stage, summary, a->vframes, a->vbytes, a->cbytes);
    a->ssl = 0; a->enc = 0;
    mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&cfg);
    mbedtls_pk_free(&key); mbedtls_x509_crt_free(&crt);
    mbedtls_ctr_drbg_free(&rng); mbedtls_entropy_free(&ent);
    return stage;
}
