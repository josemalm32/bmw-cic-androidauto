/* nbt_carplay: log, time, byte order, random numbers, small files.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "cp.h"

static pthread_mutex_t g_log = PTHREAD_MUTEX_INITIALIZER;

void say(const char* fmt, ...)
{
    struct timespec ts;
    va_list ap;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    pthread_mutex_lock(&g_log);
    printf("%5ld.%03ld ", (long)ts.tv_sec % 100000, ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    pthread_mutex_unlock(&g_log);
}

void hexdump(const char* what, const void* p, unsigned n)
{
    const unsigned char* b = (const unsigned char*)p;
    unsigned i;
    pthread_mutex_lock(&g_log);
    printf("      %s (%u bytes):", what, n);
    for (i = 0; i < n; ++i)
        printf("%s%02x", i % 32 ? " " : "\n        ", b[i]);
    putchar('\n');
    pthread_mutex_unlock(&g_log);
}

long long ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

void sleep_ms(unsigned ms)
{
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&t, &t) && errno == EINTR) {
    }
}

void put16(unsigned char* p, unsigned v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

void put32(unsigned char* p, unsigned v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

unsigned get16(const unsigned char* p)
{
    return (unsigned)p[0] << 8 | p[1];
}

unsigned get32(const unsigned char* p)
{
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

void put16le(unsigned char* p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

void put32le(unsigned char* p, unsigned v)
{
    put16le(p, v & 0xffff);
    put16le(p + 2, v >> 16);
}

void put64le(unsigned char* p, unsigned long long v)
{
    put32le(p, (unsigned)v);
    put32le(p + 4, (unsigned)(v >> 32));
}

unsigned get16le(const unsigned char* p)
{
    return (unsigned)p[1] << 8 | p[0];
}

unsigned get32le(const unsigned char* p)
{
    return get16le(p + 2) << 16 | get16le(p);
}

void tohex(char* out, const unsigned char* b, unsigned n)
{
    static const char d[] = "0123456789abcdef";
    unsigned i;
    for (i = 0; i < n; ++i) {
        out[2 * i] = d[b[i] >> 4];
        out[2 * i + 1] = d[b[i] & 15];
    }
    out[2 * n] = 0;
}

/* ---- random numbers: Mbed TLS CTR-DRBG, shared by every thread ---- */

static mbedtls_ctr_drbg_context g_drbg;
static pthread_mutex_t g_rng = PTHREAD_MUTEX_INITIALIZER;

static int entropy_read(void* ctx, unsigned char* dst, size_t size)
{
    int fd = *(const int*)ctx;
    while (size) {
        ssize_t nr = read(fd, dst, size);
        if (nr < 0 && errno == EINTR)
            continue;
        if (nr <= 0)
            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        dst += nr;
        size -= nr;
    }
    return 0;
}

int cp_random_init(void)
{
    const char* source = getenv("CIC_CARPLAY_ENTROPY_FILE");
    static int fd;
    int rc;

    fd = open(source && *source ? source : "/dev/urandom", O_RDONLY);
    if (fd < 0) {
        say("random: no entropy source: %s", strerror(errno));
        return -1;
    }
    mbedtls_ctr_drbg_init(&g_drbg);
    rc = mbedtls_ctr_drbg_seed(&g_drbg, entropy_read, &fd, (const unsigned char*)"cic-carplay", 11);
    if (rc) {
        close(fd);
        return -1;
    }
    return 0;
}

int cp_rng(void* ctx, unsigned char* out, size_t n)
{
    int r;
    (void)ctx;
    pthread_mutex_lock(&g_rng);
    r = 0;
    while (n && !r) {   /* the DRBG gives at most MBEDTLS_CTR_DRBG_MAX_REQUEST bytes per call */
        const size_t k = n > 1024 ? 1024 : n;
        r = mbedtls_ctr_drbg_random(&g_drbg, out, k);
        out += k;
        n -= k;
    }
    pthread_mutex_unlock(&g_rng);
    return r;
}

void cp_random(void* out, size_t n)
{
    if (cp_rng(0, (unsigned char*)out, n)) {
        say("random: the generator failed");
        abort();
    }
}

unsigned cp_rand32(void)
{
    unsigned v;
    cp_random(&v, sizeof(v));
    return v;
}

/* ---- files ---- */

int file_read(const char* path, void* buf, unsigned size)
{
    FILE* f = fopen(path, "rb");
    size_t n;
    if (!f)
        return -1;
    n = fread(buf, 1, size, f);
    fclose(f);
    return (int)n;
}

int file_write(const char* path, const void* data, unsigned n)
{
    char tmp[300];
    FILE* f;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f)
        return -1;
    if (fwrite(data, 1, n, f) != n) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    if (fclose(f) || rename(tmp, path)) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static void state_path(char* out, unsigned size, const char* name)
{
    snprintf(out, size, "%s/%s", g_opt.state_dir, name);
}

int state_read(const char* name, void* buf, unsigned size)
{
    char p[300];
    state_path(p, sizeof(p), name);
    return file_read(p, buf, size);
}

int state_write(const char* name, const void* data, unsigned n)
{
    char p[300];
    mkdir(g_opt.state_dir, 0700);
    state_path(p, sizeof(p), name);
    if (file_write(p, data, n)) {
        say("state: %s could not be written", p);
        return -1;
    }
    chmod(p, 0600);
    return 0;
}

void flag_set(const char* path, const char* text)
{
    FILE* f = fopen(path, "w");
    if (!f)
        return;
    if (text)
        fputs(text, f);
    fclose(f);
    chmod(path, 0644);
}

void flag_clear(const char* path)
{
    unlink(path);
}
