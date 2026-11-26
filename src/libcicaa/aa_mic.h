/* Car microphone -> Android Auto (AV input, channel 7) and AA voice (channel 5) -> CIC speech
 * output. Included by aa_audio.h. Tested on CIC HIGH C16A (E9x LCI) as a separate module first
 * (libcicmic, 1.2c-1.5); this is the same logic inside libcicaa.
 *
 * Car findings (details: docs/hallazgos):
 * - The car microphone is io-audio capture device 0 ("FACBMIC", 4 voices); speech is on voice 2.
 *   It only carries signal while audio connection 0x31 is active (the one BMW voice control
 *   requests). The plugin switches 0x31 on the HMI thread; libcicaa only says what it wants
 *   (cicaa_speech_want()).
 * - When 0x31 comes up, BMW's speech service (scp) opens the capture device too, and the first
 *   opener wins. So capture is opened when the AA picture starts (cicaa_mic_hold(1) from the
 *   plugin) and kept open until it stops.
 * - Channel 5 (Assistant answers, navigation prompts) is 16 kHz mono PCM. The DSP path only
 *   takes AAC media, so it was never audible. It is played on io-audio playback device 1
 *   ("FABSDS", the BMW prompt output), which is audible while 0x31 is active.
 * - Channel 5 arrives faster than real time and per utterance (AV start/stop): keep 0x31 until
 *   everything is played; hold 4 s after the microphone closes so the answer does not need
 *   another C4 <-> 0x31 switch (1-2 s each); prebuffer 0.35 s per utterance. */
#ifdef CICAA_HOST_TEST
/* Host test build: no io-audio. Same behaviour as before the microphone: refuse the open. */
static volatile int g_speech_want;
static void mic_hold(int on) { (void)on; }
static void mic_poll(aa_t *a) { (void)a; }
static int microphone_dispatch(aa_t *a, int id, const unsigned char *b, size_t n)
{
    unsigned long open; int present; pb_t r;
    if (id != 0x8005) return 0;
    if (audio_uint(b, n, 1, &open, &present) || open > 1) return 1;
    if (!open) return 0;
    memset(&r, 0, sizeof r); pb_u(&r, 1, 0); pb_u(&r, 2, 1);
    return send_msg(a, 7, 0, 0x8006, r.b, r.n) != 0;
}
#else
#include <pthread.h>
#include <unistd.h>
#include <sys/asoundlib.h>
static unsigned long long mono_us(void);

#define MIC_CHUNK   1024u          /* bytes per AA packet: 512 samples = 32 ms at 16 kHz */
#define MIC_RING    (64u * 1024u)  /* 2 s of 16 kHz mono */
#define MIC_VOICES  4
#define MIC_VOICE   2              /* FACBMIC voice that carries the microphone */
#define VOICE_DEV   1              /* io-audio playback device FABSDS */
#define VOICE_PREFILL 11200u       /* 0.35 s of 16 kHz mono */

static pthread_mutex_t g_mic_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_mic_hold, g_mic_cap_run, g_mic_cap_alive, g_mic_on, g_mic_unacked, g_mic_maxun = 2;
static volatile int g_speech_want, g_play_run, g_play_alive, g_play_prefill;
static volatile unsigned g_play_rpos, g_last_play_ms, g_mic_hold_until, g_play_prefill_ms;
static unsigned char g_mic_ring[MIC_RING];
static volatile unsigned g_mic_w, g_mic_r;
static unsigned long g_mic_sent, g_mic_acks;
static aa_log_fn g_mic_lg;

static unsigned mic_ms(void) { return (unsigned)(mono_us() / 1000ULL); }

/* ---------- voice output: aring channel 1 (AA channel 5) -> FABSDS ---------- */
static void *voice_thread(void *arg)
{
    snd_pcm_t *pcm = 0; snd_pcm_channel_params_t pp; snd_pcm_channel_setup_t su;
    static unsigned char buf[4096]; unsigned rpos = g_play_rpos, played = 0; int frag, errs = 0;
    (void)arg;
    if (!g_aring || snd_pcm_open(&pcm, 0, VOICE_DEV, SND_PCM_OPEN_PLAYBACK) < 0) {
        if (g_mic_lg) g_mic_lg("voice: playback device %d not available", VOICE_DEV);
        g_play_alive = 0; return 0;
    }
    snd_pcm_plugin_set_disable(pcm, PLUGIN_DISABLE_MMAP);
    memset(&pp, 0, sizeof pp);
    pp.channel = SND_PCM_CHANNEL_PLAYBACK; pp.mode = SND_PCM_MODE_BLOCK;
    pp.format.interleave = 1; pp.format.format = SND_PCM_SFMT_S16_LE;
    pp.format.rate = 16000; pp.format.voices = 1;
    pp.start_mode = SND_PCM_START_DATA; pp.stop_mode = SND_PCM_STOP_STOP;
    pp.buf.block.frag_size = 1024; pp.buf.block.frags_min = 1; pp.buf.block.frags_max = 12;  /* ~0.38 s */
    if (snd_pcm_plugin_params(pcm, &pp) < 0 || snd_pcm_plugin_prepare(pcm, SND_PCM_CHANNEL_PLAYBACK) < 0) {
        if (g_mic_lg) g_mic_lg("voice: playback params refused");
        snd_pcm_close(pcm); g_play_alive = 0; return 0;
    }
    memset(&su, 0, sizeof su); su.channel = SND_PCM_CHANNEL_PLAYBACK;
    snd_pcm_plugin_setup(pcm, &su);
    frag = su.buf.block.frag_size;
    if (frag <= 0 || frag > (int)sizeof buf) frag = 1024;
    frag &= ~1;
    g_play_prefill = 1; g_play_prefill_ms = mic_ms();
    if (g_mic_lg) g_mic_lg("voice: AA speech -> playback device %d (frag %d)", VOICE_DEV, frag);
    while (g_play_run || g_aring->wpos[1] - rpos >= (unsigned)frag) {
        unsigned w = g_aring->wpos[1], i;
        const unsigned char *d = aring_data((aring_hdr *)g_aring, 1);
        if (w - rpos > ARING_CH) rpos = w - ARING_CH / 2;               /* fell behind */
        if (g_play_prefill && w - rpos < VOICE_PREFILL && g_aring->active[1] && mic_ms() - g_play_prefill_ms < 600) {
            usleep(10000); continue;
        }
        g_play_prefill = 0;
        if (w - rpos < (unsigned)frag) {
            if (w != rpos && !g_aring->active[1]) {                      /* last partial block + silence */
                unsigned k = w - rpos;
                for (i = 0; i < (unsigned)frag; i++) buf[i] = i < k ? d[(rpos + i) & (ARING_CH - 1)] : 0;
                rpos = w; g_play_rpos = rpos;
                if (snd_pcm_plugin_write(pcm, buf, frag) < frag) { snd_pcm_plugin_prepare(pcm, SND_PCM_CHANNEL_PLAYBACK); snd_pcm_plugin_write(pcm, buf, frag); }
                g_last_play_ms = mic_ms(); played += k;
            }
            if (w == rpos && mic_ms() - g_last_play_ms > 700) { g_play_prefill = 1; g_play_prefill_ms = mic_ms(); }
            usleep(10000); continue;
        }
        for (i = 0; i < (unsigned)frag; i++) buf[i] = d[(rpos + i) & (ARING_CH - 1)];
        rpos += frag; g_play_rpos = rpos;
        if (snd_pcm_plugin_write(pcm, buf, frag) < frag) {              /* underrun: same block again */
            snd_pcm_plugin_prepare(pcm, SND_PCM_CHANNEL_PLAYBACK);
            if (snd_pcm_plugin_write(pcm, buf, frag) < frag) errs++;
        }
        g_last_play_ms = mic_ms(); played += frag;
    }
    snd_pcm_plugin_playback_drain(pcm);
    snd_pcm_close(pcm);
    if (g_mic_lg) g_mic_lg("voice: stopped, %u bytes played, %d write errors", played, errs);
    g_play_alive = 0;
    return 0;
}
static void voice_start(void)
{
    pthread_t t; int i;
    if (g_play_run || !g_aring) return;
    for (i = 0; i < 50 && g_play_alive; i++) usleep(10000);
    if (g_play_alive) return;
    g_play_run = 1; g_play_alive = 1;
    if (pthread_create(&t, 0, voice_thread, 0)) { g_play_run = 0; g_play_alive = 0; return; }
    pthread_detach(t);
}

/* Speech mode, evaluated after every capture read (~64 ms): 0x31 wanted while the phone has the
 * microphone open, while AA voice is playing (and shortly after), and 4 s after the microphone
 * closed (the answer follows without another source switch). */
static unsigned g_last_w1, g_last_voice_ms;
static void speech_tick(void)
{
    unsigned now = mic_ms(); int voice = 0, want, hold;
    if (g_aring) {
        unsigned w1 = g_aring->wpos[1];
        if (w1 != g_last_w1) { g_last_w1 = w1; g_last_voice_ms = now; }
        voice = g_aring->active[1] || (g_play_run && w1 != g_play_rpos) ||
                now - g_last_voice_ms < 1000 || now - g_last_play_ms < 600;
    }
    hold = (int)(g_mic_hold_until - now) > 0;
    want = g_mic_on || voice || hold;
    if (want && !g_speech_want) {
        if (g_aring && (g_aring->wpos[1] - g_play_rpos > 64000u || (!g_aring->active[1] && now - g_last_voice_ms > 1500)))
            g_play_rpos = g_aring->wpos[1];                              /* only fresh, unplayed speech */
        g_speech_want = 1; if (g_mic_lg) g_mic_lg("speech mode ON"); voice_start();
    } else if (!want && g_speech_want) {
        g_speech_want = 0; if (g_mic_lg) g_mic_lg("speech mode OFF"); g_play_run = 0;
    }
}

/* ---------- capture: FACBMIC voice 2 -> mono ring ---------- */
static void *mic_thread(void *arg)
{
    snd_pcm_t *pcm = 0; snd_pcm_channel_params_t pp; snd_pcm_channel_setup_t su;
    static short in[4096]; int frag, r; unsigned long total = 0;
    (void)arg;
    if (snd_pcm_open(&pcm, 0, 0, SND_PCM_OPEN_CAPTURE) < 0) {
        if (g_mic_lg) g_mic_lg("mic: capture device busy/unavailable");
        g_mic_cap_run = 0; g_mic_cap_alive = 0; return 0;
    }
    snd_pcm_plugin_set_disable(pcm, PLUGIN_DISABLE_MMAP);
    memset(&pp, 0, sizeof pp);
    pp.channel = SND_PCM_CHANNEL_CAPTURE; pp.mode = SND_PCM_MODE_BLOCK;
    pp.format.interleave = 1; pp.format.format = SND_PCM_SFMT_S16_LE;
    pp.format.rate = 16000; pp.format.voices = MIC_VOICES;
    pp.start_mode = SND_PCM_START_DATA; pp.stop_mode = SND_PCM_STOP_STOP;
    pp.buf.block.frag_size = 2048; pp.buf.block.frags_min = 1; pp.buf.block.frags_max = -1;
    if (snd_pcm_plugin_params(pcm, &pp) < 0 || snd_pcm_plugin_prepare(pcm, SND_PCM_CHANNEL_CAPTURE) < 0) {
        if (g_mic_lg) g_mic_lg("mic: capture params refused");
        snd_pcm_close(pcm); g_mic_cap_run = 0; g_mic_cap_alive = 0; return 0;
    }
    memset(&su, 0, sizeof su); su.channel = SND_PCM_CHANNEL_CAPTURE;
    snd_pcm_plugin_setup(pcm, &su);
    frag = su.buf.block.frag_size;
    if (frag <= 0 || frag > (int)sizeof in) frag = 2040;
    frag -= frag % (MIC_VOICES * 2);
    if (g_mic_lg) g_mic_lg("mic: capture started (frag %d)", frag);
    while (g_mic_cap_run) {
        int i, n;
        r = snd_pcm_plugin_read(pcm, in, frag);
        if (r <= 0) { snd_pcm_plugin_prepare(pcm, SND_PCM_CHANNEL_CAPTURE); usleep(20000); continue; }
        n = r / (MIC_VOICES * 2);
        for (i = 0; i < n; i++) {                                        /* voice 2 -> ring */
            short s = in[i * MIC_VOICES + MIC_VOICE];
            unsigned w = g_mic_w;
            g_mic_ring[w % MIC_RING] = (unsigned char)s; g_mic_ring[(w + 1) % MIC_RING] = (unsigned char)(s >> 8);
            g_mic_w = w + 2;
        }
        total += (unsigned long)r;
        speech_tick();
    }
    g_mic_on = 0; g_speech_want = 0; g_play_run = 0;
    snd_pcm_close(pcm);
    if (g_mic_lg) g_mic_lg("mic: capture stopped, %lu bytes read", total);
    g_mic_cap_alive = 0;
    return 0;
}

/* Plugin: AA picture shown (1) / gone (0). Capture is held for the whole picture time. */
static void mic_hold(int on)
{
    pthread_mutex_lock(&g_mic_mu);
    g_mic_hold = on;
    if (on && !g_mic_cap_run) {
        pthread_t t; int i;
        for (i = 0; i < 50 && g_mic_cap_alive; i++) usleep(10000);
        if (!g_mic_cap_alive) {
            g_mic_cap_run = 1; g_mic_cap_alive = 1;
            if (pthread_create(&t, 0, mic_thread, 0)) { g_mic_cap_run = 0; g_mic_cap_alive = 0; }
            else pthread_detach(t);
        }
    } else if (!on) {
        g_mic_cap_run = 0; g_mic_on = 0; g_speech_want = 0; g_play_run = 0; g_mic_hold_until = 0;
    }
    pthread_mutex_unlock(&g_mic_mu);
}

/* Channel 7 (AV input): 0x8005 open/close, 0x8004 acks. Called from audio dispatch. */
static int microphone_dispatch(aa_t *a, int id, const unsigned char *b, size_t n)
{
    unsigned long open, maxun; int present; pb_t r;
    g_mic_lg = a->lg;
    if (id == AV_ACK) { if (g_mic_unacked > 0) g_mic_unacked--; g_mic_acks++; return 0; }
    if (id != 0x8005) return 0;
    if (audio_uint(b, n, 1, &open, &present) || open > 1) return 1;
    if (!open) {
        a->lg("MIC close (%lu frames sent, %lu acks)", g_mic_sent, g_mic_acks);
        g_mic_hold_until = mic_ms() + 4000;       /* keep the speech route for the answer */
        g_mic_on = 0;
        return 0;
    }
    if (audio_uint(b, n, 4, &maxun, &present) || !present || !maxun || maxun > 16) maxun = 2;
    memset(&r, 0, sizeof r); pb_u(&r, 1, 0); pb_u(&r, 2, g_mic_cap_run ? 0 : 1);
    if (g_mic_cap_run) {
        g_mic_r = g_mic_w; g_mic_unacked = 0; g_mic_maxun = (int)maxun; g_mic_sent = g_mic_acks = 0;
        g_mic_on = 1;                             /* capture thread: speech mode -> plugin requests 0x31 */
        a->lg("MIC open (max_unacked=%lu) -> streaming car microphone", maxun);
    } else a->lg("MIC open refused: no AA picture, capture not running");
    return send_msg(a, 7, 0, 0x8006, r.b, r.n) != 0;
}

/* Session loop, every pass: send captured audio (8-byte timestamp + 1024 bytes). */
static void mic_poll(aa_t *a)
{
    int k;
    for (k = 0; k < 8 && g_mic_on && g_mic_unacked < g_mic_maxun && g_mic_w - g_mic_r >= MIC_CHUNK; k++) {
        static unsigned char p[8 + MIC_CHUNK];
        unsigned long long ts = mono_us(); unsigned i, r = g_mic_r;
        for (i = 0; i < 8; i++) p[i] = (unsigned char)(ts >> (56 - 8 * i));
        for (i = 0; i < MIC_CHUNK; i++) p[8 + i] = g_mic_ring[(r + i) % MIC_RING];
        g_mic_r = r + MIC_CHUNK;
        if (send_msg(a, 7, 0, AV_MEDIA_TS, p, sizeof p)) return;
        g_mic_unacked++; g_mic_sent++;
    }
}
#endif /* CICAA_HOST_TEST */
