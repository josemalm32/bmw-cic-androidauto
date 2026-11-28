/* nbt_carplay: the car's Apple authentication chip (ACP 2.0B on I2C bus 0, dev-i2c node below).
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 *
 * Car 2026-11-23 (nbt_carplay_probe acp): registers are read by writing the register number and reading after a
 * pause (an immediate read fails with EIO while the chip wakes up); version 03 = 2.0B, protocol 2.0, RSA-1024 /
 * SHA-1, certificate 908 bytes; a signature takes about 1.5 s. Used by iAP2 (authentication) and AirPlay
 * (auth-setup), one at a time.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cp.h"

#ifndef ACP_NODE
#define ACP_NODE "/dev/i2c/i2c_0/acp"
#endif

static int g_fd = -1;
static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static unsigned char g_cert[1280];
static unsigned g_certn;
static unsigned char g_version[4];

static int raw_write(const unsigned char* data, unsigned n)
{
    int tries;
    for (tries = 0; tries < 50; ++tries) {
        if (write(g_fd, data, n) == (ssize_t)n)
            return 0;
        usleep(1000);
    }
    say("acp: write of register 0x%02x failed: %s", data[0], strerror(errno));
    return -1;
}

static int reg_read(unsigned reg, unsigned char* out, unsigned n)
{
    const unsigned char r = (unsigned char)reg;
    int tries, k;
    for (tries = 0; tries < 5; ++tries) {
        if (raw_write(&r, 1))
            return -1;
        for (k = 0; k < 20; ++k) {
            usleep(1000);
            if (read(g_fd, out, n) == (ssize_t)n)
                return 0;
        }
    }
    say("acp: read of register 0x%02x (%u bytes) failed: %s", reg, n, strerror(errno));
    return -1;
}

static int reg_write(unsigned reg, const unsigned char* data, unsigned n)
{
    unsigned char b[1 + 128];
    if (n > 128)
        return -1;
    b[0] = (unsigned char)reg;
    memcpy(b + 1, data, n);
    return raw_write(b, n + 1);
}

int acp_open(void)
{
    unsigned char n[2];
    const char* node = getenv("CIC_CARPLAY_ACP_NODE");
    unsigned i;
    int tries;
    pthread_mutex_lock(&g_m);
    if (g_fd >= 0) {
        pthread_mutex_unlock(&g_m);
        return 0;
    }
    if (!node || !*node)
        node = ACP_NODE;
    g_fd = open(node, O_RDWR);
    if (g_fd < 0) {
        say("acp: %s: %s", node, strerror(errno));
        goto fail;
    }
    /* the first answer after a pause can be ff ff ff ff (car 2026-11-23): asked again */
    for (tries = 0; tries < 3; ++tries)
        if (!reg_read(0x00, g_version, 4) && g_version[0] != 0xff)
            break;
    if (tries == 3 || (g_version[2] != 2 && g_version[2] != 3)) {
        say("acp: no supported authentication protocol");
        goto fail;
    }
    say("acp: version 0x%02x (%s), protocol %u.%u", g_version[0],
        g_version[0] == 0x03 ? "2.0B" : g_version[0] == 0x05 ? "2.0C" : g_version[0] == 0x07 ? "3.0" : "?",
        g_version[2], g_version[3]);
    if (reg_read(0x30, n, 2))
        goto fail;
    g_certn = get16(n);
    if (!g_certn || g_certn > sizeof(g_cert)) {
        say("acp: certificate length %u", g_certn);
        goto fail;
    }
    for (i = 0; i * 128 < g_certn; ++i)
        if (reg_read(0x31 + i, g_cert + i * 128, g_certn - i * 128 < 128 ? g_certn - i * 128 : 128))
            goto fail;
    say("acp: certificate %u bytes", g_certn);
    pthread_mutex_unlock(&g_m);
    return 0;
fail:
    if (g_fd >= 0)
        close(g_fd);
    g_fd = -1;
    pthread_mutex_unlock(&g_m);
    return -1;
}

int acp_cert(const unsigned char** cert, unsigned* n)
{
    if (acp_open())
        return -1;
    *cert = g_cert;
    *n = g_certn;
    return 0;
}

int acp_major(void)
{
    return g_version[2] ? g_version[2] : 2;
}

/* challenge length, challenge, start; the status polled until "signature ready" (0x10), up to 5 s */
int acp_sign(const unsigned char* challenge, unsigned clen, unsigned char* sig, unsigned* slen)
{
    unsigned char b[2];
    unsigned i, status = 0, n;
    const long long t0 = ms_now();
    int r = -1;
    if (clen == 0 || clen > 128 || acp_open())
        return -1;
    pthread_mutex_lock(&g_m);
    put16(b, clen);
    if (reg_write(0x20, b, 2) || reg_write(0x21, challenge, clen))
        goto out;
    b[0] = 1;
    if (reg_write(0x10, b, 1))
        goto out;
    for (i = 0; i < 250; ++i) {
        usleep(20000);
        if (reg_read(0x10, b, 1) == 0) {
            status = b[0];
            if ((status & 0x70) == 0x10 || (status & 0x80))
                break;
        }
    }
    if ((status & 0x70) != 0x10) {
        say("acp: signing ended with status 0x%02x after %lld ms", status, ms_now() - t0);
        if (reg_read(0x05, b, 1) == 0)
            say("acp: error code 0x%02x", b[0]);
        goto out;
    }
    if (reg_read(0x11, b, 2))
        goto out;
    n = get16(b);
    if (n == 0 || n > 128 || reg_read(0x12, sig, n))
        goto out;
    *slen = n;
    say("acp: signed %u bytes in %lld ms", clen, ms_now() - t0);
    r = 0;
out:
    pthread_mutex_unlock(&g_m);
    return r;
}
