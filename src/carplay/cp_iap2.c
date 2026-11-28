/* nbt_carplay: iAP2 over the carkit service: identification, authentication with the car's chip, CarPlay start.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 *
 * As nbt_carplay_probe (car 2026-11-23: identification accepted, authentication succeeded with the 2.0B chip,
 * CarPlayAvailability came), then as xcertplay's wired control client: PowerSourceUpdate, the subscriptions,
 * StartHID (media playback remote), and CarPlayStartSession (4301: our IPv6 address on the NCM link, the AirPlay port,
 * our MAC, our Ed25519 key, the source version) for each CarPlayAvailability (4300). The iPhone then connects to the
 * AirPlay receiver (cp_airplay.c). NowPlayingUpdate (5001) gives the playback state for nbt_knob
 * (/fs/ram3/nbt-aa-media: 2 playing, 3 paused).
 * Link layer: marker FF 55 02 00 EE 10 and a SYN from us; packets FF 5A, length, control (80 SYN, 40 ACK, 10 RST),
 * sequence, acknowledgement, session, header checksum, payload, payload checksum. Session 10 carries the control
 * messages: 40 40, length, id, parameters (length, id, data; groups nest).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cp.h"
#include "cp_lockdown.h"

enum { CONTROL = 10, FILES = 12 };   /* the sessions named in the SYN (iap2_run) */

static struct {
    struct stream* s;
    unsigned char in[140000];
    unsigned inn;
    unsigned sent_seq, last_rx, synced, peer_maxlen, peer_acks, normal;
    unsigned char csm[140000];
    unsigned csmn;
    int identified, authenticated, failed, started, carplay_sent;
    const struct iap2_carplay* cp;
} g_l;

static unsigned char csum(const unsigned char* b, unsigned n)
{
    unsigned s = 0, i;
    for (i = 0; i < n; ++i)
        s += b[i];
    return (unsigned char)(0x100 - (s & 0xff));
}

static int link_packet(unsigned control, unsigned seq, unsigned session, const unsigned char* p, unsigned n)
{
    static unsigned char f[70000];
    const unsigned len = 9 + (n ? n + 1 : 0);
    if (len > sizeof(f))
        return -1;
    f[0] = 0xff;
    f[1] = 0x5a;
    put16(f + 2, len);
    f[4] = (unsigned char)control;
    f[5] = (unsigned char)seq;
    f[6] = (unsigned char)g_l.last_rx;
    f[7] = (unsigned char)session;
    f[8] = csum(f, 8);
    if (n) {
        memcpy(f + 9, p, n);
        f[9 + n] = csum(p, n);
    }
    return stream_write(g_l.s, f, len);
}

static int csm_send(unsigned id, const unsigned char* params, unsigned n)
{
    static unsigned char m[70000];
    unsigned off = 0;
    const unsigned chunk = g_l.peer_maxlen > 10 ? g_l.peer_maxlen - 10 : 1000;
    if (n + 6 > 0xffff)
        return -1;
    m[0] = 0x40;
    m[1] = 0x40;
    put16(m + 2, n + 6);
    put16(m + 4, id);
    memcpy(m + 6, params, n);
    say("iap2: -> %04x (%u bytes)", id, n + 6);
    while (off < n + 6) {
        const unsigned k = n + 6 - off < chunk ? n + 6 - off : chunk;
        g_l.sent_seq = (g_l.sent_seq + 1) & 0xff;
        if (link_packet(0x40, g_l.sent_seq, CONTROL, m + off, k))
            return -1;
        off += k;
    }
    return 0;
}

/* ---- parameters ---- */

static unsigned prm(unsigned char* b, unsigned at, unsigned id, const void* data, unsigned n)
{
    put16(b + at, n + 4);
    put16(b + at + 2, id);
    if (n)
        memcpy(b + at + 4, data, n);
    return at + 4 + n;
}

static unsigned prm_str(unsigned char* b, unsigned at, unsigned id, const char* s)
{
    return prm(b, at, id, s, (unsigned)strlen(s) + 1);
}

static unsigned prm_u8(unsigned char* b, unsigned at, unsigned id, unsigned v)
{
    const unsigned char x = (unsigned char)v;
    return prm(b, at, id, &x, 1);
}

static unsigned prm_u16(unsigned char* b, unsigned at, unsigned id, unsigned v)
{
    unsigned char x[2];
    put16(x, v);
    return prm(b, at, id, x, 2);
}

static unsigned prm_u32(unsigned char* b, unsigned at, unsigned id, unsigned v)
{
    unsigned char x[4];
    put32(x, v);
    return prm(b, at, id, x, 4);
}

static unsigned prm_u16s(unsigned char* b, unsigned at, unsigned id, const unsigned short* v, unsigned count)
{
    unsigned char x[128];
    unsigned i;
    for (i = 0; i < count; ++i)
        put16(x + 2 * i, v[i]);
    return prm(b, at, id, x, 2 * count);
}

static const unsigned char* prm_find(const unsigned char* p, unsigned n, unsigned id, unsigned* len)
{
    unsigned at = 0;
    while (at + 4 <= n) {
        const unsigned l = get16(p + at), i = get16(p + at + 2);
        if (l < 4 || at + l > n)
            return 0;
        if (i == id) {
            *len = l - 4;
            return p + at + 4;
        }
        at += l;
    }
    return 0;
}

/* IdentificationInformation (1D01) as the probe's (accepted on the car): identity strings, the messages we send and
 * take, power, an external accessory protocol, English, the USB host transport (the NCM control interface) and a
 * media playback HID remote */
static int send_identification(void)
{
    static const unsigned short sent[] = {0x5000, 0x5002, 0x5200, 0x5203, 0xae00, 0xae02, 0x4157, 0x4159,
                                          0x4154, 0x4156, 0xae03, 0x4301, 0x6800, 0x6802, 0x6803};
    /* 5204 = lanes; accepted with the display's lane limits by iOS 27 on the car, 2026-11-25 */
    static const unsigned short taken[] = {0xea00, 0xea01, 0x5001, 0x5201, 0x5202, 0xae01, 0x4158, 0x4155, 0x4300,
                                           0x6801, 0x5204};
    unsigned char b[2048], g[256];
    unsigned at = 0, gn;
    at = prm_str(b, at, 0, "BMW NBT");
    at = prm_str(b, at, 1, "NBT");
    at = prm_str(b, at, 2, "BMW");
    at = prm_str(b, at, 3, "nbt-android-auto");
    at = prm_str(b, at, 4, "1.0");
    at = prm_str(b, at, 5, "1.0");
    at = prm_u16s(b, at, 6, sent, sizeof(sent) / sizeof(sent[0]));
    at = prm_u16s(b, at, 7, taken, sizeof(taken) / sizeof(taken[0]));
    at = prm_u8(b, at, 8, 2);
    at = prm_u16(b, at, 9, 20);
    gn = prm_u8(g, 0, 0, 1);
    gn = prm_str(g, gn, 1, "com.nbtaa.carplay");
    gn = prm_u8(g, gn, 2, 0);
    at = prm(b, at, 10, g, gn);
    at = prm_str(b, at, 12, "en");
    at = prm_str(b, at, 13, "en");
    gn = prm_u16(g, 0, 0, 0);
    gn = prm_str(g, gn, 1, "USBHostTransport");
    gn = prm(g, gn, 2, 0, 0);
    gn = prm_u8(g, gn, 3, g_l.cp->ncm_interface);
    gn = prm(g, gn, 4, 0, 0);
    at = prm(b, at, 16, g, gn);
    gn = prm_u16(g, 0, 0, 0);
    gn = prm_str(g, gn, 1, "Media Playback Remote");
    gn = prm_u8(g, gn, 2, 1);
    at = prm(b, at, 18, g, gn);
    {   /* a route guidance display: without one the iPhone sends no 5201 / 5202 (car 2026-11-24, iOS 27) */
        gn = prm_u16(g, 0, 0, 1);
        gn = prm_str(g, gn, 1, "Instrument Cluster");
        gn = prm_u16(g, gn, 2, 64);
        gn = prm_u16(g, gn, 3, 64);
        gn = prm_u16(g, gn, 4, 64);
        gn = prm_u16(g, gn, 5, 64);
        gn = prm_u16(g, gn, 6, 8);
        gn = prm_u16(g, gn, 7, 64);   /* lane guidance: description length, storage */
        gn = prm_u16(g, gn, 8, 8);
        at = prm(b, at, 30, g, gn);
    }
    return csm_send(0x1d01, b, at);
}

static const unsigned char MEDIA_REMOTE[] = {0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
                                             0x95, 0x04, 0x09, 0xb0, 0x09, 0xb1, 0x09, 0xb5, 0x09, 0xb6, 0x81, 0x02,
                                             0x75, 0x04, 0x95, 0x01, 0x81, 0x03, 0xc0};

/* after authentication: power, subscriptions, the HID media remote (as xcertplay's wired control client) */
static void start_control(void)
{
    unsigned char b[512], g[64];
    unsigned at, gn;
    at = prm_u16(b, 0, 0, 1000);   /* 1000 mA for the iPhone */
    at = prm_u8(b, at, 1, 1);      /* charging */
    csm_send(0xae03, b, at);
    gn = prm(g, 0, 1, 0, 0);       /* NowPlaying: title, duration, album, artist, artwork id */
    gn = prm(g, gn, 4, 0, 0);
    gn = prm(g, gn, 6, 0, 0);
    gn = prm(g, gn, 12, 0, 0);
    gn = prm(g, gn, 26, 0, 0);
    at = prm(b, 0, 0, g, gn);
    gn = prm(g, 0, 0, 0, 0);       /* playback: status, elapsed, queue index, queue count, app name */
    gn = prm(g, gn, 1, 0, 0);
    gn = prm(g, gn, 2, 0, 0);
    gn = prm(g, gn, 3, 0, 0);
    gn = prm(g, gn, 7, 0, 0);
    at = prm(b, at, 1, g, gn);
    csm_send(0x5000, b, at);
    at = prm_u16(b, 0, 0, 1);   /* route guidance for the display named in the identification */
    csm_send(0x5200, b, at);
    at = prm(b, 0, 4, 0, 0);
    at = prm(b, at, 5, 0, 0);
    at = prm(b, at, 6, 0, 0);
    csm_send(0xae00, b, at);
    at = prm(b, 0, 0, 0, 0);
    at = prm(b, at, 4, 0, 0);
    at = prm(b, at, 5, 0, 0);
    csm_send(0x4157, b, at);
    at = prm(b, 0, 0, 0, 0);
    at = prm(b, at, 1, 0, 0);
    at = prm(b, at, 2, 0, 0);
    at = prm(b, at, 3, 0, 0);
    at = prm(b, at, 4, 0, 0);
    at = prm(b, at, 11, 0, 0);
    csm_send(0x4154, b, at);
    at = prm_u16(b, 0, 0, 0);
    at = prm_u16(b, at, 1, 2);
    at = prm_u16(b, at, 2, 1);
    at = prm(b, at, 4, MEDIA_REMOTE, sizeof(MEDIA_REMOTE));
    csm_send(0x6800, b, at);
    g_l.started = 1;
}

static void carplay_start(void)
{
    unsigned char b[512], g[128];
    unsigned at, gn;
    const struct iap2_carplay* cp = g_l.cp;
    gn = prm_str(g, 0, 0, cp->address);
    at = prm(b, 0, 0, g, gn);
    at = prm_u32(b, at, 2, cp->port);
    at = prm_str(b, at, 3, cp->mac);
    at = prm_str(b, at, 4, cp->public_key);
    at = prm_str(b, at, 5, cp->source_version);
    say("iap2: CarPlayStartSession: [%s]:%u, %s", cp->address, cp->port, cp->mac);
    csm_send(0x4301, b, at);
    g_l.carplay_sent++;
    cp_reached(STOP_IAP2);
}

/* What the iPhone plays, in the file this project's displays read (the iPod entry of nbt_ipod_tap / nbt_aa_ipod, which
 * only counts as playing with state=2 there; the instrument cluster; nbt_knob's skip keys): the same media.state as
 * oneplus_native writes for Android Auto (state 1 stopped, 2 playing, 3 paused; seconds). An update carries only what
 * changed, so the values are kept here. The cover (media.cover, "cover" = its size) comes as a file transfer, below. */
#define MEDIA_STATE "/fs/ram3/nbt-aa-native/media.state"
#define MEDIA_COVER "/fs/ram3/nbt-aa-native/media.cover"
static struct {
    unsigned seq, track, state, cover;
    long long position, duration;
    char source[64], title[204], artist[204], album[204];
} g_np = {0, 0, 0, 0, -1, -1, "", "", "", ""};

static void np_write(void)
{
    char buf[900];
    const int m = snprintf(buf, sizeof(buf),
                           "seq=%u\ntrack=%u\nstate=%u\nposition=%lld\nduration=%lld\nsource=%s\ntitle=%s\nartist=%s\n"
                           "album=%s\ncover=%u\n",
                           ++g_np.seq, g_np.track, g_np.state, g_np.position, g_np.duration, g_np.source, g_np.title,
                           g_np.artist, g_np.album, g_np.cover);
    const int fd = open(MEDIA_STATE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    if (write(fd, buf, (size_t)m) == m)
        rename(MEDIA_STATE ".tmp", MEDIA_STATE);
    close(fd);
}

/* The artwork of the song (attribute 26 of the song names the transfer): file transfer session, each packet the
 * transfer's identifier and a type - 04 set-up (then the size, 8 bytes), 80 / 00 / 40 first / more / last data (c0 = all
 * in one), 02 cancel. We answer 01 (start) to the set-up and 05 (success) to the last data. ponytail: the packet types
 * are from memory of the iAP2 specification, not from a document at hand; an unknown type is logged, not guessed at. */
static struct {
    int on, id;   /* on: a transfer was set up and is not over */
    unsigned n;
    unsigned char b[1024 * 1024];
} g_ft;

static int link_packet(unsigned control, unsigned seq, unsigned session, const unsigned char* p, unsigned n);

static void ft_answer(unsigned id, unsigned type)
{
    const unsigned char b[2] = {(unsigned char)id, (unsigned char)type};
    g_l.sent_seq = (g_l.sent_seq + 1) & 0xff;
    link_packet(0x40, g_l.sent_seq, FILES, b, 2);
}

static void ft_in(const unsigned char* p, unsigned n)
{
    if (n < 2)
        return;
    if (p[1] == 0x04) {
        say("iap2: artwork transfer %u: %u bytes", p[0], n >= 10 ? get32(p + 6) : 0);
        g_ft.on = 1;
        g_ft.id = p[0];
        g_ft.n = 0;
        ft_answer(p[0], 0x01);
        return;
    }
    if (p[1] & 0x3f) {
        say("iap2: file transfer %u: type %02x (%u bytes)", p[0], p[1], n);
        return;
    }
    if (!g_ft.on || p[0] != g_ft.id)
        return;
    if (p[1] & 0x80)
        g_ft.n = 0;
    if (n - 2 > sizeof(g_ft.b) - g_ft.n) {   /* larger than any cover: given up */
        g_ft.on = 0;
        return;
    }
    memcpy(g_ft.b + g_ft.n, p + 2, n - 2);
    g_ft.n += n - 2;
    if (p[1] & 0x40) {
        const int fd = open(MEDIA_COVER ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ft_answer(p[0], 0x05);
        g_ft.on = 0;
        if (fd < 0)
            return;
        if (write(fd, g_ft.b, g_ft.n) == (int)g_ft.n && !rename(MEDIA_COVER ".tmp", MEDIA_COVER)) {
            say("iap2: artwork %u bytes", g_ft.n);
            g_np.cover = g_ft.n;
            np_write();
        }
        close(fd);
    }
}

/* a text parameter into out (one line); 1 if it changed */
static int np_text(const unsigned char* g, unsigned len, unsigned id, char* out, unsigned size)
{
    char t[204];
    unsigned l, i;
    const unsigned char* v = prm_find(g, len, id, &l);
    if (!v)
        return 0;
    if (l >= size)
        l = size - 1;
    for (i = 0; i < l && v[i]; ++i)
        t[i] = v[i] == '\n' || v[i] == '\r' ? ' ' : (char)v[i];
    t[i] = 0;
    if (!strcmp(t, out))
        return 0;
    strcpy(out, t);
    return 1;
}

/* The iPhone's route guidance for the instrument cluster: the same nav.state as oneplus_native writes for Android Auto
 * (read by nbt_cluster). 5202 = one manoeuvre of the route (1 index, 3 type, 4 road after it, 10 exit angle, counted
 * counter-clockwise: -90 = right); 5201 = the state (1: 1 = guiding), the distance to the next manoeuvre (10 metres, 11
 * text, 12 unit: 0 km, 1 mi, 2 m, 3 yd, 4 ft) and the coming manoeuvres' indices (13). Seen on the car 2026-11-24.
 * In 5202, 10 (several) are the angles of the junction's other roads and 11 is the exit's, clockwise from straight on:
 * 90 = right, -90 = left (car log 2026-11-24: a right turn has 11 = 90, a left turn -90).
 * 5204 = one lane picture (1 index, 2 per lane: 1 status, 0 = not to take, 2 angles, 3 the angle to drive); 5201 says
 * which one to show (16 index, 17 how many, 18 showing; car log 2026-11-24).
 * ponytail: the type numbers beyond 1, 2 and 11 and all of 5204 are from memory of the iAP2 specification; an unknown
 * type draws the straight arrow. Roundabouts are drawn from the exit angle, the other roads as stubs (exits=).
 * nav.state's angle is Android Auto's: the way round the roundabout from the entry (90 right, 180 straight on, 270
 * left), so 180 - the iPhone's. */
#define NAV_STATE "/fs/ram3/nbt-aa-native/nav.state"
static struct {
    int type, angle;
    char road[124];
    char exits[36];   /* the junction's other roads, as nav.state's exits= */
} g_man[64];
static struct {
    unsigned seq, state, unit, cur, next, has_next, lane, lanes_on;
    long long dist, written;
    char dist_text[16];
} g_rg;

static char g_lanes[16][40];   /* per lane picture: "4h,1o,1o" as nbt_cluster reads it */
/* 1 = on the route, 6 = "proceed to the route" (parked beside the road: the first manoeuvre is already given; car
 * 2026-11-25) */
#define GUIDING (g_rg.state == 1 || g_rg.state == 6)

/* iAP2 manoeuvre type -> Android Auto's (as nbt_cluster draws it) */
static int nav_type(unsigned t)
{
    switch (t) {
    case 1: case 20: return 7;                 /* left */
    case 2: case 21: return 8;                 /* right */
    case 47: return 9;                         /* sharp left */
    case 48: return 10;                        /* sharp right */
    case 49: return 5;                         /* slight left */
    case 50: return 6;                         /* slight right */
    case 13: return 3;                         /* keep left */
    case 14: return 4;                         /* keep right */
    case 4: case 18: case 26: return 11;       /* U-turn */
    case 22: case 52: return 13;               /* ramp / change of motorway to the left */
    case 8: case 23: case 53: return 14;       /* ... to the right */
    case 10: case 12: case 24: case 25: case 27: return 39;   /* arrival */
    case 6: case 7: case 19: return 34;        /* roundabout */
    default: return t >= 28 && t <= 46 ? 34 : 0;   /* roundabout exit 1..19; else straight */
    }
}

static void nav_write(void)
{
    char buf[512];
    const int on = GUIDING && g_rg.cur < 64, two = on && g_rg.has_next && g_rg.next < 64;
    const int type = on ? nav_type((unsigned)g_man[g_rg.cur].type) : 0;
    const int m = snprintf(buf, sizeof(buf),
                           "seq=%u\nstatus=%d\ntype=%d\nangle=%d\nhas_angle=%d\ndist_m=%lld\ndist_text=%s\nunit=%u\nroad=%s\n"
                           "type2=%d\nroad2=%s\nlanes=%s\nexits=%s\n",
                           ++g_rg.seq, on, type, on ? 180 - g_man[g_rg.cur].angle : 0, type == 34, g_rg.dist, g_rg.dist_text,
                           g_rg.unit, on ? g_man[g_rg.cur].road : "", two ? nav_type((unsigned)g_man[g_rg.next].type) : -1,
                           two ? g_man[g_rg.next].road : "",
                           on && g_rg.lanes_on && g_rg.lane < 16 ? g_lanes[g_rg.lane] : "",
                           type == 34 ? g_man[g_rg.cur].exits : "");
    const int fd = open(NAV_STATE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    if (write(fd, buf, (size_t)m) == m)
        rename(NAV_STATE ".tmp", NAV_STATE);
    close(fd);
    g_rg.written = ms_now();
}

static void manoeuvre(const unsigned char* p, unsigned n)
{
    unsigned l, i, at = 0, len = 0;
    const unsigned char* v = prm_find(p, n, 1, &l);
    if (!v || l < 2 || (i = get16(v)) >= 64)
        return;
    while (at + 4 <= n) {   /* every parameter 10 is another road of the junction; the one we came from is left out */
        const unsigned pl = get16(p + at);
        if (pl < 4 || at + pl > n)
            break;
        if (get16(p + at + 2) == 10 && pl >= 6 && len + 6 < sizeof(g_man[i].exits)) {
            const int a = (short)get16(p + at + 4);
            if (a > -160 && a < 160)
                len += (unsigned)snprintf(g_man[i].exits + len, sizeof(g_man[i].exits) - len, "%s%d", len ? "," : "",
                                          180 - a);
        }
        at += pl;
    }
    g_man[i].exits[len] = 0;
    if ((v = prm_find(p, n, 3, &l)) != 0 && l >= 1)
        g_man[i].type = v[0];
    if ((v = prm_find(p, n, 11, &l)) != 0 && l >= 2)
        g_man[i].angle = (short)get16(v);
    np_text(p, n, 4, g_man[i].road, sizeof(g_man[i].road));
    if (nav_type((unsigned)g_man[i].type) == 34)
        say("iap2: manoeuvre %u: roundabout type %d, exit at %d degrees, others %s", i, g_man[i].type, g_man[i].angle,
            g_man[i].exits);
}

/* an angle (clockwise from straight on) -> the lane arrow's shape in nav.state */
static int lane_shape(int a)
{
    const int right = a > 0, m = a < 0 ? -a : a;
    return m < 20 ? 1 : m < 68 ? 2 + right : m < 113 ? 4 + right : m < 170 ? 6 + right : 8 + right;
}

static void lane_guidance(const unsigned char* p, unsigned n)
{
    static unsigned shown;
    unsigned l, i, at = 0, len = 0;
    const unsigned char* v = prm_find(p, n, 1, &l);
    if (shown < 8) {   /* the first ones whole: the layout is unconfirmed */
        shown++;
        hexdump("iap2: lanes", p, n < 400 ? n : 400);
    }
    if (!v || l < 2 || (i = get16(v)) >= 16)
        return;
    while (at + 4 <= n) {   /* every parameter 2 is one lane, left to right */
        const unsigned gl = get16(p + at), id = get16(p + at + 2);
        unsigned sl, al;
        const unsigned char *s, *a;
        if (gl < 4 || at + gl > n)
            break;
        if (id == 2 && len + 6 < sizeof(g_lanes[i])) {
            const unsigned char* g = p + at + 4;
            const int take = (s = prm_find(g, gl - 4, 1, &sl)) != 0 && sl >= 1 && s[0] != 0;
            a = take ? prm_find(g, gl - 4, 3, &al) : 0;
            if (!a || al < 2)
                a = prm_find(g, gl - 4, 2, &al);
            len += (unsigned)snprintf(g_lanes[i] + len, sizeof(g_lanes[i]) - len, "%s%d%c", len ? "," : "",
                                      a && al >= 2 ? lane_shape((short)get16(a)) : 1, take ? 'h' : 'o');
        }
        at += gl;
    }
    g_lanes[i][len] = 0;
    if (g_rg.lanes_on && g_rg.lane == i)
        nav_write();
}

static void route_guidance(const unsigned char* p, unsigned n)
{
    static const unsigned char units[5] = {3, 5, 1, 7, 6};   /* -> nav.state: km, mi, m, yd, ft */
    unsigned l;
    const unsigned char* v;
    if ((v = prm_find(p, n, 1, &l)) != 0 && l >= 1) {
        if (v[0] != g_rg.state)
            say("iap2: route guidance state %u", v[0]);
        g_rg.state = v[0];
    }
    if ((v = prm_find(p, n, 10, &l)) != 0 && l >= 4)
        g_rg.dist = get32(v);
    if ((v = prm_find(p, n, 12, &l)) != 0 && l >= 1)
        g_rg.unit = v[0] < 5 ? units[v[0]] : 0;
    np_text(p, n, 11, g_rg.dist_text, sizeof(g_rg.dist_text));
    if ((v = prm_find(p, n, 13, &l)) != 0 && l >= 2) {
        g_rg.cur = get16(v);
        g_rg.has_next = l >= 4;
        if (g_rg.has_next)
            g_rg.next = get16(v + 2);
    }
    if ((v = prm_find(p, n, 16, &l)) != 0 && l >= 2)
        g_rg.lane = get16(v);
    if ((v = prm_find(p, n, 18, &l)) != 0 && l >= 1)
        g_rg.lanes_on = v[0];
    nav_write();
}

static void now_playing(const unsigned char* p, unsigned n)
{
    unsigned len, l2;
    const unsigned char *g, *v;
    if ((g = prm_find(p, n, 0, &len)) != 0) {   /* the song: 1 title, 4 duration in ms, 6 album, 12 artist */
        if (np_text(g, len, 1, g_np.title, sizeof(g_np.title)) | np_text(g, len, 12, g_np.artist, sizeof(g_np.artist)) |
            np_text(g, len, 6, g_np.album, sizeof(g_np.album)))
            ++g_np.track;   /* the cover stays until the next one came: it may arrive before the title (car 2026-11-24) */
        if ((v = prm_find(g, len, 4, &l2)) != 0 && l2 >= 4)
            g_np.duration = get32(v) / 1000;
    }
    if ((g = prm_find(p, n, 1, &len)) != 0) {   /* the playback: 0 status, 1 elapsed ms, 7 app name */
        if ((v = prm_find(g, len, 0, &l2)) != 0 && l2 >= 1) {
            say("iap2: playback state %u", v[0]);
            flag_set("/fs/ram3/nbt-aa-media", v[0] == 1 ? "2\n" : "3\n");
            g_np.state = v[0] == 1 ? 2 : v[0] == 0 ? 1 : 3;
        }
        if ((v = prm_find(g, len, 1, &l2)) != 0 && l2 >= 4)
            g_np.position = get32(v) / 1000;
        np_text(g, len, 7, g_np.source, sizeof(g_np.source));
    }
    np_write();
}

static void on_message(unsigned id, const unsigned char* p, unsigned n)
{
    unsigned char out[1400], sig[128];
    unsigned at, len, sl;
    const unsigned char* v;
    if (id != 0x5001 && id != 0xae01 && id != 0x5201 && id != 0x5202 && id != 0x5204)
        say("iap2: <- %04x (%u bytes)", id, n + 6);
    switch (id) {
    case 0x1d00:
        send_identification();
        break;
    case 0x1d02:
        say("iap2: identification accepted");
        g_l.identified = 1;
        break;
    case 0x1d03: {
        unsigned a = 0;
        say("iap2: identification REJECTED");
        while (a + 4 <= n) {
            const unsigned l = get16(p + a);
            say("iap2:   parameter %u", get16(p + a + 2));
            if (l < 4)
                break;
            a += l;
        }
        g_l.failed = 1;
        break;
    }
    case 0xaa00: {
        const unsigned char* cert;
        unsigned cn;
        if (acp_cert(&cert, &cn)) {
            g_l.failed = 1;
            break;
        }
        at = prm(out, 0, 0, cert, cn);
        csm_send(0xaa01, out, at);
        break;
    }
    case 0xaa02:
        v = prm_find(p, n, 0, &len);
        if (!v || acp_sign(v, len, sig, &sl)) {
            g_l.failed = 1;
            break;
        }
        at = prm(out, 0, 0, sig, sl);
        csm_send(0xaa03, out, at);
        break;
    case 0xaa04:
        say("iap2: authentication FAILED");
        g_l.failed = 1;
        break;
    case 0xaa05:
        say("iap2: authentication succeeded");
        g_l.authenticated = 1;
        if (!g_l.started)
            start_control();
        break;
    case 0x4300:
        if ((v = prm_find(p, n, 0, &len)) != 0) {
            unsigned l2;
            const unsigned char* a = prm_find(v, len, 0, &l2);
            say("iap2: CarPlay available over USB: %s", a && l2 ? (a[0] ? "yes" : "no") : "?");
        }
        carplay_start();
        break;
    case 0x5001:
        now_playing(p, n);
        break;
    case 0x5201:
        route_guidance(p, n);
        break;
    case 0x5202:
        manoeuvre(p, n);
        break;
    case 0x5204:
        lane_guidance(p, n);
        break;
    default:
        break;
    }
}

static void csm_in(const unsigned char* b, unsigned n)
{
    if (n > sizeof(g_l.csm) - g_l.csmn)
        g_l.csmn = 0;
    memcpy(g_l.csm + g_l.csmn, b, n);
    g_l.csmn += n;
    for (;;) {
        unsigned skip = 0, len;
        while (skip + 1 < g_l.csmn && !(g_l.csm[skip] == 0x40 && g_l.csm[skip + 1] == 0x40))
            ++skip;
        if (skip) {
            memmove(g_l.csm, g_l.csm + skip, g_l.csmn - skip);
            g_l.csmn -= skip;
        }
        if (g_l.csmn < 6)
            return;
        len = get16(g_l.csm + 2);
        if (len < 6) {
            memmove(g_l.csm, g_l.csm + 1, --g_l.csmn);
            continue;
        }
        if (g_l.csmn < len)
            return;
        on_message(get16(g_l.csm + 4), g_l.csm + 6, len - 6);
        memmove(g_l.csm, g_l.csm + len, g_l.csmn - len);
        g_l.csmn -= len;
    }
}

static void link_in(void)
{
    for (;;) {
        unsigned skip = 0, len, control, seq, session, pn;
        const unsigned char* pl;
        while (skip + 1 < g_l.inn && !(g_l.in[skip] == 0xff && g_l.in[skip + 1] == 0x5a)) {
            if (g_l.in[skip] == 0xff && skip + 6 <= g_l.inn && g_l.in[skip + 1] == 0x55) {
                skip += 6;   /* the iPhone's marker */
                continue;
            }
            ++skip;
        }
        if (skip) {
            memmove(g_l.in, g_l.in + skip, g_l.inn - skip);
            g_l.inn -= skip;
        }
        if (g_l.inn < 9)
            return;
        if (csum(g_l.in, 8) != g_l.in[8]) {
            memmove(g_l.in, g_l.in + 1, --g_l.inn);
            continue;
        }
        len = get16(g_l.in + 2);
        control = g_l.in[4];
        seq = g_l.in[5];
        session = g_l.in[7];
        if (len < 9) {
            memmove(g_l.in, g_l.in + 1, --g_l.inn);
            continue;
        }
        if (g_l.inn < len)
            return;
        pl = g_l.in + 9;
        pn = len > 9 ? len - 10 : 0;
        if (len > 9 && csum(pl, pn) != pl[pn])
            say("iap2: packet %u with a bad payload checksum (ignored)", seq);
        else if (control & 0x10) {
            say("iap2: the iPhone reset the link");
            g_l.failed = 1;
        } else {
            if (control & 0x80) {
                if (pn >= 10) {
                    g_l.peer_maxlen = get16(pl + 2);
                    g_l.peer_acks = pl[9];
                }
                g_l.last_rx = seq;
                g_l.synced = 1;
                link_packet(0x40, g_l.sent_seq, 0, 0, 0);
            }
            if ((control & 0x40) && !g_l.normal && g_l.synced) {
                g_l.normal = 1;
                say("iap2: link up");
            }
            if (control == 0x40 && pn) {
                if (seq == ((g_l.last_rx + 1) & 0xff)) {
                    g_l.last_rx = seq;
                    if (session == CONTROL)
                        csm_in(pl, pn);
                    else if (session == FILES)
                        ft_in(pl, pn);
                }
                if (g_l.peer_acks)
                    link_packet(0x40, g_l.sent_seq, 0, 0, 0);
            }
        }
        memmove(g_l.in, g_l.in + len, g_l.inn - len);
        g_l.inn -= len;
    }
}

int iap2_run(struct stream* s, const struct iap2_carplay* cp)
{
    static const unsigned char marker[6] = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
    static const unsigned char syn[19] = {1, 4, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 10, 0, 2, 11, 2, 1, 12, 1, 2};
    const long long t0 = ms_now();
    int resent = 0;
    memset(&g_l, 0, sizeof(g_l));
    g_l.s = s;
    g_l.cp = cp;
    g_l.sent_seq = 99;
    if (acp_open())
        return -1;
    if (stream_write(s, marker, sizeof(marker)) || link_packet(0x80, g_l.sent_seq, 0, syn, sizeof(syn)))
        return -1;
    while (!g_stop && !g_l.failed && !usb_gone()) {
        unsigned char b[16384];
        const int r = stream_read_some(s, b, sizeof(b), 300);
        if (r < 0) {
            say("iap2: the carkit connection ended");
            break;
        }
        if (r > 0) {
            if ((unsigned)r > sizeof(g_l.in) - g_l.inn)
                g_l.inn = 0;
            memcpy(g_l.in + g_l.inn, b, (unsigned)r);
            g_l.inn += (unsigned)r;
            link_in();
        }
        /* nbt_cluster shows the guidance only while nav.state is younger than 15 s; the iPhone sends nothing while
         * nothing changes (standing at a light), so the file is written again every 5 s (car 2026-11-24: the arrow
         * went on and off) */
        if (GUIDING && ms_now() - g_rg.written > 5000)
            nav_write();
        if (!g_l.synced && !resent && ms_now() - t0 > 5000) {
            resent = 1;
            link_packet(0x80, g_l.sent_seq, 0, syn, sizeof(syn));
            say("iap2: SYN again");
        }
        if (!g_l.identified && ms_now() - t0 > 60000) {
            say("iap2: no identification within 60 s");
            break;
        }
    }
    say("iap2: over (identification %s, authentication %s, %d CarPlay start(s))",
        g_l.identified ? "accepted" : "not accepted", g_l.authenticated ? "succeeded" : "not succeeded",
        g_l.carplay_sent);
    flag_clear("/fs/ram3/nbt-aa-media");
    unlink(MEDIA_STATE);
    unlink(MEDIA_COVER);
    unlink(NAV_STATE);
    return g_l.authenticated ? 0 : -1;
}
