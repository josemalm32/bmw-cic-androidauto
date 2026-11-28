/* nbt_carplay: the USB multiplexor, lockdown, pairing and the carkit service (as nbt_carplay_probe, car 2026-11-23).
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 *
 * Multiplexor (usbmux version 2, as LIVI / xcertplay): frames {protocol, length, 0xfeedface, our sequence, the last
 * one received} (big-endian), first {0, 20, 2, 0, 0} raw (version 2, answered the same), then a setup frame (protocol
 * 2, one byte 7). Protocol 6 carries a small TCP {source port, destination port, seq, ack, 0x50, flags, window, 0, 0};
 * lockdown listens on 62078. Lockdown messages: a 4-byte big-endian length, then an XML plist.
 * Pairing (the iPhone asks "Trust This Computer?" once): a root key and certificate (CA), a host key and certificate
 * and a certificate for the iPhone's own key, all RSA-2048 (made once: 12-24 s on the car), kept in the state
 * directory (pair-*). StartSession turns on TLS (the root certificate and key as our identity); StartService
 * com.apple.carkit.service gives the port of the carkit service, TLS too, over which iAP2 runs (cp_iap2.c).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mbedtls/base64.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/sha1.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "cp.h"
#include "cp_lockdown.h"
#include "cp_usb.h"

enum { MUX_TCP = 6, TCP_FIN = 0x01, TCP_SYN = 0x02, TCP_RST = 0x04, TCP_ACK = 0x10, LOCKDOWN_PORT = 62078 };

static struct {
    struct usb* u;
    unsigned char rx[70000];
    unsigned rxn;
    unsigned seq, ack, next_port;
} g_mux;

struct tcpc {
    unsigned sport, dport, tseq, tack, connected, closed;
    unsigned char data[131072];
    unsigned datan;
};
static struct tcpc g_tc[3];

static int mux_send_raw(const unsigned char* data, unsigned n)
{
    unsigned actual = 0;
    const int rc = usb_xfer(&g_mux.u->mux_out, (void*)data, n, 2000, &actual);
    if (rc || actual != n) {
        say("mux: send of %u bytes failed (%d, %u sent)", n, rc, actual);
        return -1;
    }
    return 0;
}

static int mux_send(unsigned protocol, const unsigned char* payload, unsigned n)
{
    static unsigned char f[16 + 16384];
    if (n > 16384 - 16)
        return -1;
    put32(f, protocol);
    put32(f + 4, 16 + n);
    put32(f + 8, 0xfeedfaceu);
    put16(f + 12, g_mux.seq);
    put16(f + 14, g_mux.ack);
    memcpy(f + 16, payload, n);
    g_mux.seq = (g_mux.seq + 1) & 0xffff;
    return mux_send_raw(f, 16 + n);
}

/* one whole frame into f within ms: 1 = a frame, 0 = none, -1 = failed */
static int mux_frame(unsigned char* f, unsigned* n, unsigned ms)
{
    const long long until = ms_now() + ms;
    static unsigned char b[16384];
    for (;;) {
        unsigned actual = 0;
        long long left;
        int rc;
        if (g_mux.rxn >= 8) {
            const unsigned len = get32(g_mux.rx + 4);
            if (len < 8 || len > sizeof(g_mux.rx)) {
                say("mux: bad frame length %u", len);
                return -1;
            }
            if (g_mux.rxn >= len) {
                memcpy(f, g_mux.rx, len);
                *n = len;
                memmove(g_mux.rx, g_mux.rx + len, g_mux.rxn - len);
                g_mux.rxn -= len;
                if (len >= 16)
                    g_mux.ack = get16(f + 12);
                return 1;
            }
        }
        left = until - ms_now();
        if (left <= 0)
            return 0;
        if (usb_gone())
            return -1;
        rc = usb_xfer(&g_mux.u->mux_in, b, sizeof(b), left < 200 ? 200 : left > 1000 ? 1000 : (unsigned)left, &actual);
        if (!actual) {
            if (rc == 0 || rc == ETIMEDOUT)
                continue;
            say("mux: read failed (%d)", rc);
            return -1;
        }
        if (actual > sizeof(g_mux.rx) - g_mux.rxn)
            return -1;
        memcpy(g_mux.rx + g_mux.rxn, b, actual);
        g_mux.rxn += actual;
    }
}

static int tcp_send(struct tcpc* c, unsigned flags, const unsigned char* payload, unsigned n)
{
    static unsigned char t[20 + 16000];
    if (n > 16000)
        return -1;
    memset(t, 0, 20);
    put16(t, c->sport);
    put16(t + 2, c->dport);
    put32(t + 4, c->tseq);
    put32(t + 8, c->tack);
    t[12] = 0x50;
    t[13] = (unsigned char)flags;
    put16(t + 14, 512);
    if (n)
        memcpy(t + 20, payload, n);
    if (mux_send(MUX_TCP, t, 20 + n))
        return -1;
    c->tseq += n;
    return 0;
}

/* one frame to its connection: 1 = a frame, 0 = none, -1 = the multiplexor failed */
static int tcp_poll(unsigned ms)
{
    static unsigned char f[70000];
    unsigned n, k;
    const int r = mux_frame(f, &n, ms);
    const unsigned char* t;
    unsigned hl, flags, port, len;
    struct tcpc* c = 0;
    if (r <= 0)
        return r;
    if (get32(f) != MUX_TCP || n < 16 + 20)
        return 1;
    t = f + 16;
    hl = (t[12] >> 4) * 4u;
    flags = t[13];
    port = get16(t + 2);
    for (k = 0; k < sizeof(g_tc) / sizeof(g_tc[0]); ++k)
        if (g_tc[k].sport == port && !g_tc[k].closed)
            c = &g_tc[k];
    if (!c || hl < 20 || 16 + hl > n)
        return 1;
    if (flags & TCP_RST) {
        say("tcp %u: reset by the iPhone", c->dport);
        c->closed = 2;
        return 1;
    }
    if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
        c->tseq += 1;
        c->tack = get32(t + 4) + 1;
        c->connected = 1;
        tcp_send(c, TCP_ACK, 0, 0);
        return 1;
    }
    len = n - 16 - hl;
    if (len) {
        if (len > sizeof(c->data) - c->datan) {
            say("tcp %u: receive buffer full", c->dport);
            c->closed = 2;
            return 1;
        }
        memcpy(c->data + c->datan, t + hl, len);
        c->datan += len;
        c->tack += len;
        tcp_send(c, TCP_ACK, 0, 0);
    }
    if (flags & TCP_FIN) {
        c->tack += 1;
        tcp_send(c, TCP_ACK, 0, 0);
        c->closed = 1;
    }
    return 1;
}

static struct tcpc* tcp_open(unsigned slot, unsigned port)
{
    struct tcpc* c = &g_tc[slot];
    long long until;
    memset(c, 0, sizeof(*c));
    c->sport = g_mux.next_port++;
    c->dport = port;
    if (tcp_send(c, TCP_SYN, 0, 0))
        return 0;
    until = ms_now() + 5000;
    while (!c->connected && !c->closed && ms_now() < until)
        if (tcp_poll(200) < 0)
            return 0;
    if (!c->connected) {
        say("tcp: port %u %s", port, c->closed ? "refused" : "did not answer");
        c->closed = 2;
        return 0;
    }
    return c;
}

static void tcp_close(struct tcpc* c)
{
    if (c && c->connected && !c->closed)
        tcp_send(c, TCP_FIN | TCP_ACK, 0, 0);
    if (c)
        c->closed = 1;
}

static int tcp_take(struct tcpc* c, unsigned char* out, unsigned n, unsigned ms)
{
    const long long until = ms_now() + ms;
    while (c->datan < n && !c->closed && ms_now() < until)
        if (tcp_poll(200) < 0)
            return -1;
    if (c->datan < n)
        return -1;
    memcpy(out, c->data, n);
    memmove(c->data, c->data + n, c->datan - n);
    c->datan -= n;
    return 0;
}

/* ---- a byte stream: a TCP connection over the multiplexor, TLS on it when a session or service asks for it ---- */

struct stream {
    struct tcpc* c;
    int tls;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
};

static unsigned g_recv_ms = 8000;   /* how long a TLS read waits for data (short: polling) */

static int bio_send(void* ctx, const unsigned char* buf, size_t len)
{
    struct tcpc* c = (struct tcpc*)ctx;
    const unsigned n = len > 16000 ? 16000 : (unsigned)len;
    if (c->closed)
        return MBEDTLS_ERR_NET_CONN_RESET;
    return tcp_send(c, TCP_ACK, buf, n) ? MBEDTLS_ERR_NET_SEND_FAILED : (int)n;
}

static int bio_recv(void* ctx, unsigned char* buf, size_t len)
{
    struct tcpc* c = (struct tcpc*)ctx;
    const long long until = ms_now() + g_recv_ms;
    unsigned n;
    while (!c->datan && !c->closed && ms_now() < until)
        if (tcp_poll(200) < 0)
            return MBEDTLS_ERR_NET_RECV_FAILED;
    if (!c->datan)
        return c->closed ? MBEDTLS_ERR_NET_CONN_RESET
                         : g_recv_ms < 8000 ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_TIMEOUT;
    n = c->datan < len ? c->datan : (unsigned)len;
    memcpy(buf, c->data, n);
    memmove(c->data, c->data + n, c->datan - n);
    c->datan -= n;
    return (int)n;
}

static void tls_error(const char* what, int r)
{
    char e[160];
    mbedtls_strerror(r, e, sizeof(e));
    say("tls: %s: -0x%04x %s", what, (unsigned)-r, e);
}

static int stream_tls(struct stream* s, mbedtls_x509_crt* cert, mbedtls_pk_context* key)
{
    int r;
    mbedtls_ssl_init(&s->ssl);
    mbedtls_ssl_config_init(&s->conf);
    r = mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
    if (r) {
        tls_error("config", r);
        return -1;
    }
    mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&s->conf, cp_rng, 0);
    mbedtls_ssl_conf_max_tls_version(&s->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    r = mbedtls_ssl_conf_own_cert(&s->conf, cert, key);
    if (!r)
        r = mbedtls_ssl_setup(&s->ssl, &s->conf);
    if (r) {
        tls_error("setup", r);
        return -1;
    }
    mbedtls_ssl_set_bio(&s->ssl, s->c, bio_send, bio_recv, 0);
    while ((r = mbedtls_ssl_handshake(&s->ssl)) != 0)
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            tls_error("handshake", r);
            return -1;
        }
    s->tls = 1;
    say("tls: %s, %s", mbedtls_ssl_get_version(&s->ssl), mbedtls_ssl_get_ciphersuite(&s->ssl));
    return 0;
}

int stream_write(struct stream* s, const unsigned char* b, unsigned n)
{
    while (n) {
        int r;
        if (!s->tls) {
            const unsigned k = n > 16000 ? 16000 : n;
            if (tcp_send(s->c, TCP_ACK, b, k))
                return -1;
            b += k;
            n -= k;
            continue;
        }
        r = mbedtls_ssl_write(&s->ssl, b, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (r <= 0) {
            tls_error("write", r);
            return -1;
        }
        b += r;
        n -= (unsigned)r;
    }
    return 0;
}

static int stream_read(struct stream* s, unsigned char* b, unsigned n, unsigned ms)
{
    if (!s->tls)
        return tcp_take(s->c, b, n, ms);
    while (n) {
        const int r = mbedtls_ssl_read(&s->ssl, b, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (r <= 0) {
            if (r != MBEDTLS_ERR_SSL_TIMEOUT)
                tls_error("read", r);
            return -1;
        }
        b += r;
        n -= (unsigned)r;
    }
    return 0;
}

int stream_read_some(struct stream* s, unsigned char* b, unsigned n, unsigned ms)
{
    int r;
    if (!s->tls) {
        if (!s->c->datan && tcp_poll(ms) < 0)
            return -1;
        if (!s->c->datan)
            return s->c->closed ? -1 : 0;
        r = (int)(s->c->datan < n ? s->c->datan : n);
        tcp_take(s->c, b, (unsigned)r, 0);
        return r;
    }
    g_recv_ms = ms;
    r = mbedtls_ssl_read(&s->ssl, b, n);
    g_recv_ms = 8000;
    if (r == MBEDTLS_ERR_SSL_TIMEOUT || r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
        return 0;
    if (r <= 0) {
        tls_error("read", r);
        return -1;
    }
    return r;
}

/* ---- lockdown ---- */

static char g_answer[65536];

/* the value after <key>key</key>: a string, integer or data element's text, or "true" / "false" */
static int plist_value(const char* xml, const char* key, char* out, unsigned size)
{
    char k[96];
    const char *p, *e;
    snprintf(k, sizeof(k), "<key>%s</key>", key);
    p = strstr(xml, k);
    if (!p)
        return 0;
    p += strlen(k);
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')
        ++p;
    if (!strncmp(p, "<true/>", 7) || !strncmp(p, "<false/>", 8)) {
        snprintf(out, size, "%s", p[1] == 't' ? "true" : "false");
        return 1;
    }
    p = strchr(p, '>');
    if (!p)
        return 0;
    ++p;
    e = strchr(p, '<');
    if (!e || (unsigned)(e - p) >= size)
        return 0;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = 0;
    return 1;
}

static int lockdown_request(struct stream* s, const char* body)
{
    static char msg[16384];
    static const char head[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD "
                               "PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist "
                               "version=\"1.0\">\n<dict>\n<key>Label</key>\n<string>nbt</string>\n";
    unsigned char len[4];
    unsigned n;
    const int m = snprintf(msg + 4, sizeof(msg) - 4, "%s%s</dict>\n</plist>\n", head, body);
    if (m <= 0 || m >= (int)sizeof(msg) - 4)
        return -1;
    put32((unsigned char*)msg, (unsigned)m);
    if (stream_write(s, (const unsigned char*)msg, 4 + (unsigned)m) || stream_read(s, len, 4, 10000)) {
        say("lockdown: no answer");
        return -1;
    }
    n = get32(len);
    if (n >= sizeof(g_answer) || stream_read(s, (unsigned char*)g_answer, n, 10000)) {
        say("lockdown: answer of %u bytes not read", n);
        return -1;
    }
    g_answer[n] = 0;
    return 0;
}

static int lockdown_error(char* e, unsigned size)
{
    if (!plist_value(g_answer, "Error", e, size))
        return 0;
    say("lockdown: error %s", e);
    return 1;
}

/* ---- the pair record (state directory, pair-*) ---- */

static struct {
    char host_id[40], buid[40];
    char root_key[4096], root_cert[4096], host_key[4096], host_cert[4096], device_cert[4096];
} g_pair;

static int get_text(const char* name, char* out, unsigned size)
{
    const int n = state_read(name, out, size - 1);
    if (n <= 0)
        return -1;
    out[n] = 0;
    return 0;
}

static int pair_load(void)
{
    char done[8];
    return !get_text("pair-host-id", g_pair.host_id, sizeof(g_pair.host_id)) &&
                   !get_text("pair-buid", g_pair.buid, sizeof(g_pair.buid)) &&
                   !get_text("pair-root-key.pem", g_pair.root_key, sizeof(g_pair.root_key)) &&
                   !get_text("pair-root-cert.pem", g_pair.root_cert, sizeof(g_pair.root_cert)) &&
                   !get_text("pair-done", done, sizeof(done))
               ? 0
               : -1;
}

static void uuid(char* out)
{
    unsigned char r[16];
    cp_random(r, sizeof(r));
    r[6] = (unsigned char)((r[6] & 0x0f) | 0x40);
    r[8] = (unsigned char)((r[8] & 0x3f) | 0x80);
    sprintf(out, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", r[0], r[1], r[2], r[3], r[4],
            r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}

/* subject's key signed by the root key; ca = the root's own; ski = with a subject key identifier */
static int make_cert(mbedtls_pk_context* subject, mbedtls_pk_context* root, int ca, int ski, char* pem, unsigned size)
{
    mbedtls_x509write_cert crt;
    static const unsigned char serial[1] = {0};
    static const unsigned char bc_ca[] = {0x30, 0x03, 0x01, 0x01, 0xff}, bc_leaf[] = {0x30, 0x00};
    static const unsigned char ku[] = {0x03, 0x02, 0x05, 0xa0};
    int r;
    mbedtls_x509write_crt_init(&crt);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, subject);
    mbedtls_x509write_crt_set_issuer_key(&crt, root);
    r = mbedtls_x509write_crt_set_serial_raw(&crt, (unsigned char*)serial, 1);
    if (!r)
        r = mbedtls_x509write_crt_set_validity(&crt, "20260101000000", "20360101000000");
    if (!r)
        r = mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_BASIC_CONSTRAINTS,
                                                MBEDTLS_OID_SIZE(MBEDTLS_OID_BASIC_CONSTRAINTS), 1,
                                                ca ? bc_ca : bc_leaf, ca ? sizeof(bc_ca) : sizeof(bc_leaf));
    if (!r && ski) {
        unsigned char der[600], v[22];
        unsigned char* end = der + sizeof(der);
        const int n = mbedtls_pk_write_pubkey(&end, der, subject);
        if (n <= 0)
            r = -1;
        else {
            v[0] = 0x04;
            v[1] = 20;
            r = mbedtls_sha1(der + sizeof(der) - n, (size_t)n, v + 2);
            if (!r)
                r = mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_SUBJECT_KEY_IDENTIFIER,
                                                        MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_KEY_IDENTIFIER), 0, v,
                                                        sizeof(v));
        }
    }
    if (!r && !ca)
        r = mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_KEY_USAGE, MBEDTLS_OID_SIZE(MBEDTLS_OID_KEY_USAGE),
                                                1, ku, sizeof(ku));
    if (!r)
        r = mbedtls_x509write_crt_pem(&crt, (unsigned char*)pem, size, cp_rng, 0);
    mbedtls_x509write_crt_free(&crt);
    if (r)
        tls_error("certificate", r);
    return r ? -1 : 0;
}

static int gen_key(mbedtls_pk_context* k, char* pem, unsigned size)
{
    const long long t0 = ms_now();
    int r = mbedtls_pk_setup(k, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (!r)
        r = mbedtls_rsa_gen_key(mbedtls_pk_rsa(*k), cp_rng, 0, 2048, 65537);
    if (!r && pem)
        r = mbedtls_pk_write_key_pem(k, (unsigned char*)pem, size);
    if (r)
        tls_error("RSA key", r);
    else
        say("pair: RSA-2048 key made in %lld ms", ms_now() - t0);
    return r ? -1 : 0;
}

static void put_text(const char* name, const char* text)
{
    state_write(name, text, (unsigned)strlen(text));
}

/* A key of the pair record: the stored one, else a new one that is stored at once. Making one takes 6 to 56 s on the
 * head unit, and until 2026.11.26 both were made again at every attempt and only stored after "Trust". An iPhone that
 * drops off USB during those seconds (logs of issue #21: after about 10 s, more than 70 attempts) then never got as
 * far as the trust question. The keys do not depend on the iPhone; pair_load() still asks for pair-done, so stored
 * keys alone are not taken as a finished pairing. */
static int stored_key(mbedtls_pk_context* k, const char* name, char* pem, unsigned size)
{
    if (!get_text(name, pem, size)) {
        if (!mbedtls_pk_parse_key(k, (const unsigned char*)pem, strlen(pem) + 1, 0, 0, cp_rng, 0)) {
            say("pair: %s from the state directory", name);
            return 0;
        }
        say("pair: %s in the state directory cannot be read: a new one", name);
        mbedtls_pk_free(k);
        mbedtls_pk_init(k);
    }
    if (gen_key(k, pem, size))
        return -1;
    put_text(name, pem);
    return 0;
}

static int pair_make(const char* device_pem)
{
    mbedtls_pk_context root, host, device;
    int r = -1;
    mbedtls_pk_init(&root);
    mbedtls_pk_init(&host);
    mbedtls_pk_init(&device);
    if (mbedtls_pk_parse_public_key(&device, (const unsigned char*)device_pem, strlen(device_pem) + 1)) {
        say("pair: the iPhone's key could not be read");
        goto out;
    }
    /* the certificates are signed anew each time (milliseconds; the device's hangs on this iPhone's key) */
    if (stored_key(&root, "pair-root-key.pem", g_pair.root_key, sizeof(g_pair.root_key)) ||
        stored_key(&host, "pair-host-key.pem", g_pair.host_key, sizeof(g_pair.host_key)) ||
        make_cert(&root, &root, 1, 0, g_pair.root_cert, sizeof(g_pair.root_cert)) ||
        make_cert(&host, &root, 0, 0, g_pair.host_cert, sizeof(g_pair.host_cert)) ||
        make_cert(&device, &root, 0, 1, g_pair.device_cert, sizeof(g_pair.device_cert)))
        goto out;
    if (get_text("pair-host-id", g_pair.host_id, sizeof(g_pair.host_id)) || strlen(g_pair.host_id) != 36) {
        uuid(g_pair.host_id);
        put_text("pair-host-id", g_pair.host_id);
    }
    r = 0;
out:
    mbedtls_pk_free(&root);
    mbedtls_pk_free(&host);
    mbedtls_pk_free(&device);
    return r;
}

static void strip_space(char* s)
{
    char* d = s;
    for (; *s; ++s)
        if (*s != '\n' && *s != '\r' && *s != '\t' && *s != ' ')
            *d++ = *s;
    *d = 0;
}

static void pem_base64(const char* pem, char* out, unsigned size)
{
    size_t n = 0;
    if (mbedtls_base64_encode((unsigned char*)out, size, &n, (const unsigned char*)pem, strlen(pem)))
        n = 0;
    out[n] = 0;
}

static int lockdown_connect(struct stream* s)
{
    memset(s, 0, sizeof(*s));
    s->c = tcp_open(0, LOCKDOWN_PORT);
    return s->c ? 0 : -1;
}

/* "Trust": until it is answered, Pair says PairingDialogResponsePending; asked again (a new connection each time)
 * for up to 2 minutes */
static int pair(void)
{
    struct stream s;
    static char value[4096], device_pem[2048], body[16384], dev64[3000], host64[3000], root64[3000];
    size_t n = 0;
    long long until;
    if (!g_pair.buid[0] && (get_text("pair-buid", g_pair.buid, sizeof(g_pair.buid)) || strlen(g_pair.buid) != 36)) {
        uuid(g_pair.buid);
        put_text("pair-buid", g_pair.buid);
    }
    if (lockdown_connect(&s))
        return -1;
    snprintf(body, sizeof(body),
             "<key>Request</key>\n<string>SetValue</string>\n<key>Key</key>\n<string>UntrustedHostBUID</string>\n"
             "<key>Value</key>\n<string>%s</string>\n",
             g_pair.buid);
    if (lockdown_request(&s, body))
        return -1;
    if (lockdown_request(&s, "<key>Request</key>\n<string>GetValue</string>\n<key>Key</key>\n"
                             "<string>DevicePublicKey</string>\n") ||
        !plist_value(g_answer, "Value", value, sizeof(value)) || (strip_space(value), 0) ||
        mbedtls_base64_decode((unsigned char*)device_pem, sizeof(device_pem) - 1, &n, (const unsigned char*)value,
                              strlen(value))) {
        say("pair: no DevicePublicKey");
        return -1;
    }
    device_pem[n] = 0;
    tcp_close(s.c);
    say("pair: making the pair record (two RSA-2048 keys, made once and kept)");
    if (pair_make(device_pem))
        return -1;
    pem_base64(g_pair.device_cert, dev64, sizeof(dev64));
    pem_base64(g_pair.host_cert, host64, sizeof(host64));
    pem_base64(g_pair.root_cert, root64, sizeof(root64));
    snprintf(body, sizeof(body),
             "<key>PairRecord</key>\n<dict>\n<key>DeviceCertificate</key>\n<data>%s</data>\n"
             "<key>HostCertificate</key>\n<data>%s</data>\n<key>HostID</key>\n<string>%s</string>\n"
             "<key>RootCertificate</key>\n<data>%s</data>\n<key>SystemBUID</key>\n<string>%s</string>\n</dict>\n"
             "<key>Request</key>\n<string>Pair</string>\n<key>ProtocolVersion</key>\n<string>2</string>\n"
             "<key>PairingOptions</key>\n<dict>\n<key>ExtendedPairingErrors</key>\n<true/>\n</dict>\n",
             dev64, host64, g_pair.host_id, root64, g_pair.buid);
    say("pair: asking the iPhone (answer \"Trust\" on it, with the passcode if it asks)");
    flag_set("/fs/ram3/nbt-carplay-trust", 0);
    until = ms_now() + 120000;
    while (ms_now() < until && !g_stop) {
        if (lockdown_connect(&s))
            break;
        if (lockdown_request(&s, body))
            break;
        tcp_close(s.c);
        if (!plist_value(g_answer, "Error", value, sizeof(value))) {
            say("pair: paired");
            flag_clear("/fs/ram3/nbt-carplay-trust");
            put_text("pair-host-id", g_pair.host_id);
            put_text("pair-buid", g_pair.buid);
            put_text("pair-root-key.pem", g_pair.root_key);
            put_text("pair-root-cert.pem", g_pair.root_cert);
            put_text("pair-host-key.pem", g_pair.host_key);
            put_text("pair-host-cert.pem", g_pair.host_cert);
            put_text("pair-device-cert.pem", g_pair.device_cert);
            if (plist_value(g_answer, "EscrowBag", value, sizeof(value))) {
                strip_space(value);
                put_text("pair-escrow.b64", value);
            }
            put_text("pair-done", "1\n");
            return 0;
        }
        if (strcmp(value, "PairingDialogResponsePending")) {
            say("pair: refused: %s", value);
            break;
        }
        sleep_ms(1000);
    }
    flag_clear("/fs/ram3/nbt-carplay-trust");
    if (ms_now() >= until)
        say("pair: no answer on the iPhone within 2 minutes");
    return -1;
}

static int pair_identity(mbedtls_x509_crt* cert, mbedtls_pk_context* key)
{
    int r = mbedtls_x509_crt_parse(cert, (const unsigned char*)g_pair.root_cert, strlen(g_pair.root_cert) + 1);
    if (!r)
        r = mbedtls_pk_parse_key(key, (const unsigned char*)g_pair.root_key, strlen(g_pair.root_key) + 1, 0, 0, cp_rng,
                                 0);
    if (r)
        tls_error("pair record", r);
    return r ? -1 : 0;
}

/* ---- the whole way to the carkit service ---- */

static struct stream g_session, g_carkit;
static mbedtls_x509_crt g_cert;
static mbedtls_pk_context g_key;

static int start_session(void)
{
    char body[512], value[256];
    if (lockdown_connect(&g_session))
        return -1;
    snprintf(body, sizeof(body),
             "<key>Request</key>\n<string>StartSession</string>\n<key>HostID</key>\n<string>%s</string>\n"
             "<key>SystemBUID</key>\n<string>%s</string>\n",
             g_pair.host_id, g_pair.buid);
    if (lockdown_request(&g_session, body))
        return -1;
    if (lockdown_error(value, sizeof(value))) {
        tcp_close(g_session.c);
        return !strcmp(value, "InvalidHostID") ? -2 : -1;   /* -2: the iPhone forgot us */
    }
    if (plist_value(g_answer, "EnableSessionSSL", value, sizeof(value)) && !strcmp(value, "true")) {
        mbedtls_x509_crt_free(&g_cert);
        mbedtls_pk_free(&g_key);
        mbedtls_x509_crt_init(&g_cert);
        mbedtls_pk_init(&g_key);
        if (pair_identity(&g_cert, &g_key) || stream_tls(&g_session, &g_cert, &g_key))
            return -1;
    } else
        say("lockdown: session without TLS");
    return 0;
}

int carkit_connect(struct usb* u, struct stream** out)
{
    char value[256];
    unsigned port;
    int ssl, r;
    memset(&g_mux, 0, sizeof(g_mux));
    memset(g_tc, 0, sizeof(g_tc));
    g_mux.u = u;
    {   /* version 2 */
        unsigned char v[20];
        static unsigned char f[70000];
        unsigned n;
        memset(v, 0, sizeof(v));
        put32(v + 4, 20);
        put32(v + 8, 2);
        if (mux_send_raw(v, 20))
            return -1;
        for (;;) {
            r = mux_frame(f, &n, 5000);
            if (r <= 0) {
                say("mux: no version answer");
                return -1;
            }
            if (get32(f) == 0 && n == 20)
                break;
        }
        say("mux: version %u", get32(f + 8));
        v[0] = 7;
        if (mux_send(2, v, 1))
            return -1;
    }
    g_mux.next_port = 1 + (unsigned)(time(0) % 1000) * 16;
    if (pair_load() == 0)
        say("pair: record from the state directory (HostID %s)", g_pair.host_id);
    else if (pair())
        return -1;
    r = start_session();
    if (r == -2) {
        say("pair: the iPhone does not know this record any more: pairing again");
        memset(&g_pair, 0, sizeof(g_pair));
        if (pair() || start_session())
            return -1;
    } else if (r)
        return -1;
    if (lockdown_request(&g_session, "<key>Request</key>\n<string>StartService</string>\n<key>Service</key>\n"
                                     "<string>com.apple.carkit.service</string>\n") ||
        lockdown_error(value, sizeof(value)) || !plist_value(g_answer, "Port", value, sizeof(value)))
        return -1;
    port = (unsigned)atoi(value);
    ssl = plist_value(g_answer, "EnableServiceSSL", value, sizeof(value)) && !strcmp(value, "true");
    say("carkit: port %u%s", port, ssl ? ", TLS" : "");
    if (ssl && !g_cert.raw.p) {   /* a session without TLS: the identity is still needed here */
        mbedtls_x509_crt_init(&g_cert);
        mbedtls_pk_init(&g_key);
        if (pair_identity(&g_cert, &g_key))
            return -1;
    }
    memset(&g_carkit, 0, sizeof(g_carkit));
    g_carkit.c = tcp_open(1, port);
    if (!g_carkit.c || (ssl && stream_tls(&g_carkit, &g_cert, &g_key)))
        return -1;
    say("carkit: connected");
    *out = &g_carkit;
    return 0;
}

void carkit_close(void)
{
    unsigned i;
    for (i = 0; i < 3; ++i)
        tcp_close(&g_tc[i]);
}
