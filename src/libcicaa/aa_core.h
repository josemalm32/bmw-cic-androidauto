/* Android Auto head-unit protocol core (transport independent).
 * Wire format (as implemented by the open-source aasdk/openauto head units):
 *   frame  = channel u8 | flags u8 | size u16 BE | [total u32 BE if FIRST-only] | payload
 *   flags  = frame type (bit0 FIRST, bit1 LAST; 3 = complete) | 0x04 CONTROL | 0x08 ENCRYPTED
 *   payload= message id u16 BE | body
 * Control channel 0: VERSION_REQUEST(1) -> VERSION_RESPONSE(2), SSL_HANDSHAKE(3) carrying
 * raw TLS records (head unit = TLS client), AUTH_COMPLETE(4), then encrypted
 * SERVICE_DISCOVERY_REQUEST(5) from the phone. */
#ifndef AA_CORE_H
#define AA_CORE_H
#include <stddef.h>

typedef struct aa_transport {
    void *ctx;
    /* return bytes written (all) or <0 */
    int (*send)(void *ctx, const unsigned char *b, int n);
    /* return bytes read (>0), 0 on timeout, <0 on error */
    int (*recv)(void *ctx, unsigned char *b, int n, int timeout_ms);
} aa_transport_t;

typedef void (*aa_log_fn)(const char *fmt, ...);
typedef void (*aa_status_fn)(const char *s);

typedef struct aa_pki {
    const unsigned char *crt; size_t crt_len;   /* PEM incl. NUL, or DER */
    const unsigned char *key; size_t key_len;
    const char *origin;                         /* for the log */
} aa_pki_t;

/* Runs version exchange, TLS handshake, AUTH_COMPLETE and waits for the
 * phone's first encrypted control messages. Returns highest stage reached:
 * 0 none, 1 version, 2 TLS, 3 auth, 4 SD answered, 5 input bound, 6 video start, 7 video data. */
int aa_session(aa_transport_t *t, const aa_pki_t *pki, aa_log_fn lg, aa_status_fn st,
               char *summary, size_t summary_len);

#endif
