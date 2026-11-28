/* nbt_carplay: wired CarPlay on the NBT, one iPhone session (started by native_service when an iPhone is plugged in;
 * docs/CARPLAY.md).
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 * (license and the section 7 permission for the head unit's libraries: NOTICE.md, "GPLv3 parts")
 *
 *   USB (cp_usb.c)        the iPhone's CarPlay configuration: multiplexor + NCM
 *   network (cp_ncm.c)    NCM <-> lwIP, IPv6 link-local (step a)
 *   lockdown, iAP2        pairing ("Trust", once), carkit service, identification, authentication with the car's
 *                         chip, CarPlayStartSession (cp_lockdown.c, cp_iap2.c)
 *   AirPlay (step b)      the iPhone connects to port 7000: pairing, auth-setup, streams (cp_airplay.c)
 *   screen (step c)       H.264 into nbt_decoder's FIFO (cp_screen.c)
 *   audio, microphone     the car's audio manager (cp_audio.c, cp_mic.c)
 *   keys                  nbt_knob's key lines -> knob / media HID, Siri (cp_input.c)
 *
 * usage: nbt_carplay [--stop-after usb|iap2|net|airplay|video [--seconds N]] [--pcap FILE] [--dump FILE]
 *                    [--audio-dump DIR] [--no-audio] [--no-mic] [--fps N] [--knob-invert] [--state DIR]
 *                    [--fifo PATH|none] [--input PATH]
 *   --stop-after   end N seconds (default 15) after that step worked: usb = configuration and pipes, iap2 =
 *                  CarPlayStartSession sent, net = the iPhone connected to us over NCM, airplay = pairing and the
 *                  screen stream set up, video = 60 frames written
 * Environment (native_service): NBT_AA_MEDIA_OUT (default input_stereo_1), NBT_AA_PROMPT_OUT (prompts while another
 * source plays, e.g. input_stereo_3), NBT_AUX_SOURCE (our main source, default f6), NBT_KNOB_INVERT=1.
 * Exit status: 0 after a session, 1 when it could not start, 2 wrong arguments.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cp.h"
#include "cp_lockdown.h"
#include "cp_usb.h"

struct cp_options g_opt;
volatile int g_stop;

static volatile long long g_reached_at;

void cp_reached(int step)
{
    if (g_opt.stop_after == step && !g_reached_at) {
        g_reached_at = ms_now();
        say("nbt_carplay: step reached; ending in %d s (--stop-after)", g_opt.seconds);
    }
}

static void* watch(void* arg)
{
    (void)arg;
    while (!g_stop) {
        if (g_reached_at && ms_now() - g_reached_at >= g_opt.seconds * 1000LL) {
            say("nbt_carplay: --stop-after: done");
            g_stop = 1;
        }
        if (usb_gone())
            g_stop = 1;
        sleep_ms(200);
    }
    return 0;
}

static void stopped(int s)
{
    (void)s;
    g_stop = 1;
}

static struct usb g_usb;

static unsigned long long g_send_errors;

static int ncm_send(void* ctx, const unsigned char* block, unsigned n)
{
    unsigned actual = 0;
    const int rc = usb_xfer(&g_usb.ncm_out, (void*)block, n, 1000, &actual);
    (void)ctx;
    if (rc || actual != n) {
        if (++g_send_errors <= 5)
            say("ncm: send of %u bytes: result %d, status %08x, %u sent", n, rc, g_usb.ncm_out.status, actual);
        return -1;
    }
    return 0;
}

/* NCM in, with what the transfers end with (car 2026-11-23: no frame came from the iPhone; the reads may fail) */
static void* ncm_rx(void* arg)
{
    static unsigned char b[65536];
    unsigned long long reads = 0, empty = 0, bytes = 0, fin, fout;
    unsigned last_status = 0, logged = 0;
    long long next = ms_now() + 10000;
    (void)arg;
    while (!g_stop && !usb_gone()) {
        unsigned actual = 0;
        const int rc = usb_xfer(&g_usb.ncm_in, b, g_usb.ntb_in + 2048, 1000, &actual);
        if (actual) {
            if (reads++ < 6)   /* whole, to read a block format the parser does not know (car 2026-11-24: "ncmx") */
                hexdump("ncm: transfer in", b, actual < 512 ? actual : 512);
            bytes += actual;
            ncm_input(b, actual);
        } else {
            ++empty;
            if (g_usb.ncm_in.status != last_status && logged < 8) {
                ++logged;
                say("ncm: read ended without data: result %d, status %08x", rc, g_usb.ncm_in.status);
            }
            last_status = g_usb.ncm_in.status;
            if (rc && rc != ETIMEDOUT)
                sleep_ms(100);
        }
        if (ms_now() >= next) {
            next = ms_now() + 10000;
            ncm_counts(&fin, &fout);
            say("ncm: %llu transfers in (%llu bytes), %llu empty, frames %llu in / %llu out, %llu send errors", reads,
                bytes, empty, fin, fout, g_send_errors);
        }
    }
    return 0;
}

static void* ncm_notify(void* arg)
{
    unsigned char b[64];
    unsigned n = 0;
    (void)arg;
    while (!g_stop && !usb_gone()) {
        unsigned actual = 0;
        const int rc = usb_xfer(&g_usb.ncm_int, b, sizeof(b), 1000, &actual);
        if (actual && n++ < 10)
            hexdump("ncm: notification", b, actual);
        else if (!actual && rc && rc != ETIMEDOUT)
            sleep_ms(500);
    }
    return 0;
}

/* Use the file only if it is the exact certificate returned by the signer. */
static int check_mfi_cert(const char* path)
{
    const unsigned char* chip_cert;
    unsigned chip_size;
    unsigned char file_cert[1281];
    FILE* file = fopen(path, "rb");
    size_t size;
    int read_error;

    if (!file) {
        say("mfi: cannot open public certificate %s: %s", path, strerror(errno));
        return -1;
    }
    size = fread(file_cert, 1, sizeof(file_cert), file);
    read_error = ferror(file);
    fclose(file);
    if (read_error || !size || size == sizeof(file_cert)) {
        say("mfi: invalid public certificate file size or read error");
        return -1;
    }
    if (acp_cert(&chip_cert, &chip_size)) {
        say("mfi: no compatible authentication signer");
        return -1;
    }
    if (size != chip_size || memcmp(file_cert, chip_cert, chip_size)) {
        say("mfi: public certificate does not match the authentication chip");
        return -1;
    }
    return 0;
}

/* Stop after selecting the CarPlay interfaces, before lockdown or MFi. */
static int probe_usb(void)
{
    int rc;
    signal(SIGTERM, stopped);
    signal(SIGINT, stopped);
    if (cp_random_init()) {
        say("usb probe: could not initialize entropy");
        return 1;
    }
    rc = usb_open(&g_usb, 60);
    if (!rc)
        say("usb probe: multiplexor and NCM endpoints opened (NTB input %u bytes)", g_usb.ntb_in);
    usb_close(&g_usb);
    return rc ? 1 : 0;
}

static int usage(void)
{
    fprintf(stderr, "usage: nbt_carplay [--stop-after usb|iap2|net|airplay|video [--seconds N]] [--pcap FILE] "
                    "[--dump FILE] [--audio-dump DIR] [--no-audio] [--no-mic] [--fps N] [--knob-invert] "
                    "--mfi-cert PATH [--state DIR] [--fifo PATH|none] [--input PATH]\n");
    fprintf(stderr, "       cic_carplay --probe-usb\n");
    return 2;
}

int main(int argc, char** argv)
{
    static const char* steps[] = {"", "usb", "iap2", "net", "airplay", "video"};
    struct iap2_carplay cp;
    struct stream* carkit;
    char mac[20];
    const char* mfi_cert = 0;
    pthread_t t;
    int i, r;
    setvbuf(stdout, 0, _IOLBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--probe-usb"))
        return probe_usb();
    g_opt.width = 800;
    g_opt.height = 480;
    g_opt.fps = 30;
    g_opt.width_mm = 152;   /* 800x480 bench profile */
    g_opt.height_mm = 91;
    g_opt.seconds = 15;
    g_opt.state_dir = "/mnt/hbdebug/aap/carplay";
    g_opt.fifo = 0;
    g_opt.input = "/tmp/aap.carplay.input";
    g_opt.audio = g_opt.mic = 0;
    g_opt.media_out = getenv("NBT_AA_MEDIA_OUT") ? getenv("NBT_AA_MEDIA_OUT") : "input_stereo_1";
    g_opt.prompt_out = getenv("NBT_AA_PROMPT_OUT");
    g_opt.aux_source = getenv("NBT_AUX_SOURCE") ? (unsigned)strtoul(getenv("NBT_AUX_SOURCE"), 0, 16) : 0xf6;
    g_opt.knob_invert = getenv("NBT_KNOB_INVERT") && !strcmp(getenv("NBT_KNOB_INVERT"), "1");
    for (i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : 0;
        if (!strcmp(a, "--stop-after") && v) {
            unsigned k;
            for (k = 1; k < sizeof(steps) / sizeof(steps[0]) && strcmp(v, steps[k]); ++k) {
            }
            if (k == sizeof(steps) / sizeof(steps[0]))
                return usage();
            g_opt.stop_after = (int)k;
            ++i;
        } else if (!strcmp(a, "--seconds") && v) {
            g_opt.seconds = atoi(v);
            ++i;
        } else if (!strcmp(a, "--pcap") && v) {
            g_opt.pcap = v;
            ++i;
        } else if (!strcmp(a, "--dump") && v) {
            g_opt.dump = v;
            ++i;
        } else if (!strcmp(a, "--audio-dump") && v) {
            g_opt.audio_dump = v;
            ++i;
        } else if (!strcmp(a, "--mfi-cert") && v) {
            mfi_cert = v;
            ++i;
        } else if (!strcmp(a, "--state") && v) {
            g_opt.state_dir = v;
            ++i;
        } else if (!strcmp(a, "--fifo") && v) {
            g_opt.fifo = strcmp(v, "none") ? v : 0;
            ++i;
        } else if (!strcmp(a, "--input") && v) {
            g_opt.input = v;
            ++i;
        } else if (!strcmp(a, "--fps") && v) {
            g_opt.fps = atoi(v);
            ++i;
        } else if (!strcmp(a, "--no-audio"))
            g_opt.audio = g_opt.mic = 0;
        else if (!strcmp(a, "--no-mic"))
            g_opt.mic = 0;
        else if (!strcmp(a, "--knob-invert"))
            g_opt.knob_invert = 1;
        else
            return usage();
    }
    if (g_opt.fps < 15 || g_opt.fps > 60 || g_opt.seconds < 0)
        return usage();
    if (!mfi_cert) {
        say("mfi: --mfi-cert requires a public PKCS#7 certificate file");
        return usage();
    }
    if (check_mfi_cert(mfi_cert))
        return 1;
    signal(SIGTERM, stopped);
    signal(SIGINT, stopped);
    signal(SIGPIPE, SIG_IGN);
    say("nbt_carplay: %dx%d at %d fps, state in %s%s%s", g_opt.width, g_opt.height, g_opt.fps, g_opt.state_dir,
        g_opt.stop_after ? ", stop after " : "", g_opt.stop_after ? steps[g_opt.stop_after] : "");
    if (cp_random_init()) {
        say("nbt_carplay: no random numbers");
        return 1;
    }
    if (airplay_identity())
        return 1;
    if (pthread_create(&t, 0, watch, 0))
        return 1;
    if (usb_open(&g_usb, 60)) {
        g_stop = 1;
        usb_close(&g_usb);
        return 1;
    }
    if (g_opt.pcap)
        ncm_pcap(g_opt.pcap);
    if (net_init() || ncm_start(g_usb.mac, ncm_send, 0) || pthread_create(&t, 0, ncm_rx, 0)) {
        g_stop = 1;
        usb_close(&g_usb);
        return 1;
    }
    if (g_usb.have_int)
        pthread_create(&t, 0, ncm_notify, 0);
    cp_reached(STOP_USB);
    if (airplay_start(AIRPLAY_PORT)) {
        g_stop = 1;
        usb_close(&g_usb);
        return 1;
    }
    if (g_opt.audio)
        audio_start();
    input_start();
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", g_usb.mac[0], g_usb.mac[1], g_usb.mac[2],
             g_usb.mac[3], g_usb.mac[4], g_usb.mac[5]);
    memset(&cp, 0, sizeof(cp));
    cp.address = ncm_address();
    cp.port = AIRPLAY_PORT;
    cp.mac = mac;
    cp.public_key = airplay_public_hex();
    cp.source_version = SOURCE_VERSION;
    cp.ncm_interface = g_usb.ncm_ctrl;
    r = -1;
    if (!g_stop && !carkit_connect(&g_usb, &carkit))
        r = iap2_run(carkit, &cp);
    g_stop = 1;
    carkit_close();
    audio_stop();
    usb_close(&g_usb);
    say("nbt_carplay: over");
    return r ? 1 : 0;
}
