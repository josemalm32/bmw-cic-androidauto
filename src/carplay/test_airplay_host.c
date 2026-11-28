/* nbt_carplay host test: the AirPlay receiver (pairing, auth-setup, SETUP, screen, audio, event connection, timing)
 * on the host's loopback. The simulated chip returns a padded challenge.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 *
 *   test_airplay_host STATE_DIR FIFO_OR_FILE INPUT_FIFO AUDIO_DIR
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cp.h"

struct cp_options g_opt;
volatile int g_stop;

void cp_reached(int step)
{
    say("test: step %d reached", step);
}

/* the fake chip: a fixed "certificate" and the challenge itself (padded) as the "signature" */
static const unsigned char CERT[] = "test certificate";

int acp_open(void)
{
    return 0;
}

int acp_cert(const unsigned char** cert, unsigned* n)
{
    *cert = CERT;
    *n = sizeof(CERT) - 1;
    return 0;
}

int acp_major(void)
{
    return 2;
}

int acp_sign(const unsigned char* challenge, unsigned clen, unsigned char* sig, unsigned* slen)
{
    if (getenv("CARPLAY_TEST_SIGN_FAIL") || clen > 128)
        return -1;
    memset(sig, 0xa5, 128);
    memcpy(sig, challenge, clen);
    *slen = 128;
    return 0;
}

static void stopped(int s)
{
    (void)s;
    g_stop = 1;
}

int main(int argc, char** argv)
{
    if (argc != 5)
        return 2;
    setvbuf(stdout, 0, _IOLBF, 0);
    signal(SIGTERM, stopped);
    signal(SIGINT, stopped);
    g_opt.width = 1280;
    g_opt.height = 480;
    g_opt.fps = 60;
    g_opt.width_mm = 243;
    g_opt.height_mm = 91;
    g_opt.state_dir = argv[1];
    g_opt.dump = argv[2];
    g_opt.fifo = 0;
    g_opt.input = argv[3];
    g_opt.audio_dump = argv[4];
    g_opt.audio = 1;
    g_opt.mic = 1;
    g_opt.aux_source = 0xf6;
    mkdir(argv[1], 0700);
    if (cp_random_init() || net_init() || airplay_identity())
        return 1;
    audio_start();
    input_start();
    if (airplay_start(AIRPLAY_PORT))
        return 1;
    while (!g_stop)
        sleep_ms(100);
    audio_stop();
    return 0;
}
