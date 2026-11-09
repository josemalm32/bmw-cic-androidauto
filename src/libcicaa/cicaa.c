/* libcicaa.so - Android Auto head-unit protocol for the BMW CIC (QNX 6.3.2 / SH4).
 *
 * Loaded on demand (dlopen) by the AAPlugin HMI plugin from /mnt/hbdebug/aap/.
 * cicaa_start() spawns one worker (512 KB stack):
 *   USB (io-usb client) -> AOA switch -> accessory bulk pipes -> AA version ->
 *   TLS 1.2 (head unit = client) -> service discovery -> video/audio/input channels.
 * Video and audio are handed to the cicaalive process through shared-memory rings
 * (/cicaa_live, /cicaa_audio). Nothing is written to disk.
 *
 * Head-unit certificate/key: /mnt/hbdebug/aap/hu.crt + hu.key (PEM). They are NOT
 * part of this project: the installer copies them from the USB stick (AAHU.CRT,
 * AAHU.KEY). Without them the phone rejects the TLS handshake.
 *
 * Logging: only when /mnt/hbdebug/aap/logs.on exists (toggled by the plugin's
 * "Logs" button). Then lines go to /mnt/hbdebug/aap/logs/cicaa.log. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>
#include "aa_core.h"
#include "aa_usb.h"
#include "cic_entropy.h"
#include "aap_paths.h"

static char g_status[128] = "sin iniciar";
static char g_result[192] = "";
static volatile int g_busy = 0;
static int g_logon = 0;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static double g_t0;
static void lg(const char *fmt, ...)
{
    char line[320];
    int p, fd;
    va_list ap;
    if (!g_logon) return;
    p = snprintf(line, sizeof line, "[%7.2f] ", now_s() - g_t0);
    va_start(ap, fmt);
    vsnprintf(line + p, sizeof line - p, fmt, ap);
    va_end(ap);
    fd = open(AAP_LOGDIR "/cicaa.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    write(fd, line, strlen(line)); write(fd, "\n", 1); close(fd);
}
static void st(const char *s) { snprintf(g_status, sizeof g_status, "%s", s); lg("STATUS %s", s); }

int cicaa_entropy(void *d, unsigned char *o, size_t n, size_t *olen)
{
    int fd;
    ssize_t r;
    (void)d;
    *olen = 0;
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) fd = open("/dev/random", O_RDONLY);
    /* The CIC has no random device: timing jitter, chained through a RAM-only seed. */
    if (fd < 0) return cic_jitter_entropy(AAP_SEED, o, n, olen);
    r = read(fd, o, n);
    close(fd);
    if (r <= 0) return -0x003C;
    *olen = (size_t)r;
    return 0;
}

/* load whole file + NUL (PEM parser wants the terminator counted) */
static unsigned char *slurp(const char *path, size_t *n)
{
    int fd = open(path, O_RDONLY);
    unsigned char *b;
    long sz;
    if (fd < 0) return 0;
    sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (sz <= 0 || sz > 65536) { close(fd); return 0; }
    b = malloc((size_t)sz + 1);
    if (!b) { close(fd); return 0; }
    if (read(fd, b, (size_t)sz) != sz) { free(b); close(fd); return 0; }
    close(fd);
    b[sz] = 0;
    *n = (size_t)sz + 1;
    return b;
}

static void *worker(void *arg)
{
    aa_transport_t t;
    aa_pki_t pki;
    unsigned char *crt, *key;
    size_t cn = 0, kn = 0;
    char sum[160];
    int stage;
    (void)arg;
    g_t0 = now_s();
    g_logon = access(AAP_LOGS_ON, F_OK) == 0;
    if (g_logon) mkdir(AAP_LOGDIR, 0755);
    lg("=== libcicaa " AAP_VERSION " ===");
    crt = slurp(AAP_DIR "/hu.crt", &cn);
    key = slurp(AAP_DIR "/hu.key", &kn);
    if (!crt || !key) {
        free(crt); free(key);
        snprintf(g_result, sizeof g_result, "falta hu.crt/hu.key (AAHU.CRT/AAHU.KEY en el USB al instalar)");
        st("Falta certificado AA");
        g_busy = 0;
        return 0;
    }
    pki.crt = crt; pki.crt_len = cn; pki.key = key; pki.key_len = kn; pki.origin = "hu.crt/hu.key";
    st("Esperando movil por USB...");
    if (aa_usb_open(lg, st, 120, &t) != 0) {
        snprintf(g_result, sizeof g_result, "no se detecto movil Android Auto");
        st("Sin movil");
    } else {
        stage = aa_session(&t, &pki, lg, st, sum, sizeof sum);
        snprintf(g_result, sizeof g_result, "etapa %d/7: %s", stage, sum);
        st(stage >= 7 ? "Sesion terminada" : "Sesion terminada con error");
    }
    aa_usb_close();
    free(crt); free(key);
    lg("RESULT %s", g_result);
    g_busy = 0;
    return 0;
}

int cicaa_start(void)
{
    pthread_attr_t a;
    pthread_t t;
    if (g_busy) return 1;
    g_busy = 1;
    g_result[0] = 0;
    snprintf(g_status, sizeof g_status, "arrancando...");
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 512 * 1024);
    if (pthread_create(&t, &a, worker, 0) != 0) {
        g_busy = 0;
        snprintf(g_status, sizeof g_status, "error creando hilo");
        return -1;
    }
    return 0;
}
const char *cicaa_status(void) { return g_status; }
const char *cicaa_result(void) { return g_result; }
int cicaa_busy(void) { return g_busy; }
