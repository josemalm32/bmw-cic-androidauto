/* Entropy for the CIC: QNX 6.3 image has no /dev/random nor /dev/urandom
 * (car log: open_fail on both). Source = CPU cycle-counter jitter around
 * scheduler yields/sleeps + time + pid, mixed with SHA-256 and chained with a
 * persistent seed file on the HDD (updated every call). Lab-grade, not FIPS. */
#ifndef CIC_ENTROPY_H
#define CIC_ENTROPY_H
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#ifndef CICTLS_HOST_TEST
#ifndef CICAA_HOST_TEST
#include <sys/neutrino.h>
#define CIC_CYCLES() ((unsigned long long)ClockCycles())
#endif
#endif
#ifndef CIC_CYCLES
static unsigned long long cic_cycles_fallback(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (unsigned long long)t.tv_sec * 1000000000ULL + t.tv_nsec; }
#define CIC_CYCLES() cic_cycles_fallback()
#endif
#include "mbedtls/sha256.h"

static int cic_jitter_entropy(const char *seedfile, unsigned char *out, size_t len, size_t *olen)
{
    unsigned char pool[32], seed[32];
    mbedtls_sha256_context h;
    size_t done = 0;
    int i, fd;
    unsigned long long prev = CIC_CYCLES();
    memset(seed, 0, sizeof seed);
    if (seedfile && (fd = open(seedfile, O_RDONLY)) >= 0) { read(fd, seed, sizeof seed); close(fd); }
    while (done < len) {
        mbedtls_sha256_init(&h);
        mbedtls_sha256_starts(&h, 0);
        mbedtls_sha256_update(&h, seed, sizeof seed);
        for (i = 0; i < 256; ++i) {
            unsigned long long now;
            struct timespec ts = {0, 1000 * (i & 7)};
            if (i & 1) sched_yield(); else nanosleep(&ts, 0);
            now = CIC_CYCLES();
            {   unsigned long long d = now - prev; prev = now;
                mbedtls_sha256_update(&h, (const unsigned char *)&d, sizeof d); }
        }
        {   long t = (long)time(0); int pid = getpid();
            mbedtls_sha256_update(&h, (const unsigned char *)&t, sizeof t);
            mbedtls_sha256_update(&h, (const unsigned char *)&pid, sizeof pid); }
        mbedtls_sha256_finish(&h, pool);
        mbedtls_sha256_free(&h);
        {   size_t n = len - done < 32 ? len - done : 32;
            memcpy(out + done, pool, n); done += n; }
        memcpy(seed, pool, 32);
        seed[0] ^= 0x5a;
    }
    /* chain: next boot starts from a different state */
    if (seedfile && (fd = open(seedfile, O_WRONLY | O_CREAT | O_TRUNC, 0600)) >= 0) {
        mbedtls_sha256(pool, 32, seed, 0);
        write(fd, seed, 32);
        close(fd);
    }
    *olen = len;
    return 0;
}
#endif
