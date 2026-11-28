/* nbt_carplay: the AirPlay receiver CarPlay talks to (step b), as xcertplay (GPL-3.0, github.com/shilapi/xcertplay).
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 *
 * After CarPlayStartSession (iAP2 4301: our address, port 7000 and Ed25519 key) the iPhone connects over the NCM link:
 *   GET /info              what we are: screen 1280x480, knob, audio formats, HID devices (binary plist)
 *   POST /pair-setup       SRP-6a (PIN 3939), Ed25519 identities exchanged (TLV8); the iPhone's key is kept
 *   POST /pair-verify      X25519 + Ed25519: the shared secret; from then on the control connection is encrypted
 *                          (ChaCha20-Poly1305 frames {u16 LE length, ciphertext, tag}, counters as nonces)
 *   POST /auth-setup       the car's authentication chip signs SHA-1(our X25519 key | the iPhone's), AES-CTR
 *   SETUP (no streams)     timing (NTP-like, UDP) and event ports; enabled features
 *   SETUP streams          screen 110 (TCP), audio 100 / 101 (UDP); per stream keys from the shared secret
 *   RECORD, TEARDOWN, POST /command (modes, UI requests), POST /feedback (audio clocks)
 * The event connection (we listen, the iPhone connects) carries our commands: HID reports (knob, media keys), Siri,
 * key frame requests.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "cp.h"
#include "cp_bplist.h"
#include "cp_crypto.h"

#define MAIN_UUID "e5f7a68d-7b0f-4305-984b-974f677a150b"
#define DEVICE_ID "02:00:00:00:00:02"
#define BT_ID "02:00:00:00:00:01"
#define FEATURES 0x615653aee2ULL
#define PLIST "application/x-apple-binary-plist"

/* ---- our identity (persistent: state file airplay-identity) and the iPhones paired with us (airplay-pairings) ---- */

static unsigned char g_seed[32], g_secret[64], g_public[32];
static char g_pairing_id[40], g_public_hex[65];

int airplay_identity(void)
{
    char b[256], hex[65];
    unsigned i;
    const int n = state_read("airplay-identity", b, sizeof(b) - 1);
    if (n > 0) {
        b[n] = 0;
        if (sscanf(b, "seed=%64s\nid=%39s", hex, g_pairing_id) == 2 && strlen(hex) == 64) {
            for (i = 0; i < 32; ++i) {
                unsigned v;
                sscanf(hex + 2 * i, "%2x", &v);
                g_seed[i] = (unsigned char)v;
            }
            goto keys;
        }
    }
    cp_random(g_seed, 32);
    {
        unsigned char r[16];
        cp_random(r, 16);
        r[6] = (unsigned char)((r[6] & 0x0f) | 0x40);
        r[8] = (unsigned char)((r[8] & 0x3f) | 0x80);
        sprintf(g_pairing_id, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", r[0], r[1],
                r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
    }
    tohex(hex, g_seed, 32);
    snprintf(b, sizeof(b), "seed=%s\nid=%s\n", hex, g_pairing_id);
    state_write("airplay-identity", b, (unsigned)strlen(b));
    say("airplay: new identity %s", g_pairing_id);
keys:
    ed25519_keys(g_seed, g_secret, g_public);
    tohex(g_public_hex, g_public, 32);
    say("airplay: identity %s, key %.16s...", g_pairing_id, g_public_hex);
    return 0;
}

const char* airplay_public_hex(void)
{
    return g_public_hex;
}

#define MAX_PAIRINGS 16
static struct {
    char id[64];
    unsigned char key[32];
} g_pairings[MAX_PAIRINGS];
static unsigned g_npairings;
static int g_pairings_loaded;

static void pairings_load(void)
{
    char b[4096], *line, *save = 0;
    const int n = state_read("airplay-pairings", b, sizeof(b) - 1);
    g_pairings_loaded = 1;
    if (n <= 0)
        return;
    b[n] = 0;
    for (line = strtok_r(b, "\n", &save); line && g_npairings < MAX_PAIRINGS; line = strtok_r(0, "\n", &save)) {
        char id[64], hex[65];
        unsigned i;
        if (sscanf(line, "%63s %64s", id, hex) != 2 || strlen(hex) != 64)
            continue;
        snprintf(g_pairings[g_npairings].id, sizeof(g_pairings[0].id), "%s", id);
        for (i = 0; i < 32; ++i) {
            unsigned v;
            sscanf(hex + 2 * i, "%2x", &v);
            g_pairings[g_npairings].key[i] = (unsigned char)v;
        }
        ++g_npairings;
    }
}

static const unsigned char* pairing_key(const char* id)
{
    unsigned i;
    if (!g_pairings_loaded)
        pairings_load();
    for (i = 0; i < g_npairings; ++i)
        if (!strcmp(g_pairings[i].id, id))
            return g_pairings[i].key;
    return 0;
}

static void pairing_save(const char* id, const unsigned char key[32])
{
    char b[4096], hex[65];
    unsigned i, at = 0, slot;
    if (!g_pairings_loaded)
        pairings_load();
    for (slot = 0; slot < g_npairings && strcmp(g_pairings[slot].id, id); ++slot) {
    }
    if (slot == g_npairings) {
        if (g_npairings == MAX_PAIRINGS) {   /* the oldest goes */
            memmove(g_pairings, g_pairings + 1, sizeof(g_pairings[0]) * (MAX_PAIRINGS - 1));
            slot = MAX_PAIRINGS - 1;
        } else
            ++g_npairings;
    }
    snprintf(g_pairings[slot].id, sizeof(g_pairings[0].id), "%s", id);
    memcpy(g_pairings[slot].key, key, 32);
    for (i = 0; i < g_npairings; ++i) {
        tohex(hex, g_pairings[i].key, 32);
        at += (unsigned)snprintf(b + at, sizeof(b) - at, "%s %s\n", g_pairings[i].id, hex);
    }
    state_write("airplay-pairings", b, at);
}

/* ---- TLV8 ---- */

/* the value of a type (fragments of 255 bytes joined); its length, -1 = not there */
static int tlv_get(const unsigned char* b, unsigned n, unsigned type, unsigned char* out, unsigned size)
{
    unsigned at = 0, len = 0;
    int found = 0, last = -1;
    while (at + 2 <= n) {
        const unsigned t = b[at], l = b[at + 1];
        if (at + 2 + l > n)
            break;
        if (t == type) {
            if (!(found && last == (int)type))
                len = 0;   /* a new item of this type: the last one wins, as xcertplay */
            if (len + l > size)
                return -1;
            memcpy(out + len, b + at + 2, l);
            len += l;
            found = 1;
        }
        last = (int)t;
        at += 2 + l;
    }
    return found ? (int)len : -1;
}

static void tlv_put(unsigned char* b, unsigned* at, unsigned type, const unsigned char* v, unsigned n)
{
    unsigned off = 0;
    do {
        const unsigned k = n - off > 255 ? 255 : n - off;
        b[(*at)++] = (unsigned char)type;
        b[(*at)++] = (unsigned char)k;
        memcpy(b + *at, v + off, k);
        *at += k;
        off += k;
    } while (off < n);
}

static void tlv_byte(unsigned char* b, unsigned* at, unsigned type, unsigned v)
{
    const unsigned char x = (unsigned char)v;
    tlv_put(b, at, type, &x, 1);
}

enum { T_METHOD = 0, T_ID = 1, T_SALT = 2, T_KEY = 3, T_PROOF = 4, T_DATA = 5, T_STATE = 6, T_ERROR = 7, T_SIG = 10 };

/* ---- one session (the control connection) ---- */

struct cipher {
    unsigned char key_read[32], key_write[32];
    unsigned long long n_read, n_write;
};

struct session {
    int s;
    struct net_addr peer;
    /* pair-setup */
    struct srp srp;
    int srp_ready;
    unsigned char K[64];
    int have_K;
    /* pair-verify */
    unsigned char eph_secret[32], eph_public[32], peer_eph[32], shared[32], verify_key[32];
    int have_shared;
    /* control encryption */
    int encrypted, encrypt_next;
    struct cipher c;
    /* event connection */
    int ev_listen, ev_s;
    struct cipher ev;
    unsigned ev_cseq;
    pthread_t ev_thread;
    int ev_thread_on;
    /* timing */
    int ntp_s;
    unsigned short ntp_peer_port;
    pthread_t ntp_rx, ntp_tx;
    int ntp_on;
    /* streams */
    int screen, audio[3], mic_pending;
    struct net_addr mic_to;
    unsigned char mic_key[32];
    int mic_rate, mic_channels, mic_frames;
    int keepalive_s;
    unsigned char raw[1 << 17], plain[1 << 18];         /* control connection: received, decrypted */
    unsigned char ev_raw[1 << 16], ev_plain[1 << 16];   /* event connection */
    volatile int closing;
};

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;   /* g_cur and event sends */
static struct session* g_cur;

/* ---- encrypted frames: {u16 LE length, ciphertext, 16-byte tag}, AAD = the length, nonce = counter ---- */

static unsigned char* seal_frames(struct cipher* c, const unsigned char* in, unsigned n, unsigned* out_n)
{
    const unsigned frames = n ? (n + 0x3fff) / 0x4000 : 1;
    unsigned char* out = (unsigned char*)malloc(n + frames * 18);
    unsigned off = 0, at = 0;
    if (!out)
        return 0;
    do {
        unsigned char nonce[12];
        const unsigned k = n - off > 0x4000 ? 0x4000 : n - off;
        put16le(out + at, k);
        nonce_counter(nonce, c->n_write++);
        aead_seal(c->key_write, nonce, out + at, 2, in + off, k, out + at + 2);
        at += 2 + k + 16;
        off += k;
    } while (off < n);
    *out_n = at;
    return out;
}

/* whole frames from buf (moved out of it) into plain; -1 = not authentic */
static int open_frames(struct cipher* c, unsigned char* buf, unsigned* bufn, unsigned char* plain, unsigned* plainn,
                       unsigned plain_size)
{
    unsigned at = 0;
    while (*bufn - at >= 2) {
        const unsigned k = get16le(buf + at);
        unsigned char nonce[12];
        if (*bufn - at < 2 + k + 16)
            break;
        if (*plainn + k > plain_size)
            return -1;
        nonce_counter(nonce, c->n_read++);
        if (aead_open(c->key_read, nonce, buf + at, 2, buf + at + 2, k + 16, plain + *plainn))
            return -1;
        *plainn += k;
        at += 2 + k + 16;
    }
    memmove(buf, buf + at, *bufn - at);
    *bufn -= at;
    return 0;
}

/* ---- RTSP messages ---- */

struct request {
    char method[16], path[128], protocol[16];
    unsigned cseq;
    int has_cseq;
    const unsigned char* body;
    unsigned body_n;
};

/* one whole request at the start of buf: its length, 0 = not complete yet, -1 = garbage */
static int parse_request(const unsigned char* buf, unsigned n, struct request* r)
{
    const char* end = 0;
    unsigned i, head, clen = 0;
    char line[512];
    for (i = 0; i + 3 < n; ++i)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            end = (const char*)buf + i;
            break;
        }
    if (!end)
        return n > 16384 ? -1 : 0;
    head = (unsigned)(end - (const char*)buf) + 4;
    memset(r, 0, sizeof(*r));
    {
        const char* p = (const char*)buf;
        while (p < end) {
            const char* e = p;
            unsigned k;
            while (e < end && *e != '\r')
                ++e;
            k = (unsigned)(e - p) < sizeof(line) - 1 ? (unsigned)(e - p) : sizeof(line) - 1;
            memcpy(line, p, k);
            line[k] = 0;
            if (p == (const char*)buf) {
                if (sscanf(line, "%15s %127s %15s", r->method, r->path, r->protocol) < 2)
                    return -1;
                if (!r->protocol[0])
                    strcpy(r->protocol, "RTSP/1.0");
            } else if (!strncasecmp(line, "content-length:", 15))
                clen = (unsigned)strtoul(line + 15, 0, 10);
            else if (!strncasecmp(line, "cseq:", 5)) {
                r->cseq = (unsigned)strtoul(line + 5, 0, 10);
                r->has_cseq = 1;
            }
            p = e + 2;
        }
    }
    if (clen > 4 * 1024 * 1024)
        return -1;
    if (n < head + clen)
        return 0;
    r->body = buf + head;
    r->body_n = clen;
    return (int)(head + clen);
}

struct response {
    int status;
    const char* type;
    unsigned char* body;   /* malloc'ed or 0 */
    unsigned body_n;
};

static unsigned char* build_response(const struct request* q, const struct response* a, unsigned* n)
{
    char head[512];
    int k = snprintf(head, sizeof(head), "%s %d %s\r\n", q->protocol, a->status,
                     a->status == 200 ? "OK" : a->status == 400 ? "Bad Request" : a->status == 404 ? "Not Found"
                                                                                                  : "Internal Server Error");
    unsigned char* out;
    if (a->type)
        k += snprintf(head + k, sizeof(head) - k, "Content-Type: %s\r\n", a->type);
    if (q->has_cseq)
        k += snprintf(head + k, sizeof(head) - k, "CSeq: %u\r\n", q->cseq);
    k += snprintf(head + k, sizeof(head) - k, "Content-Length: %u\r\n\r\n", a->body_n);
    out = (unsigned char*)malloc((unsigned)k + a->body_n);
    if (!out)
        return 0;
    memcpy(out, head, (unsigned)k);
    if (a->body_n)
        memcpy(out + k, a->body, a->body_n);
    *n = (unsigned)k + a->body_n;
    return out;
}

static void plist_answer(struct response* a, struct bp* v)
{
    a->body = bp_encode(v, &a->body_n);
    a->type = PLIST;
    if (!a->body)
        a->status = 500;
    bp_free(v);
}

static void log_plist(const char* what, const struct bp* v)
{
    char b[1200];
    bp_print(v, b, sizeof(b));
    say("airplay: %s %s", what, b);
}

/* ---- /info ---- */

static struct bp* hid_device(unsigned uid, const char* name, const unsigned char* descriptor, unsigned n)
{
    struct bp* d = bp_dict();
    char id[16];
    snprintf(id, sizeof(id), "%x", uid);
    bp_set(d, "hidProductID", bp_int(1));
    bp_set(d, "hidVendorID", bp_int(2));
    bp_set(d, "hidCountryCode", bp_int(0));
    bp_set(d, "uuid", bp_str(id));
    bp_set(d, "name", bp_str(name));
    bp_set(d, "displayUUID", bp_str(MAIN_UUID));
    bp_set(d, "hidDescriptor", bp_data(descriptor, n));
    return d;
}

/* HID report descriptors as xcertplay's (AirPlayHid.kt): knob {buttons select / home / back, x, y, wheel}; media
 * (one byte: 1 play, 2 pause, 3 play/pause, 4 next, 5 previous, 6 ...); telephony (one byte) */
static const unsigned char KNOB_DESCRIPTOR[] = {
    0x05, 0x01, 0x09, 0x08, 0xa1, 0x01, 0x05, 0x09, 0x09, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81,
    0x02, 0x05, 0x0c, 0x0a, 0x23, 0x02, 0x0a, 0x24, 0x02, 0x95, 0x02, 0x81, 0x02, 0x95, 0x05, 0x81, 0x01, 0x05, 0x01,
    0x09, 0x01, 0xa1, 0x00, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0xc0,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xc0};
static const unsigned char MEDIA_DESCRIPTOR[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x15, 0x00, 0x25, 0x06, 0x05, 0x0c, 0x0a, 0x00, 0x00, 0x0a, 0xb0, 0x00, 0x0a,
    0xb1, 0x00, 0x0a, 0xcd, 0x00, 0x0a, 0xb5, 0x00, 0x0a, 0xb6, 0x00, 0x0a, 0x9e, 0x02, 0x75, 0x08, 0x95, 0x01, 0x81,
    0x00, 0xc0};
static const unsigned char PHONE_DESCRIPTOR[] = {
    0x05, 0x0b, 0x09, 0x07, 0xa1, 0x01, 0x15, 0x00, 0x25, 0x11, 0x05, 0x0b, 0x09, 0x00, 0x09, 0x20, 0x09, 0x21,
    0x09, 0x26, 0x09, 0x2f, 0x09, 0xb0, 0x09, 0xb1, 0x09, 0xb2, 0x09, 0xb3, 0x09, 0xb4, 0x09, 0xb5, 0x09, 0xb6,
    0x09, 0xb7, 0x09, 0xb8, 0x09, 0xb9, 0x09, 0xba, 0x09, 0xbb, 0x05, 0x07, 0x09, 0x2a, 0x75, 0x08, 0x95, 0x01,
    0x81, 0x00, 0xc0};

static struct bp* resource(int id)
{
    struct bp* r = bp_dict();
    bp_set(r, "resourceID", bp_int(id));
    bp_set(r, "transferType", bp_int(1));
    bp_set(r, "transferPriority", bp_int(100));
    bp_set(r, "takeConstraint", bp_int(100));
    bp_set(r, "borrowConstraint", bp_int(100));
    bp_set(r, "unborrowConstraint", bp_int(100));
    return r;
}

static struct bp* app_state(int id, const char* key, long long v, int boolean)
{
    struct bp* a = bp_dict();
    bp_set(a, "appStateID", bp_int(id));
    bp_set(a, key, boolean ? bp_bool((int)v) : bp_int(v));
    return a;
}

static void latency(struct bp* list, int type, const char* audio_type)
{
    struct bp* e = bp_add(list, bp_dict());
    bp_set(e, "type", bp_int(type));
    if (audio_type)
        bp_set(e, "audioType", bp_str(audio_type));
    bp_set(e, "inputLatencyMicros", bp_int(0));
    bp_set(e, "outputLatencyMicros", bp_int(0));
}

static void format(struct bp* list, int type, const char* audio_type, unsigned long long out, unsigned long long in)
{
    struct bp* e = bp_add(list, bp_dict());
    bp_set(e, "type", bp_int(type));
    bp_set(e, "audioType", bp_str(audio_type));
    bp_set(e, "audioOutputFormats", bp_uint(out));
    if (in)
        bp_set(e, "audioInputFormats", bp_uint(in));
}

/* LPCM format bits (xcertplay CarPlayAudioFormats, wired): bit 2+2i mono / 3+2i stereo for 8, 16, 24, 32, 44.1 kHz,
 * 14 / 15 for 48 kHz. Assistant mono 24-48 kHz, navigation / alerts mono or stereo 44.1 / 48 kHz, telephony mono
 * 8-24 kHz, media stereo 44.1 / 48 kHz, microphone mono 8 / 16 kHz. No AAC (type 102): nothing here decodes it. */
#define F_ASSISTANT 0x4540ULL
#define F_NAVIGATION 0xcc00ULL
#define F_PHONE 0x54ULL
#define F_MEDIA 0x8800ULL
#define F_MIC 0x14ULL

static struct bp* info_plist(void)
{
    struct bp* d = bp_dict();
    struct bp *modes, *list, *disp, *area, *safe;
    const unsigned long long mic = g_opt.mic ? F_MIC : 0;
    bp_set(d, "sourceVersion", bp_str(SOURCE_VERSION));
    bp_set(d, "features", bp_uint(FEATURES));
    bp_set(d, "statusFlags", bp_int(4));
    bp_set(d, "model", bp_str("NBT"));
    bp_set(d, "manufacturer", bp_str("BMW"));
    bp_set(d, "deviceID", bp_str(DEVICE_ID));
    list = bp_set(d, "bluetoothIDs", bp_array());
    bp_add(list, bp_str(BT_ID));
    bp_set(d, "name", bp_str("BMW NBT"));
    bp_set(d, "rightHandDrive", bp_bool(0));
    bp_set(d, "keepAliveLowPower", bp_bool(0));
    bp_set(d, "keepAliveSendStatsAsBody", bp_bool(0));
    modes = bp_set(d, "modes", bp_dict());
    list = bp_set(modes, "resources", bp_array());
    bp_add(list, resource(1));
    bp_add(list, resource(2));
    list = bp_set(modes, "appStates", bp_array());
    bp_add(list, app_state(2, "state", 0, 1));
    bp_add(list, app_state(1, "speechMode", -1, 0));
    bp_add(list, app_state(3, "state", 0, 1));
    if (g_opt.audio) {
        list = bp_set(d, "audioLatencies", bp_array());
        latency(list, 100, 0);
        latency(list, 100, "default");
        latency(list, 100, "media");
        latency(list, 100, "telephony");
        latency(list, 100, "speechRecognition");
        latency(list, 100, "alert");
        latency(list, 101, 0);
        latency(list, 101, "default");
        list = bp_set(d, "audioFormats", bp_array());
        format(list, 100, "compatibility", F_ASSISTANT, mic);
        format(list, 101, "compatibility", F_NAVIGATION, 0);
        format(list, 100, "default", F_ASSISTANT, mic);
        format(list, 100, "alert", F_NAVIGATION, 0);
        format(list, 100, "media", F_MEDIA, 0);
        format(list, 100, "telephony", F_PHONE, mic);
        format(list, 100, "speechRecognition", F_ASSISTANT, mic);
        format(list, 101, "default", F_NAVIGATION, 0);
    }
    list = bp_set(d, "extendedFeatures", bp_array());
    bp_add(list, bp_str("vocoderInfo"));
    bp_add(list, bp_str("enhancedRequestCarUI"));
    list = bp_set(d, "displays", bp_array());
    disp = bp_add(list, bp_dict());
    bp_set(disp, "uuid", bp_str(MAIN_UUID));
    bp_set(disp, "type", bp_int(110));
    bp_set(disp, "maxFPS", bp_int(g_opt.fps));
    bp_set(disp, "widthPixels", bp_int(g_opt.width));
    bp_set(disp, "heightPixels", bp_int(g_opt.height));
    bp_set(disp, "widthPixelsMax", bp_int(g_opt.width));
    bp_set(disp, "heightPixelsMax", bp_int(g_opt.height));
    bp_set(disp, "widthPhysical", bp_int(g_opt.width_mm));
    bp_set(disp, "heightPhysical", bp_int(g_opt.height_mm));
    /* features: knobs (0x02), no touch; primary input: the knob (3; 1 = touch screen, 2 = touch pad: a guess from
     * Apple's AirPlay constants, xcertplay only uses 1) */
    bp_set(disp, "features", bp_int(0x02));
    bp_set(disp, "primaryInputDevice", bp_int(3));
    list = bp_set(disp, "viewAreas", bp_array());
    area = bp_add(list, bp_dict());
    bp_set(area, "viewAreaIndex", bp_int(0));
    bp_set(area, "widthPixels", bp_int(g_opt.width));
    bp_set(area, "heightPixels", bp_int(g_opt.height));
    bp_set(area, "originXPixels", bp_int(0));
    bp_set(area, "originYPixels", bp_int(0));
    bp_set(area, "viewAreaTransitionControl", bp_int(0));
    bp_set(area, "viewAreaStatusBarEdge", bp_int(0));
    bp_set(area, "viewAreaSupportsFocusTransfer", bp_bool(0));
    safe = bp_set(area, "safeArea", bp_dict());
    bp_set(safe, "widthPixels", bp_int(g_opt.width));
    bp_set(safe, "heightPixels", bp_int(g_opt.height));
    bp_set(safe, "originXPixels", bp_int(0));
    bp_set(safe, "originYPixels", bp_int(0));
    bp_set(safe, "drawUIOutsideSafeArea", bp_bool(1));
    bp_set(disp, "initialViewArea", bp_int(0));
    bp_set(disp, "adjacentViewAreas", bp_array());
    list = bp_set(d, "hidDevices", bp_array());
    bp_add(list, hid_device(HID_KNOB, "BMW iDrive Controller", KNOB_DESCRIPTOR, sizeof(KNOB_DESCRIPTOR)));
    bp_add(list, hid_device(HID_MEDIA, "BMW Media Keys", MEDIA_DESCRIPTOR, sizeof(MEDIA_DESCRIPTOR)));
    bp_add(list, hid_device(HID_PHONE, "BMW Telephony", PHONE_DESCRIPTOR, sizeof(PHONE_DESCRIPTOR)));
    return d;
}

/* ---- pairing ---- */

static void tlv_error(struct response* a, unsigned state)
{
    unsigned at = 0;
    a->body = (unsigned char*)malloc(6);
    if (!a->body) {
        a->status = 500;
        return;
    }
    tlv_byte(a->body, &at, T_STATE, state);
    tlv_byte(a->body, &at, T_ERROR, 2);
    a->body_n = at;
    a->type = "application/pairing+tlv8";
}

static void pair_setup(struct session* s, const struct request* q, struct response* a)
{
    unsigned char st[1], A[400], proof[64], data[1024], plain[1024], sub[512], out[1100];
    int k;
    unsigned at = 0;
    if (tlv_get(q->body, q->body_n, T_STATE, st, 1) != 1) {
        tlv_error(a, 0);
        return;
    }
    a->type = "application/pairing+tlv8";
    if (st[0] == 1) {   /* M1 -> M2: salt and B */
        const long long t0 = ms_now();
        if (srp_start(&s->srp, "Pair-Setup", "3939")) {
            tlv_error(a, 2);
            return;
        }
        s->srp_ready = 1;
        say("airplay: pair-setup M1 -> M2 (SRP in %lld ms)", ms_now() - t0);
        tlv_byte(out, &at, T_STATE, 2);
        tlv_put(out, &at, T_KEY, s->srp.B, sizeof(s->srp.B));
        tlv_put(out, &at, T_SALT, s->srp.salt, sizeof(s->srp.salt));
    } else if (st[0] == 3) {   /* M3 -> M4: the proofs */
        unsigned char M2[64];
        const int an = tlv_get(q->body, q->body_n, T_KEY, A, sizeof(A));
        const int pn = tlv_get(q->body, q->body_n, T_PROOF, proof, sizeof(proof));
        if (!s->srp_ready || an <= 0 || pn != 64 ||
            srp_verify(&s->srp, A, (unsigned)an, proof, (unsigned)pn, s->K, M2)) {
            say("airplay: pair-setup M3: wrong proof");
            tlv_error(a, 4);
            return;
        }
        s->have_K = 1;
        say("airplay: pair-setup M3 -> M4");
        tlv_byte(out, &at, T_STATE, 4);
        tlv_put(out, &at, T_PROOF, M2, 64);
    } else if (st[0] == 5) {   /* M5 -> M6: Ed25519 identities */
        unsigned char key[32], nonce[12], sign_key[32], ltpk[32], sig[64], msg[200];
        char id[64];
        int idn;
        unsigned m, sn = 0;
        k = tlv_get(q->body, q->body_n, T_DATA, data, sizeof(data));
        if (!s->have_K || k < 16) {
            tlv_error(a, 6);
            return;
        }
        hkdf512(s->K, 64, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", key, 32);
        nonce_label(nonce, "PS-Msg05");
        if (aead_open(key, nonce, 0, 0, data, (unsigned)k, plain)) {
            say("airplay: pair-setup M5 not authentic");
            tlv_error(a, 6);
            return;
        }
        m = (unsigned)k - 16;
        idn = tlv_get(plain, m, T_ID, (unsigned char*)id, sizeof(id) - 1);
        if (idn <= 0 || tlv_get(plain, m, T_KEY, ltpk, 32) != 32 || tlv_get(plain, m, T_SIG, sig, 64) != 64) {
            tlv_error(a, 6);
            return;
        }
        id[idn] = 0;
        hkdf512(s->K, 64, "Pair-Setup-Controller-Sign-Salt", "Pair-Setup-Controller-Sign-Info", sign_key, 32);
        memcpy(msg, sign_key, 32);
        memcpy(msg + 32, id, (unsigned)idn);
        memcpy(msg + 32 + idn, ltpk, 32);
        if (ed25519_verify(sig, ltpk, msg, 64 + (unsigned)idn)) {
            say("airplay: pair-setup M5: the iPhone's signature is wrong");
            tlv_error(a, 6);
            return;
        }
        pairing_save(id, ltpk);
        say("airplay: pair-setup M5: iPhone %s paired", id);
        hkdf512(s->K, 64, "Pair-Setup-Accessory-Sign-Salt", "Pair-Setup-Accessory-Sign-Info", sign_key, 32);
        memcpy(msg, sign_key, 32);
        memcpy(msg + 32, g_pairing_id, strlen(g_pairing_id));
        memcpy(msg + 32 + strlen(g_pairing_id), g_public, 32);
        ed25519_sign(sig, g_secret, msg, 64 + (unsigned)strlen(g_pairing_id));
        tlv_put(sub, &sn, T_ID, (const unsigned char*)g_pairing_id, (unsigned)strlen(g_pairing_id));
        tlv_put(sub, &sn, T_KEY, g_public, 32);
        tlv_put(sub, &sn, T_SIG, sig, 64);
        nonce_label(nonce, "PS-Msg06");
        aead_seal(key, nonce, 0, 0, sub, sn, data);
        tlv_byte(out, &at, T_STATE, 6);
        tlv_put(out, &at, T_DATA, data, sn + 16);
    } else {
        tlv_error(a, st[0] + 1u);
        return;
    }
    a->body = (unsigned char*)malloc(at);
    if (!a->body) {
        a->status = 500;
        return;
    }
    memcpy(a->body, out, at);
    a->body_n = at;
}

static void pair_verify(struct session* s, const struct request* q, struct response* a)
{
    unsigned char st[1], out[600], sub[200], data[300], nonce[12], sig[64], msg[200];
    unsigned at = 0, sn = 0;
    if (tlv_get(q->body, q->body_n, T_STATE, st, 1) != 1) {
        tlv_error(a, 0);
        return;
    }
    a->type = "application/pairing+tlv8";
    if (st[0] == 1) {   /* M1 -> M2 */
        const unsigned idn = (unsigned)strlen(g_pairing_id);
        if (tlv_get(q->body, q->body_n, T_KEY, s->peer_eph, 32) != 32) {
            tlv_error(a, 2);
            return;
        }
        x25519_keys(s->eph_secret, s->eph_public);
        x25519_shared(s->shared, s->eph_secret, s->peer_eph);
        s->have_shared = 0;
        hkdf512(s->shared, 32, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", s->verify_key, 32);
        memcpy(msg, s->eph_public, 32);
        memcpy(msg + 32, g_pairing_id, idn);
        memcpy(msg + 32 + idn, s->peer_eph, 32);
        ed25519_sign(sig, g_secret, msg, 64 + idn);
        tlv_put(sub, &sn, T_ID, (const unsigned char*)g_pairing_id, idn);
        tlv_put(sub, &sn, T_SIG, sig, 64);
        nonce_label(nonce, "PV-Msg02");
        aead_seal(s->verify_key, nonce, 0, 0, sub, sn, data);
        tlv_byte(out, &at, T_STATE, 2);
        tlv_put(out, &at, T_KEY, s->eph_public, 32);
        tlv_put(out, &at, T_DATA, data, sn + 16);
        say("airplay: pair-verify M1 -> M2");
    } else if (st[0] == 3) {   /* M3 -> M4: the iPhone proves it is the paired one; control keys */
        char id[64];
        int idn;
        const unsigned char* ltpk;
        const int k = tlv_get(q->body, q->body_n, T_DATA, data, sizeof(data));
        nonce_label(nonce, "PV-Msg03");
        if (k < 16 || aead_open(s->verify_key, nonce, 0, 0, data, (unsigned)k, sub)) {
            say("airplay: pair-verify M3 not authentic");
            tlv_error(a, 4);
            return;
        }
        idn = tlv_get(sub, (unsigned)k - 16, T_ID, (unsigned char*)id, sizeof(id) - 1);
        if (idn <= 0 || tlv_get(sub, (unsigned)k - 16, T_SIG, sig, 64) != 64) {
            tlv_error(a, 4);
            return;
        }
        id[idn] = 0;
        ltpk = pairing_key(id);
        if (!ltpk) {
            say("airplay: pair-verify: iPhone %s is not paired with us (it pairs again)", id);
            tlv_error(a, 4);
            return;
        }
        memcpy(msg, s->peer_eph, 32);
        memcpy(msg + 32, id, (unsigned)idn);
        memcpy(msg + 32 + idn, s->eph_public, 32);
        if (ed25519_verify(sig, ltpk, msg, 64 + (unsigned)idn)) {
            say("airplay: pair-verify: the iPhone's signature is wrong");
            tlv_error(a, 4);
            return;
        }
        hkdf512(s->shared, 32, "Control-Salt", "Control-Write-Encryption-Key", s->c.key_read, 32);
        hkdf512(s->shared, 32, "Control-Salt", "Control-Read-Encryption-Key", s->c.key_write, 32);
        s->c.n_read = s->c.n_write = 0;
        s->have_shared = 1;
        s->encrypt_next = 1;   /* after this answer */
        tlv_byte(out, &at, T_STATE, 4);
        say("airplay: pair-verify done: iPhone %s, control connection encrypted from now on", id);
    } else {
        tlv_error(a, st[0] + 1u);
        return;
    }
    a->body = (unsigned char*)malloc(at);
    if (!a->body) {
        a->status = 500;
        return;
    }
    memcpy(a->body, out, at);
    a->body_n = at;
}

/* MFi authentication: {1, the iPhone's X25519 key} -> our key, the chip's certificate, its signature of
 * SHA-1(our key | the iPhone's key) encrypted with AES-128-CTR (key and IV from the X25519 secret) */
static void auth_setup(const struct request* q, struct response* a)
{
    unsigned char secret[32], pub[32], shared[32], h[20], key[20], iv[20], digest[32], sig[128], enc[128];
    const unsigned char* cert;
    unsigned certn, sl, dn, i, nz = 0;
    if (q->body_n != 33 || q->body[0] != 1 || acp_cert(&cert, &certn)) {
        say("airplay: auth-setup: %u bytes, or no chip", q->body_n);
        a->status = 400;
        return;
    }
    x25519_keys(secret, pub);
    x25519_shared(shared, secret, q->body + 1);
    for (i = 0; i < 32; ++i)
        nz |= shared[i];
    if (!nz) {
        a->status = 400;
        return;
    }
    sha1_of(key, (const unsigned char*)"AES-KEY", 7, shared, 32);
    sha1_of(iv, (const unsigned char*)"AES-IV", 6, shared, 32);
    if (acp_major() >= 3) {   /* 3.0 chips sign SHA-256 */
        sha256_of(digest, pub, 32, q->body + 1, 32);
        dn = 32;
    } else {
        sha1_of(h, pub, 32, q->body + 1, 32);
        memcpy(digest, h, 20);
        dn = 20;
    }
    if (acp_sign(digest, dn, sig, &sl)) {
        a->status = 500;
        return;
    }
    aes128_ctr(key, iv, sig, sl, enc);
    a->body = (unsigned char*)malloc(32 + 4 + certn + 4 + sl);
    if (!a->body) {
        a->status = 500;
        return;
    }
    memcpy(a->body, pub, 32);
    put32(a->body + 32, certn);
    memcpy(a->body + 36, cert, certn);
    put32(a->body + 36 + certn, sl);
    memcpy(a->body + 40 + certn, enc, sl);
    a->body_n = 40 + certn + sl;
    a->type = "application/octet-stream";
    say("airplay: auth-setup: chip certificate and signature sent");
}

/* ---- the event connection ---- */

/* a command to the current session's iPhone over the event connection (any thread); takes command */
static int event_send(struct bp* command)
{
    char head[200];
    unsigned char *body, *plain, *wire;
    unsigned bn, wn;
    int k, r = -1;
    struct session* s;
    body = bp_encode(command, &bn);
    bp_free(command);
    if (!body)
        return -1;
    pthread_mutex_lock(&g_m);   /* the session is not freed while this is held (session_end) */
    s = g_cur;
    if (!s || s->ev_s < 0) {
        pthread_mutex_unlock(&g_m);
        free(body);
        return -1;
    }
    k = snprintf(head, sizeof(head), "POST /command RTSP/1.0\r\nContent-Type: " PLIST
                                     "\r\nContent-Length: %u\r\nCSeq: %u\r\n\r\n",
                 bn, ++s->ev_cseq);
    plain = (unsigned char*)malloc((unsigned)k + bn);
    if (plain) {
        memcpy(plain, head, (unsigned)k);
        memcpy(plain + k, body, bn);
        wire = seal_frames(&s->ev, plain, (unsigned)k + bn, &wn);
        if (wire) {
            r = net_send(s->ev_s, wire, (int)wn);
            free(wire);
        }
        free(plain);
    }
    pthread_mutex_unlock(&g_m);
    free(body);
    return r;
}

static void handle_command(struct session* s, const struct bp* body);

static void* event_thread(void* arg)
{
    struct session* s = (struct session*)arg;
    unsigned char* buf = s->ev_raw;
    unsigned char* plain = s->ev_plain;
    unsigned bn = 0, pn = 0;
    struct net_addr peer;
    int c = -1;
    while (!s->closing && !g_stop) {
        c = net_accept(s->ev_listen, &peer, 1000);
        if (c >= 0)
            break;
        if (c == -1)
            return 0;
    }
    if (c < 0)
        return 0;
    hkdf512(s->shared, 32, "Events-Salt", "Events-Write-Encryption-Key", s->ev.key_write, 32);
    hkdf512(s->shared, 32, "Events-Salt", "Events-Read-Encryption-Key", s->ev.key_read, 32);
    s->ev.n_read = s->ev.n_write = 0;
    pthread_mutex_lock(&g_m);
    s->ev_s = c;
    pthread_mutex_unlock(&g_m);
    say("airplay: event connection up");
    while (!s->closing && !g_stop) {
        struct request q;
        int used;
        const int r = net_recv(c, buf + bn, (int)(sizeof(s->ev_raw) - bn), 1000);
        if (r == -2)
            continue;
        if (r <= 0)
            break;
        bn += (unsigned)r;
        if (open_frames(&s->ev, buf, &bn, plain, &pn, sizeof(s->ev_plain))) {
            say("airplay: event connection: frame not authentic");
            break;
        }
        while ((used = parse_request(plain, pn, &q)) > 0) {
            if (strncmp(q.method, "RTSP/", 5) && strncmp(q.method, "HTTP/", 5)) {   /* a request, not an answer */
                struct response a;
                unsigned char* wire;
                unsigned wn;
                memset(&a, 0, sizeof(a));
                a.status = 200;
                if (!strcmp(q.method, "POST") && strstr(q.path, "/command")) {
                    struct bp* body = bp_decode(q.body, q.body_n);
                    handle_command(s, body);
                    bp_free(body);
                }
                wire = build_response(&q, &a, &wn);
                if (wire) {
                    unsigned sealed_n;
                    unsigned char* sealed;
                    pthread_mutex_lock(&g_m);
                    sealed = seal_frames(&s->ev, wire, wn, &sealed_n);
                    if (sealed) {
                        net_send(c, sealed, (int)sealed_n);
                        free(sealed);
                    }
                    pthread_mutex_unlock(&g_m);
                    free(wire);
                }
            }
            memmove(plain, plain + used, pn - (unsigned)used);
            pn -= (unsigned)used;
        }
        if (used < 0) {
            say("airplay: event connection: unreadable message");
            break;
        }
    }
    say("airplay: event connection closed");
    pthread_mutex_lock(&g_m);
    s->ev_s = -1;
    pthread_mutex_unlock(&g_m);
    net_close(c);
    return 0;
}

void airplay_hid(unsigned uid, const unsigned char* report, unsigned n)
{
    struct bp* c = bp_dict();
    char id[16];
    snprintf(id, sizeof(id), "%x", uid);
    bp_set(c, "type", bp_str("hidSendReport"));
    bp_set(c, "uuid", bp_str(id));
    bp_set(c, "hidReport", bp_data(report, n));
    event_send(c);
}

static void send_simple(const char* type, struct bp* params)
{
    struct bp* c = bp_dict();
    bp_set(c, "type", bp_str(type));
    bp_set(c, "params", params ? params : bp_dict());
    event_send(c);
}

void airplay_siri(void)
{
    struct bp* p = bp_dict();
    bp_set(p, "siriAction", bp_int(2));
    send_simple("requestSiri", p);
    p = bp_dict();
    bp_set(p, "siriAction", bp_int(3));
    send_simple("requestSiri", p);
}

void airplay_key_frame(void)
{
    send_simple("forceKeyFrame", 0);
}

int airplay_connected(void)
{
    int r;
    pthread_mutex_lock(&g_m);
    r = g_cur && g_cur->ev_s >= 0;
    pthread_mutex_unlock(&g_m);
    return r;
}

/* ---- timing: 32-byte packets, 210 request / 211 answer, timestamps at 8 (origin), 16 (receive), 24 (transmit) ---- */

static pthread_mutex_t g_clock = PTHREAD_MUTEX_INITIALIZER;
static long long g_offset_ns;   /* the iPhone's clock minus ours */
static int g_synced;

static unsigned long long ntp_now(void)
{
    struct timespec t;
    long long ns;
    clock_gettime(CLOCK_REALTIME, &t);
    pthread_mutex_lock(&g_clock);
    ns = (long long)t.tv_sec * 1000000000LL + t.tv_nsec + g_offset_ns;
    pthread_mutex_unlock(&g_clock);
    {
        const unsigned long long sec = (unsigned long long)(ns / 1000000000LL) + 2208988800ULL;
        const unsigned long long frac = ((unsigned long long)(ns % 1000000000LL) << 32) / 1000000000ULL;
        return sec << 32 | frac;
    }
}

static void put_ntp(unsigned char* p, unsigned long long v)
{
    put32(p, (unsigned)(v >> 32));
    put32(p + 4, (unsigned)v);
}

static unsigned long long get_ntp(const unsigned char* p)
{
    return (unsigned long long)get32(p) << 32 | get32(p + 4);
}

static double ntp_seconds(long long d)
{
    return (double)d / 4294967296.0;
}

static unsigned long long g_pending_t1;
static double g_best_rtt = 1e9, g_best_offset;
static int g_picks;

static void timing_answer(const unsigned char* m)
{
    const unsigned long long t4 = ntp_now(), t1 = get_ntp(m + 8), t2 = get_ntp(m + 16), t3 = get_ntp(m + 24);
    double offset, rtt;
    if (t1 != g_pending_t1)
        return;
    g_pending_t1 = 0;
    offset = (ntp_seconds((long long)(t2 - t1)) + ntp_seconds((long long)(t3 - t4))) / 2;
    rtt = ntp_seconds((long long)(t4 - t1)) - ntp_seconds((long long)(t3 - t2));
    if (rtt < 0)
        return;
    if (rtt < g_best_rtt) {
        g_best_rtt = rtt;
        g_best_offset = offset;
    }
    if (++g_picks < 2)
        return;
    g_picks = 0;
    pthread_mutex_lock(&g_clock);
    if (!g_synced || g_best_offset > 0.128 || g_best_offset < -0.128) {
        g_offset_ns += (long long)(g_best_offset * 1e9);
        if (!g_synced)
            say("airplay: clock synchronised (offset %.3f s, round trip %.1f ms)", g_best_offset, g_best_rtt * 1000);
        g_synced = 1;
    } else
        g_offset_ns += (long long)(g_best_offset * 1e9 / 8);
    pthread_mutex_unlock(&g_clock);
    g_best_rtt = 1e9;
}

static void* timing_rx(void* arg)
{
    struct session* s = (struct session*)arg;
    unsigned char m[128], a[32];
    struct net_addr from;
    while (!s->closing && !g_stop) {
        const int n = net_recvfrom(s->ntp_s, m, sizeof(m), 1000, &from);
        if (n == -2)
            continue;
        if (n < 0)
            break;
        if (n < 32)
            continue;
        if (m[1] == 210) {   /* a request: answered */
            memset(a, 0, sizeof(a));
            a[0] = 0x80;
            a[1] = 211;
            put16(a + 2, 7);
            memcpy(a + 8, m + 24, 8);
            put_ntp(a + 16, ntp_now());
            put_ntp(a + 24, ntp_now());
            net_sendto(s->ntp_s, a, 32, &from);
        } else if (m[1] == 211)
            timing_answer(m);
    }
    return 0;
}

static void* timing_tx(void* arg)
{
    struct session* s = (struct session*)arg;
    unsigned char m[32];
    struct net_addr to = s->peer;
    to.port = s->ntp_peer_port;
    while (!s->closing && !g_stop) {
        memset(m, 0, sizeof(m));
        m[0] = 0x80;
        m[1] = 210;
        put16(m + 2, 7);
        g_pending_t1 = ntp_now();
        put_ntp(m + 24, g_pending_t1);
        net_sendto(s->ntp_s, m, 32, &to);
        sleep_ms(1000);
    }
    return 0;
}

/* ---- SETUP ---- */

static void stream_key(const struct session* s, unsigned long long connection, const char* label, unsigned char key[32])
{
    char salt[64];
    snprintf(salt, sizeof(salt), "DataStream-Salt%llu", connection);
    hkdf512(s->shared, 32, salt, label, key, 32);
}

static void setup_session(struct session* s, const struct bp* d, struct response* a)
{
    struct bp *answer = bp_dict(), *features;
    const struct bp* asked = bp_get(d, "features");
    unsigned short ev_port = 0, ntp_port = 0;
    unsigned i;
    const char* name = bp_text(d, "name");
    const char* model = bp_text(d, "model");
    if (name || model)
        say("airplay: iPhone \"%s\" (%s, %s)", name ? name : "?", model ? model : "?",
            bp_text(d, "deviceID") ? bp_text(d, "deviceID") : "?");
    s->ntp_peer_port = (unsigned short)bp_num_or(d, "timingPort", 0);
    if (s->ev_listen < 0) {
        s->ev_listen = net_tcp_listen(0, &ev_port);
        if (s->ev_listen >= 0 && !pthread_create(&s->ev_thread, 0, event_thread, s))
            s->ev_thread_on = 1;
    }
    if (s->ntp_s < 0) {
        s->ntp_s = net_udp(0, &ntp_port);
        if (s->ntp_s >= 0 && !pthread_create(&s->ntp_rx, 0, timing_rx, s)) {
            s->ntp_on = 1;   /* answers the iPhone's requests */
            if (s->ntp_peer_port && !pthread_create(&s->ntp_tx, 0, timing_tx, s))
                s->ntp_on = 2;   /* and asks it every second */
        }
    }
    bp_set(answer, "timingPort", bp_int(ntp_port));
    bp_set(answer, "eventPort", bp_int(ev_port));
    if (bp_num_or(d, "keepAliveLowPower", 0) && s->keepalive_s < 0) {
        unsigned short kp = 0;
        s->keepalive_s = net_udp(0, &kp);   /* never read: what comes is dropped once its queue is full */
        bp_set(answer, "keepAlivePort", bp_int(kp));
    }
    features = bp_set(answer, "enabledFeatures", bp_array());
    if (asked && asked->type == BP_ARRAY)
        for (i = 0; i < asked->n; ++i)
            if (asked->items[i]->type == BP_STR && !strcmp(asked->items[i]->s, "viewAreas"))
                bp_add(features, bp_str("viewAreas"));
    log_plist("SETUP answer", answer);
    plist_answer(a, answer);
}

static void setup_streams(struct session* s, const struct bp* list, struct response* a)
{
    struct bp *answer = bp_dict(), *streams = bp_set(answer, "streams", bp_array());
    unsigned i;
    for (i = 0; i < list->n; ++i) {
        const struct bp* st = list->items[i];
        const long long type = bp_num_or(st, "type", -1);
        const struct bp* idv = bp_get(st, "streamConnectionID");
        const unsigned long long connection = idv && idv->type == BP_INT ? idv->u : 0;
        unsigned char key[32];
        if (!s->have_shared) {
            say("airplay: SETUP of a stream before pair-verify");
            break;
        }
        stream_key(s, connection, "DataStream-Output-Encryption-Key", key);
        if (type == 110) {
            unsigned short port = 0;
            if (screen_open(key, &port))
                continue;
            s->screen = 1;
            {
                struct bp* e = bp_add(streams, bp_dict());
                bp_set(e, "type", bp_int(110));
                bp_set(e, "dataPort", bp_int(port));
            }
            say("airplay: screen stream on port %u", port);
        } else if (type >= 100 && type <= 102) {
            unsigned short dp = 0, cport = 0;
            const char* audio_type = bp_text(st, "audioType");
            const unsigned long long fmt = (unsigned long long)bp_num_or(st, "audioFormat", 0);
            if (!g_opt.audio ||
                audio_stream_open((int)type, audio_type ? audio_type : "default", fmt, connection, key, &dp, &cport))
                continue;
            s->audio[type - 100] = 1;
            {
                struct bp* e = bp_add(streams, bp_dict());
                bp_set(e, "type", bp_int(type));
                bp_set(e, "dataPort", bp_int(dp));
                bp_set(e, "controlPort", bp_int(cport));
                bp_set(e, "streamConnectionID", bp_uint(connection));
            }
            /* the microphone: the iPhone gives its port in the main audio stream's SETUP */
            if (type == 100 && g_opt.mic && bp_num_or(st, "dataPort", 0) > 0) {
                int ch = 1;
                s->mic_to = s->peer;
                s->mic_to.port = (unsigned short)bp_num_or(st, "dataPort", 0);
                stream_key(s, connection, "DataStream-Input-Encryption-Key", s->mic_key);
                s->mic_rate = audio_rate(fmt, &ch);
                s->mic_channels = ch;
                s->mic_frames = (int)bp_num_or(st, "framesPerPacket", 0);
                s->mic_pending = 1;
            }
        } else
            say("airplay: stream type %lld not offered", type);
    }
    log_plist("SETUP streams answer", answer);
    plist_answer(a, answer);
    if (s->screen)
        cp_reached(STOP_AIRPLAY);
}

static void teardown(struct session* s, const struct bp* d)
{
    const struct bp* list = bp_get(d, "streams");
    unsigned i;
    if (!list || list->type != BP_ARRAY || !list->n) {
        say("airplay: TEARDOWN of everything");
        screen_close();
        audio_stream_close(-1);
        mic_stop();
        s->screen = 0;
        memset(s->audio, 0, sizeof(s->audio));
        return;
    }
    for (i = 0; i < list->n; ++i) {
        const long long type = bp_num_or(list->items[i], "type", -1);
        say("airplay: TEARDOWN of stream %lld", type);
        if (type == 110) {
            screen_close();
            s->screen = 0;
        } else if (type >= 100 && type <= 102) {
            if (type == 100)
                mic_stop();
            audio_stream_close((int)type);
            s->audio[type - 100] = 0;
        }
    }
}

/* modesChanged: who owns the screen and the sound; the speech state (Siri) for nbt_knob (nbt-aa-voice) */
static void handle_command(struct session* s, const struct bp* body)
{
    const char* type = bp_text(body, "type");
    const struct bp* params = bp_get(body, "params");
    (void)s;
    if (!type)
        return;
    log_plist("command", body);
    if (!strcmp(type, "modesChanged") && params) {
        const struct bp* states = bp_get(params, "appStates");
        unsigned i;
        if (states && states->type == BP_ARRAY)
            for (i = 0; i < states->n; ++i)
                if (bp_num_or(states->items[i], "appStateID", -1) == 1) {
                    const long long mode = bp_num_or(states->items[i], "speechMode", -1);
                    if (mode == 1 || mode == 2)
                        flag_set("/fs/ram3/nbt-aa-voice", 0);
                    else
                        flag_clear("/fs/ram3/nbt-aa-voice");
                }
    } else if (!strcmp(type, "requestViewArea") && params) {
        struct bp* p = bp_dict();
        bp_set(p, "uuid", bp_str(MAIN_UUID));
        bp_set(p, "viewAreaIndex", bp_int(0));
        bp_set(p, "adjacentViewAreas", bp_array());
        bp_set(p, "animationDurationMillis", bp_int(0));
        send_simple("updateViewArea", p);
    }
}

/* ---- requests on the control connection ---- */

static void handle(struct session* s, const struct request* q, struct response* a)
{
    a->status = 200;
    if (!strcmp(q->method, "SETUP")) {
        struct bp* d = bp_decode(q->body, q->body_n);
        const struct bp* streams = bp_get(d, "streams");
        if (!d) {
            a->status = 400;
            return;
        }
        log_plist("SETUP", d);
        if (streams && streams->type == BP_ARRAY)
            setup_streams(s, streams, a);
        else
            setup_session(s, d, a);
        bp_free(d);
        return;
    }
    if (!strcmp(q->method, "RECORD")) {
        say("airplay: RECORD");
        return;
    }
    if (!strcmp(q->method, "TEARDOWN")) {
        struct bp* d = bp_decode(q->body, q->body_n);
        teardown(s, d);
        bp_free(d);
        return;
    }
    if (strstr(q->path, "/pair-setup")) {
        pair_setup(s, q, a);
        return;
    }
    if (strstr(q->path, "/pair-verify")) {
        pair_verify(s, q, a);
        return;
    }
    if (strstr(q->path, "/auth-setup")) {
        auth_setup(q, a);
        return;
    }
    if (strstr(q->path, "/info")) {
        struct bp* info = info_plist();
        say("airplay: /info (%ux%u, %u fps)", g_opt.width, g_opt.height, g_opt.fps);
        plist_answer(a, info);
        return;
    }
    if (!strcmp(q->method, "POST") && strstr(q->path, "/command")) {
        struct bp* d = bp_decode(q->body, q->body_n);
        handle_command(s, d);
        bp_free(d);
        return;
    }
    if (!strcmp(q->method, "POST") && strstr(q->path, "/feedback")) {
        struct bp *d = bp_dict(), *streams = bp_set(d, "streams", bp_array());
        audio_feedback(streams, ntp_now());
        if (streams->n)
            plist_answer(a, d);
        else
            bp_free(d);
        return;
    }
    say("airplay: %s %s (%u bytes): answered 200", q->method, q->path, q->body_n);
}

static int send_answer(struct session* s, const struct request* q, struct response* a)
{
    unsigned n, wn;
    unsigned char* wire = build_response(q, a, &n);
    int r = -1;
    free(a->body);
    if (!wire)
        return -1;
    if (s->encrypted) {
        unsigned char* sealed = seal_frames(&s->c, wire, n, &wn);
        if (sealed) {
            r = net_send(s->s, sealed, (int)wn);
            free(sealed);
        }
    } else
        r = net_send(s->s, wire, (int)n);
    free(wire);
    if (s->encrypt_next) {
        s->encrypt_next = 0;
        s->encrypted = 1;
    }
    return r;
}

static void session_end(struct session* s)
{
    s->closing = 1;
    pthread_mutex_lock(&g_m);
    if (g_cur == s)
        g_cur = 0;
    pthread_mutex_unlock(&g_m);
    mic_stop();
    screen_close();
    audio_stream_close(-1);
    if (s->ev_thread_on)
        pthread_join(s->ev_thread, 0);
    net_close(s->ev_listen);
    if (s->ntp_on) {
        pthread_join(s->ntp_rx, 0);
        if (s->ntp_on == 2)
            pthread_join(s->ntp_tx, 0);
    }
    net_close(s->ntp_s);
    net_close(s->keepalive_s);
    net_close(s->s);
    flag_clear("/fs/ram3/nbt-aa-voice");
}

static void* session_thread(void* arg)
{
    struct session* s = (struct session*)arg;
    unsigned char* raw = s->raw;
    unsigned char* plain = s->plain;
    unsigned rn = 0, pn = 0;
    char peer[80];
    say("airplay: connection from %s", net_text(&s->peer, peer, sizeof(peer)));
    pthread_mutex_lock(&g_m);
    g_cur = s;
    pthread_mutex_unlock(&g_m);
    while (!g_stop) {
        struct request q;
        int used;
        const int r = net_recv(s->s, s->encrypted ? raw + rn : plain + pn,
                               (int)(s->encrypted ? sizeof(s->raw) - rn : sizeof(s->plain) - pn), 1000);
        if (r == -2)
            continue;
        if (r <= 0) {
            say("airplay: the iPhone closed the connection");
            break;
        }
        if (s->encrypted) {
            rn += (unsigned)r;
            if (open_frames(&s->c, raw, &rn, plain, &pn, sizeof(s->plain))) {
                say("airplay: control frame not authentic");
                break;
            }
        } else
            pn += (unsigned)r;
        while ((used = parse_request(plain, pn, &q)) > 0) {
            struct response a;
            memset(&a, 0, sizeof(a));
            if (!strstr(q.path, "/feedback"))
                say("airplay: <- %s %s (%u bytes)", q.method, q.path, q.body_n);
            handle(s, &q, &a);
            if (send_answer(s, &q, &a))
                break;
            if (s->mic_pending) {   /* after the SETUP answer, as xcertplay */
                s->mic_pending = 0;
                mic_start(&s->mic_to, s->mic_key, s->mic_rate, s->mic_channels, 100, s->mic_frames);
            }
            memmove(plain, plain + used, pn - (unsigned)used);
            pn -= (unsigned)used;
        }
        if (used < 0) {
            say("airplay: unreadable request");
            break;
        }
    }
    session_end(s);
    free(s);
    say("airplay: session over");
    return 0;
}

static int g_listen = -1;

static void* listen_thread(void* arg)
{
    (void)arg;
    while (!g_stop) {
        struct net_addr peer;
        const int c = net_accept(g_listen, &peer, 1000);
        struct session* s;
        pthread_t t;
        if (c == -2)
            continue;
        if (c < 0) {
            sleep_ms(200);
            continue;
        }
        cp_reached(STOP_NET);   /* the network works: the iPhone reached us */
        s = (struct session*)calloc(1, sizeof(*s));
        if (!s) {
            net_close(c);
            continue;
        }
        s->s = c;
        s->peer = peer;
        s->ev_listen = s->ev_s = s->ntp_s = s->keepalive_s = -1;
        if (pthread_create(&t, 0, session_thread, s)) {
            net_close(c);
            free(s);
            continue;
        }
        pthread_detach(t);
    }
    return 0;
}

int airplay_start(unsigned short port)
{
    pthread_t t;
    unsigned short bound = 0;
    g_listen = net_tcp_listen(port, &bound);
    if (g_listen < 0) {
        say("airplay: port %u not available", port);
        return -1;
    }
    if (pthread_create(&t, 0, listen_thread, 0))
        return -1;
    pthread_detach(t);
    say("airplay: listening on port %u", bound);
    return 0;
}
