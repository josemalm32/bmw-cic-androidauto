/* ---------------- audio + video live loop ----------------
 * Audio: phone AAC-LC access units from ring channel 0 -> DSP M4AC stream.
 * Silent AAC frames are sent whenever the phone has no audio queued, because the
 * DSP only starts its clock on "Audio Pipe ES buffer is Full".
 * (PCM PCMA/DVDA code paths are kept but unused: the DSP refused them in the car.) */
#include "aa_aring.h"
#define AFR_US 21333u
#define PFR_SAMPLES 960u                 /* 20 ms @ 48 kHz */
#define PFR_US 20000u
enum { FMT_PCMA = 1, FMT_DVDA = 2, FMT_AAC = 3, FMT_PCMB = 4 };  /* PCMB = PCMA byteswapped (manual only) */
static const char *fmt_name(int f) { return f == FMT_PCMA ? "PCMA" : f == FMT_DVDA ? "DVDA" : f == FMT_PCMB ? "PCMB" : "M4AC"; }
static int g_fmt = FMT_AAC;
static int g_vmode;                /* 0 PTS+vlead, 1 no PTS, 2 PTS -1 (experimental) */
static uint64_t g_apts;            /* next audio PTS (us, DSP timeline) */
static unsigned long g_aframes, g_amix_media, g_amix_voice, g_aunder;
static aring_hdr *g_ar;
static struct { uint32_t rpos, gen; int primed; int16_t last; } g_ac[3];

static aring_hdr *aring_open(void)
{
    int fd = shm_open(ARING_NAME, O_RDWR | O_CREAT, 0666); void *m; aring_hdr *h;
    if (fd < 0) return 0;
    if (ftruncate(fd, (off_t)ARING_BYTES)) { close(fd); return 0; }
    m = mmap(0, ARING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); close(fd);
    if (m == MAP_FAILED) return 0;
    h = m;
    if (h->magic != ARING_MAGIC || h->chsize != ARING_CH) { memset(h, 0, sizeof *h); h->chsize = ARING_CH; h->magic = ARING_MAGIC; }
    return h;
}
/* read n bytes of channel c into out (zero-filled on underrun); returns bytes of real data */
static uint32_t aring_take(int c, uint8_t *out, uint32_t n, uint32_t prime, uint32_t maxlag)
{
    aring_hdr *h = g_ar; uint32_t w, avail, got, k = 0; uint8_t *d;
    memset(out, 0, n);
    if (!h) return 0;
    w = h->wpos[c];
    if (g_ac[c].gen != h->gen[c]) { g_ac[c].gen = h->gen[c]; g_ac[c].primed = 0; }
    avail = w - g_ac[c].rpos;
    if (avail > ARING_CH - 8192u || avail > maxlag) {       /* too far behind: keep newest 'prime' bytes */
        g_ac[c].rpos = w - (prime & ~3u); avail = w - g_ac[c].rpos;
    }
    if (!g_ac[c].primed) { if (avail < prime) return 0; g_ac[c].primed = 1; }
    d = aring_data(h, c);
    got = avail < n ? avail : n;
    while (k < got) {
        uint32_t o = (g_ac[c].rpos + k) & (ARING_CH - 1), m = ARING_CH - o;
        if (m > got - k) m = got - k;
        memcpy(out + k, d + o, m); k += m;
    }
    g_ac[c].rpos += got;
    if (got < n) { g_ac[c].primed = 0; g_aunder++; }
    return got;
}
static int16_t sat16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static void mix_frame(int16_t *out /* PFR_SAMPLES*2 */)
{
    static int16_t med[PFR_SAMPLES * 2]; static int16_t v1[PFR_SAMPLES / 3], v2[PFR_SAMPLES / 3];
    uint32_t gm, g1, g2, i; int voice, gain;
    gm = aring_take(0, (uint8_t *)med, sizeof med, 48000u * 4u / 10u, 48000u * 4u * 4u / 10u);   /* prime 100 ms, max lag 400 ms */
    g1 = aring_take(1, (uint8_t *)v1, sizeof v1, 16000u * 2u / 10u, 16000u * 2u * 4u / 10u);
    g2 = aring_take(2, (uint8_t *)v2, sizeof v2, 16000u * 2u / 10u, 16000u * 2u * 4u / 10u);
    voice = g1 || g2; gain = voice ? 77 : 256;               /* duck media to 30 % under voice */
    if (gm) g_amix_media++;
    if (voice) g_amix_voice++;
    for (i = 0; i < PFR_SAMPLES; i++) {
        uint32_t j = i / 3, k = i % 3; int a, b, v;
        a = (j ? v1[j - 1] : g_ac[1].last); b = v1[j]; v = a + (b - a) * (int)(k + 1) / 3;
        a = (j ? v2[j - 1] : g_ac[2].last); b = v2[j]; v += a + (b - a) * (int)(k + 1) / 3;
        out[2 * i]     = sat16(med[2 * i] * gain / 256 + v);
        out[2 * i + 1] = sat16(med[2 * i + 1] * gain / 256 + v);
    }
    g_ac[1].last = v1[PFR_SAMPLES / 3 - 1]; g_ac[2].last = v2[PFR_SAMPLES / 3 - 1];
}
static int send_audio_header(void)
{
    static const unsigned char asc[5] = {0x11, 0x90, 0x56, 0xe5, 0x00};
    if (g_fmt == FMT_AAC) {
        if (prop(121, 4, 0) || prop(121, 0x20, 0x80000004u) || prop(121, 0x21, 0x4d344143u) ||
            prop(121, 0x22, 2) || prop(121, 0x23, 48000) || prop(30, 0, 1) || data(asc, 5)) return -1;
    } else {
        uint32_t fc = g_fmt == FMT_DVDA ? 0x44564441u : 0x50434d41u;   /* DVDA : PCMA */
        if (prop(121, 4, 0) || prop(121, 0x20, 0x80000004u) || prop(121, 0x21, fc) ||
            prop(121, 0x22, 2) || prop(121, 0x23, 48000)) return -1;
    }
    lg("AUDIO HEADER %s 48k 2ch", fmt_name(g_fmt));
    return 0;
}
/* v55: one 2048-byte DVD pack: 14 pack hdr + 6 PES hdr + 8 PTS/ext + 4 private hdr + 3 LPCM hdr + 1920 PCM + padding PES */
#define PACK_BYTES (PFR_SAMPLES * 2u)   /* half a 20 ms frame: 480 stereo 16-bit samples */
static unsigned g_lpcm_fn;
static void put_ts(unsigned char *p, unsigned pre, uint64_t t)   /* 33-bit PTS, 5 bytes */
{
    p[0] = (unsigned char)(pre | ((t >> 29) & 0x0e) | 1); p[1] = (unsigned char)(t >> 22);
    p[2] = (unsigned char)(((t >> 14) & 0xfe) | 1); p[3] = (unsigned char)(t >> 7); p[4] = (unsigned char)(((t << 1) & 0xfe) | 1);
}
static int dvd_lpcm_pack(const unsigned char *pcm, uint64_t pts_us)
{
    static unsigned char pb[2048]; unsigned char *p = pb; uint64_t pts = pts_us * 9u / 100u;   /* 90 kHz, same base as prop(15) / video PTS 0 */
    uint64_t scr = pts > 18000u ? pts - 18000u : 0; unsigned plen, pad;
    memset(pb, 0xff, sizeof pb);
    /* pack header MPEG-2 */
    p[0] = 0; p[1] = 0; p[2] = 1; p[3] = 0xba;
    p[4] = (unsigned char)(0x44 | ((scr >> 27) & 0x38) | ((scr >> 28) & 3));
    p[5] = (unsigned char)(scr >> 20);
    p[6] = (unsigned char)(((scr >> 12) & 0xf8) | 4 | ((scr >> 13) & 3));
    p[7] = (unsigned char)(scr >> 5);
    p[8] = (unsigned char)(((scr << 3) & 0xf8) | 4); p[9] = 1;            /* SCR ext 0 */
    p[10] = 0x01; p[11] = 0x89; p[12] = 0xc3;                             /* mux rate 10.08 Mbps */
    p[13] = 0xf8;                                                         /* no stuffing */
    p += 14;
    /* PES private_stream_1 */
    plen = 3 + 5 + 4 + 3 + PACK_BYTES;
    p[0] = 0; p[1] = 0; p[2] = 1; p[3] = 0xbd; p[4] = (unsigned char)(plen >> 8); p[5] = (unsigned char)plen;
    p[6] = 0x81; p[7] = 0x80; p[8] = 5; put_ts(p + 9, 0x20, pts);
    p += 14;
    /* private header: substream A0, 1 frame header, first AU ptr, LPCM hdr */
    p[0] = 0xa0; p[1] = 1; p[2] = 0; p[3] = 4;
    p[4] = (unsigned char)(g_lpcm_fn++ & 0x1f); p[5] = 0x01; p[6] = 0x80;   /* 16 bit, 48 kHz, 2 ch, DRC neutral */
    p += 7;
    memcpy(p, pcm, PACK_BYTES); p += PACK_BYTES;
    pad = (unsigned)(pb + sizeof pb - p);
    if (pad >= 6) { p[0] = 0; p[1] = 0; p[2] = 1; p[3] = 0xbe; p[4] = (unsigned char)((pad - 6) >> 8); p[5] = (unsigned char)(pad - 6); }
    if (DATA_MAX - pk.dlen < sizeof pb && pkt_flush()) return -1;   /* keep each pack whole in one buffer */
    return data(pb, sizeof pb);
}
/* v56: next AAC access unit from ch0 records [u16 LE len][AU]. 0 = none (send silence). */
static unsigned long g_aac_real, g_aac_sil, g_aac_skip;
static uint32_t ar_u16(uint32_t pos) { uint8_t *d = aring_data(g_ar, 0); return d[pos & (ARING_CH - 1)] | (uint32_t)d[(pos + 1) & (ARING_CH - 1)] << 8; }
static uint32_t aac_take(uint8_t *out, uint32_t max)
{
    aring_hdr *h = g_ar; uint32_t w, avail, len, k = 0; uint8_t *d;
    if (!h || h->codec0 != 2) return 0;
    w = h->wpos[0];
    if (g_ac[0].gen != h->gen[0]) { g_ac[0].gen = h->gen[0]; g_ac[0].rpos = w; g_ac[0].primed = 0; return 0; }
    avail = w - g_ac[0].rpos;
    if (avail > ARING_CH - 16384u) { g_ac[0].rpos = w; g_ac[0].primed = 0; g_aac_skip++; return 0; }   /* overrun: restart at boundary */
    /* lag control: keep at most ~16 KB (~0.3 s at 128 kbps... ~0.6 s at 256) queued */
    while (avail > 98304u) { len = ar_u16(g_ac[0].rpos); if (len + 2 > avail) break; g_ac[0].rpos += len + 2; avail -= len + 2; g_aac_skip++; }
    if (!g_ac[0].primed) { if (avail < 24000u) return 0; g_ac[0].primed = 1; }   /* v57b ~0.35 s cushion; was ~3 AUs of jitter cushion */
    if (avail < 2) { g_ac[0].primed = 0; g_aunder++; return 0; }
    len = ar_u16(g_ac[0].rpos);
    if (len + 2 > avail) { g_ac[0].primed = 0; g_aunder++; return 0; }
    if (!len || len > max) { g_ac[0].rpos += len + 2; return 0; }
    d = aring_data(h, 0);
    while (k < len) { uint32_t o = (g_ac[0].rpos + 2 + k) & (ARING_CH - 1), m = ARING_CH - o; if (m > len - k) m = len - k; memcpy(out + k, d + o, m); k += m; }
    g_ac[0].rpos += len + 2;
    if (len > 7 && out[0] == 0xff && (out[1] & 0xf6) == 0xf0) {   /* ADTS -> raw AU */
        uint32_t hl = (out[1] & 1) ? 7 : 9; memmove(out, out + hl, len - hl); len -= hl;
    }
    return len;
}
/* v57b: 1 if a real AU can be taken now (or no AAC stream), without consuming */
static int aac_has_au(void)
{
    aring_hdr *h = g_ar; uint32_t avail;
    if (!h || h->codec0 != 2) return 1;
    if (g_ac[0].gen != h->gen[0]) return 0;
    avail = h->wpos[0] - g_ac[0].rpos;
    if (!g_ac[0].primed) return avail >= 24000u;
    return avail >= 2 && ar_u16(g_ac[0].rpos) + 2 <= avail;
}
/* silent != 0: prefill silence */
static int send_audio_frame(int silent)
{
    if (prop(121, 4, 0)) return -1;
    if (!g_aframes && prop(9, 0, 0x8fa5)) return -1;
    if (prop(15, 0, (uint32_t)g_apts)) return -1;
    if (g_fmt == FMT_AAC) {
        static uint8_t au[4096]; uint32_t n = silent ? 0 : aac_take(au, sizeof au);
        if (n) { if (data(au, n)) return -1; if (!g_aac_real++) lg("V56 first real AAC AU %u bytes: %02x %02x %02x %02x", (unsigned)n, au[0], au[1], au[2], au[3]); }
        else { if (data(aac_sil, sizeof aac_sil)) return -1; g_aac_sil++; }
        g_apts += AFR_US;
    } else {
        static int16_t pcm[PFR_SAMPLES * 2]; uint32_t i;
        if (silent) memset(pcm, 0, sizeof pcm); else mix_frame(pcm);
        if (g_fmt == FMT_DVDA || g_fmt == FMT_PCMB) for (i = 0; i < PFR_SAMPLES * 2; i++) { uint16_t x = (uint16_t)pcm[i]; pcm[i] = (int16_t)((x >> 8) | (x << 8)); }
        if (g_fmt == FMT_DVDA) {   /* v55: DVD-Video LPCM packs (CheckForAudioPack: BA pack + BD PES + substream A0) */
            uint32_t h;
            for (h = 0; h < 2; h++) if (dvd_lpcm_pack((const unsigned char *)pcm + h * PACK_BYTES, g_apts + h * (PFR_US / 2))) return -1;
        } else if (data((const unsigned char *)pcm, sizeof pcm)) return -1;
        g_apts += PFR_US;
    }
    g_aframes++;
    return 0;
}

/* ---------------- ring reader ---------------- */
static ring_hdr *ring_open(void)
{
    int fd = shm_open(RING_NAME, O_RDWR | O_CREAT, 0666); void *m; ring_hdr *r;
    if (fd < 0) return 0;
    if (ftruncate(fd, (off_t)RING_BYTES)) { close(fd); return 0; }
    m = mmap(0, RING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); close(fd);
    if (m == MAP_FAILED) return 0;
    r = m;
    if (r->magic != RING_MAGIC) { memset(r, 0, sizeof *r); r->size = RING_DATA; r->magic = RING_MAGIC; }
    return r;
}

/* one DSP session with format g_fmt. 0 normal end, 1 error, 2 audio format refused (try next) */
static int live_once(ring_hdr *r, uint32_t vlead)
{
    uint32_t cfg_gen = 0; int have_hdr = 0, waiting_idr = 1, rc = 0, prefilled = 0;
    uint64_t alead = 300000u, last_idr_req = 0, last_stat = 0; unsigned long frames = 0, bytes = 0, skipped = 0;
    unsigned char *rbuf = (unsigned char *)(r + 1);
    pid_t parent = getppid();
    d.h = d.dbg = d.cmd = d.strm = 0; d.have_mem = 0;
    d.buf_back = d.started = d.stopped = d.errors = d.fatal = 0; d.mra = 0;
    g_clk = 0; g_afull = 0; g_resync = 0; g_apts = 0; g_aframes = 0;
    memset(g_ac, 0, sizeof g_ac);
    if (g_ar) { int c; for (c = 0; c < 3; c++) { g_ac[c].rpos = g_ar->wpos[c]; g_ac[c].gen = g_ar->gen[c]; } }
    r->rpos = r->wpos; r->want_idr = 1;
    lg("=== DSP session audio=%s ===", fmt_name(g_fmt));
    if (dsp_open() || dsp_start()) { lg("DSP init failed"); dsp_close(); return 1; }
    pkt_reset();
    while (!stop_requested() && !d.fatal) {
        uint32_t rp, wp; ring_rec h; uint64_t now;
        if (getppid() != parent) { lg("parent gone -> exit"); break; }
        rp = r->rpos; wp = r->wpos; now = now_us();
        if (g_resync) {                    /* v53: EVT_STREAM_ERROR (video, non fatal) -> resend header, wait IDR */
            g_resync = 0; cfg_gen = 0; waiting_idr = 1; r->want_idr = 1; last_idr_req = now;
            lg("STREAM ERROR -> resync: header + wait IDR (errors=%d)", d.errors);
        }
        if (g_clk && prefilled) {          /* audio at real-time rate, lead alead */
            int na = 0;
            while (g_apts < (now - g_clk_us) + alead && na < 40) {
                /* v57b: no real AU yet -> wait while DSP still has >80 ms queued (no silence holes / drift) */
                if (g_fmt == FMT_AAC && g_aac_real && !aac_has_au() && g_apts > (now - g_clk_us) + 80000u) break;
                if (send_audio_frame(0)) { rc = 1; break; } na++; }
            if (rc) break;
            if (na && pkt_flush()) { rc = 1; break; }
        }
        if (r->cfg_len && r->cfg_gen != cfg_gen) {          /* new SPS/PPS */
            uint8_t cfg[RING_CFGMAX]; uint32_t n = r->cfg_len, g = r->cfg_gen;
            if (n <= RING_CFGMAX) {
                memcpy(cfg, r->cfg, n); cfg_gen = g;
                if (send_header(cfg, n, r->width ? r->width : 800, r->height ? r->height : 480) || pkt_flush()) { rc = 1; break; }
                have_hdr = 1; waiting_idr = 1;
            }
        }
        if (now - last_stat > 5000000u) {
            last_stat = now;
            lg("STAT frames=%lu bytes=%lu skipped=%lu ring_in=%u ring_drop=%u dsp_err=%d | audio %s fr=%lu media=%lu voice=%lu under=%lu aw=%u/%u/%u act=%u%u%u | aac codec=%u real=%lu sil=%lu skip=%lu",
               frames, bytes, skipped, (unsigned)r->frames_in, (unsigned)r->frames_drop, d.errors, fmt_name(g_fmt),
               g_aframes, g_amix_media, g_amix_voice, g_aunder,
               g_ar ? (unsigned)g_ar->wpos[0] : 0, g_ar ? (unsigned)g_ar->wpos[1] : 0, g_ar ? (unsigned)g_ar->wpos[2] : 0,
               g_ar ? (unsigned)g_ar->active[0] : 0, g_ar ? (unsigned)g_ar->active[1] : 0, g_ar ? (unsigned)g_ar->active[2] : 0,
               g_ar ? (unsigned)g_ar->codec0 : 0, g_aac_real, g_aac_sil, g_aac_skip);
        }
        if (rp == wp) {
            if (waiting_idr && now - last_idr_req > 3000000u) { r->want_idr = 1; last_idr_req = now; }
            usleep(2000); continue;
        }
        if (r->size - rp < sizeof h) { r->rpos = 0; continue; }
        memcpy(&h, rbuf + rp, sizeof h);
        if (h.kind == REC_WRAP) { r->rpos = 0; continue; }
        if (h.kind != REC_FRAME || h.len > r->size / 2) { lg("ring corrupt kind=%u len=%u; resync", (unsigned)h.kind, (unsigned)h.len); r->rpos = r->wpos; waiting_idr = 1; continue; }
        if (!have_hdr || (waiting_idr && !h.idr)) skipped++;
        else {
            uint32_t pts = (g_clk && h.pts_us > g_clk_us) ? (uint32_t)(h.pts_us - g_clk_us) + vlead : 0;
            if (frames == 0) { if (prop(121, 3, 0) || prop(9, 0, 0x8fa5) || prop(14, 0, 0)) { rc = 1; break; } }
            else if (g_vmode == 1 && g_clk) { if (prop(121, 3, 0)) { rc = 1; break; } }          /* free-run: no PTS */
            else if (prop(121, 3, 0) || prop(14, 0, g_vmode == 2 && g_clk ? 0xffffffffu : pts)) { rc = 1; break; }
            if (data(rbuf + rp + sizeof h, h.len) || pkt_flush()) { rc = 1; break; }
            if (waiting_idr) lg("FIRST IDR sent len=%u", (unsigned)h.len);
            if (!g_clk && !prefilled) {
                uint64_t tw;
                prefilled = 1;
                /* audio prefill (silence) until audio pipe full / clock start (cap 8 s PTS) */
                if (send_audio_header()) { rc = 1; break; }
                while (!g_clk && !d.fatal && g_apts < 8000000u && !stop_requested()) {
                    int k;
                    for (k = 0; k < 10; k++) if (send_audio_frame(1)) { rc = 1; break; }
                    if (rc || pkt_flush()) { rc = 1; break; }
                    if (g_afull) usleep(20000);
                }
                tw = now_us();
                while (!g_clk && !d.fatal && now_us() - tw < 2000000u) usleep(20000);
                lg("PREFILL AUDIO %s frames=%lu apts=%u ms afull=%d clock=%d fatal=%d errors=%d rc=%d", fmt_name(g_fmt), g_aframes,
                   (unsigned)(g_apts / 1000), g_afull, g_clk, d.fatal, d.errors, rc);
                if (!g_clk) {
                    if (g_fmt != FMT_AAC) { lg("audio %s refused (no clock) -> next format", fmt_name(g_fmt)); rc = 2; break; }
                    if (rc) break;
                    g_clk_us = now_us(); g_clk = 1; lg("no clock msg -> assume started now");
                }
            }
            waiting_idr = 0; frames++; bytes += h.len;
        }
        __asm__ __volatile__("" ::: "memory");
        r->rpos = (rp + (uint32_t)((sizeof h + h.len + 3) & ~3u)) % r->size;
    }
    lg("LIVE end stop=%d fatal=%d frames=%lu bytes=%lu skipped=%lu rc=%d", stop_requested(), d.fatal, frames, bytes, skipped, rc);
    dsp_close();
    return rc;
}

static int live(int secs_unused)
{
    ring_hdr *r; int rc = 1, f, order[4], n = 0, i; uint32_t vlead = AAP_VLEAD_MS * 1000u;
    (void)secs_unused;
    /* vlead_ms.txt: a number (50..2000 ms), or EXPERIMENTAL "nopts" / "pts-1":
     * once the clock runs, video frames carry no PTS (or PTS -1). The DSP then takes
     * avSyncShowFrame's "Show Vid no pts" path (pts == -1 -> shown as soon as decoded,
     * never "late"/skipped): lowest latency, no A/V sync (AA does not need it). */
    { FILE *fp = fopen(AAP_VLEAD_TXT, "r"); char w[16]; unsigned v;
      if (fp) {
          if (fscanf(fp, "%15s", w) == 1) {
              if (!strcmp(w, "nopts")) g_vmode = 1;
              else if (!strcmp(w, "pts-1")) g_vmode = 2;
              else if (sscanf(w, "%u", &v) == 1 && v >= 50 && v <= 2000) vlead = v * 1000u;
          }
          fclose(fp);
      } }
    lg("video lead %u ms mode=%s", (unsigned)(vlead / 1000), g_vmode == 1 ? "nopts" : g_vmode == 2 ? "pts-1" : "pts");
    r = ring_open(); if (!r) { lg("ring open failed errno=%d", errno); return 1; }
    r->reader_pid = (uint32_t)getpid();
    g_ar = aring_open();
    lg("RING attached size=%u cfg_len=%u frames_in=%u drop=%u %ux%u audio_ring=%s", (unsigned)r->size, (unsigned)r->cfg_len,
       (unsigned)r->frames_in, (unsigned)r->frames_drop, (unsigned)r->width, (unsigned)r->height, g_ar ? "ok" : "FAILED");
    (void)f; order[n++] = FMT_AAC;   /* phone AAC passthrough (silent AAC until the phone sends audio) */
    if (!g_ar) { n = 0; order[n++] = FMT_AAC; }
    for (i = 0; i < n && !stop_requested(); i++) {
        g_fmt = order[i];
        rc = live_once(r, vlead);
        if (rc == 1 && g_fmt == FMT_AAC) { int k; for (k = 0; k < 3 && rc == 1 && !stop_requested(); k++) { lg("AAC init retry %d", k + 1); sleep(1); rc = live_once(r, vlead); } }
        if (rc != 2) break;
        usleep(300000);
    }
    r->reader_pid = 0;
    return rc == 2 ? 1 : rc;
}

