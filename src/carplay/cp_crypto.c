/* nbt_carplay: the cryptography AirPlay uses.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 *
 * SRP as xcertplay's (and HomeKit's): k = H(N | pad(g)), x = H(salt | H(user ":" password)), v = g^x,
 * B = k v + g^b, u = H(pad(A) | pad(B)), S = (A v^u)^b, K = H(S without leading zeros),
 * M1 = H(H(N) xor H(g) | H(user) | salt | pad(A) | pad(B) | K), M2 = H(pad(A) | M1 | K); pad = 384 bytes.
 */
#include <stdio.h>
#include <string.h>
#include "mbedtls/aes.h"
#include "mbedtls/bignum.h"
#include "mbedtls/chachapoly.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include "cp.h"
#include "cp_crypto.h"

void hkdf512(const unsigned char* ikm, unsigned ikmn, const char* salt, const char* info, unsigned char* out,
             unsigned outn)
{
    mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA512), (const unsigned char*)salt, strlen(salt), ikm, ikmn,
                 (const unsigned char*)info, strlen(info), out, outn);
}

int aead_seal(const unsigned char key[32], const unsigned char nonce[12], const unsigned char* aad, unsigned aadn,
              const unsigned char* in, unsigned n, unsigned char* out)
{
    mbedtls_chachapoly_context c;
    int r;
    mbedtls_chachapoly_init(&c);
    r = mbedtls_chachapoly_setkey(&c, key);
    if (!r)
        r = mbedtls_chachapoly_encrypt_and_tag(&c, n, nonce, aad, aadn, in, out, out + n);
    mbedtls_chachapoly_free(&c);
    return r ? -1 : 0;
}

int aead_open(const unsigned char key[32], const unsigned char nonce[12], const unsigned char* aad, unsigned aadn,
              const unsigned char* in, unsigned n, unsigned char* out)
{
    mbedtls_chachapoly_context c;
    int r;
    if (n < 16)
        return -1;
    mbedtls_chachapoly_init(&c);
    r = mbedtls_chachapoly_setkey(&c, key);
    if (!r)
        r = mbedtls_chachapoly_auth_decrypt(&c, n - 16, nonce, aad, aadn, in + n - 16, in, out);
    mbedtls_chachapoly_free(&c);
    return r ? -1 : 0;
}

void nonce_label(unsigned char nonce[12], const char* label)
{
    memset(nonce, 0, 12);
    memcpy(nonce + 4, label, strlen(label) < 8 ? strlen(label) : 8);
}

void nonce_counter(unsigned char nonce[12], unsigned long long counter)
{
    memset(nonce, 0, 4);
    put64le(nonce + 4, counter);
}

void sha1_of(unsigned char out[20], const unsigned char* a, unsigned an, const unsigned char* b, unsigned bn)
{
    mbedtls_sha1_context c;
    mbedtls_sha1_init(&c);
    mbedtls_sha1_starts(&c);
    mbedtls_sha1_update(&c, a, an);
    if (bn)
        mbedtls_sha1_update(&c, b, bn);
    mbedtls_sha1_finish(&c, out);
    mbedtls_sha1_free(&c);
}

void sha256_of(unsigned char out[32], const unsigned char* a, unsigned an, const unsigned char* b, unsigned bn)
{
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, a, an);
    if (bn)
        mbedtls_sha256_update(&c, b, bn);
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
}

void sha512_of(unsigned char out[64], const unsigned char* a, unsigned an, const unsigned char* b, unsigned bn)
{
    mbedtls_sha512_context c;
    mbedtls_sha512_init(&c);
    mbedtls_sha512_starts(&c, 0);
    mbedtls_sha512_update(&c, a, an);
    if (bn)
        mbedtls_sha512_update(&c, b, bn);
    mbedtls_sha512_finish(&c, out);
    mbedtls_sha512_free(&c);
}

int aes128_ctr(const unsigned char key[16], const unsigned char iv[16], const unsigned char* in, unsigned n,
               unsigned char* out)
{
    mbedtls_aes_context c;
    unsigned char counter[16], block[16];
    size_t off = 0;
    int r;
    memcpy(counter, iv, 16);
    mbedtls_aes_init(&c);
    r = mbedtls_aes_setkey_enc(&c, key, 128);
    if (!r)
        r = mbedtls_aes_crypt_ctr(&c, n, &off, counter, block, in, out);
    mbedtls_aes_free(&c);
    return r ? -1 : 0;
}

void ed25519_keys(const unsigned char seed[32], unsigned char secret[64], unsigned char pub[32])
{
    unsigned char s[32];
    memcpy(s, seed, 32);   /* Monocypher wipes the seed it gets */
    crypto_ed25519_key_pair(secret, pub, s);
}

void ed25519_sign(unsigned char sig[64], const unsigned char secret[64], const unsigned char* msg, unsigned n)
{
    crypto_ed25519_sign(sig, secret, msg, n);
}

int ed25519_verify(const unsigned char sig[64], const unsigned char pub[32], const unsigned char* msg, unsigned n)
{
    return crypto_ed25519_check(sig, pub, msg, n) ? -1 : 0;
}

void x25519_keys(unsigned char secret[32], unsigned char pub[32])
{
    cp_random(secret, 32);
    crypto_x25519_public_key(pub, secret);
}

void x25519_shared(unsigned char out[32], const unsigned char secret[32], const unsigned char peer[32])
{
    crypto_x25519(out, secret, peer);
}

/* ---- SRP-6a ---- */

static const char N_HEX[] =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B"
    "302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE6"
    "49286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB9ED529077096966D"
    "670C354E4ABC9804F1746C08CA18217C32905E462E36CE3BE39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF695581718"
    "3995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7D"
    "B3970F85A6E1E4C7ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
    "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";

#define SRP_BYTES 384

static void sha512_start(mbedtls_sha512_context* c)
{
    mbedtls_sha512_init(c);
    mbedtls_sha512_starts(c, 0);
}

static void sha512_end(mbedtls_sha512_context* c, unsigned char out[64])
{
    mbedtls_sha512_finish(c, out);
    mbedtls_sha512_free(c);
}

int srp_start(struct srp* s, const char* user, const char* password)
{
    mbedtls_mpi N, g, x, v, b, k, t, B;
    mbedtls_sha512_context c;
    unsigned char h[64], nb[SRP_BYTES], gb[SRP_BYTES];
    int r;
    snprintf(s->user, sizeof(s->user), "%s", user);
    cp_random(s->salt, sizeof(s->salt));
    cp_random(s->b, sizeof(s->b));
    mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&g);
    mbedtls_mpi_init(&x);
    mbedtls_mpi_init(&v);
    mbedtls_mpi_init(&b);
    mbedtls_mpi_init(&k);
    mbedtls_mpi_init(&t);
    mbedtls_mpi_init(&B);
    r = mbedtls_mpi_read_string(&N, 16, N_HEX);
    if (!r)
        r = mbedtls_mpi_lset(&g, 5);
    /* x = H(salt | H(user ":" password)) */
    sha512_start(&c);
    mbedtls_sha512_update(&c, (const unsigned char*)user, strlen(user));
    mbedtls_sha512_update(&c, (const unsigned char*)":", 1);
    mbedtls_sha512_update(&c, (const unsigned char*)password, strlen(password));
    sha512_end(&c, h);
    sha512_start(&c);
    mbedtls_sha512_update(&c, s->salt, sizeof(s->salt));
    mbedtls_sha512_update(&c, h, 64);
    sha512_end(&c, h);
    if (!r)
        r = mbedtls_mpi_read_binary(&x, h, 64);
    if (!r)
        r = mbedtls_mpi_exp_mod(&v, &g, &x, &N, 0);
    /* k = H(N | pad(g)) */
    if (!r)
        r = mbedtls_mpi_write_binary(&N, nb, SRP_BYTES);
    if (!r)
        r = mbedtls_mpi_write_binary(&g, gb, SRP_BYTES);
    sha512_start(&c);
    mbedtls_sha512_update(&c, nb, SRP_BYTES);
    mbedtls_sha512_update(&c, gb, SRP_BYTES);
    sha512_end(&c, h);
    if (!r)
        r = mbedtls_mpi_read_binary(&k, h, 64);
    /* B = (k v + g^b) mod N */
    if (!r)
        r = mbedtls_mpi_read_binary(&b, s->b, sizeof(s->b));
    if (!r)
        r = mbedtls_mpi_exp_mod(&t, &g, &b, &N, 0);
    if (!r)
        r = mbedtls_mpi_mul_mpi(&B, &k, &v);
    if (!r)
        r = mbedtls_mpi_add_mpi(&B, &B, &t);
    if (!r)
        r = mbedtls_mpi_mod_mpi(&B, &B, &N);
    if (!r)
        r = mbedtls_mpi_write_binary(&B, s->B, SRP_BYTES);
    if (!r)
        r = mbedtls_mpi_write_binary(&v, s->v, SRP_BYTES);
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&g);
    mbedtls_mpi_free(&x);
    mbedtls_mpi_free(&v);
    mbedtls_mpi_free(&b);
    mbedtls_mpi_free(&k);
    mbedtls_mpi_free(&t);
    mbedtls_mpi_free(&B);
    return r ? -1 : 0;
}

int srp_verify(struct srp* s, const unsigned char* A, unsigned An, const unsigned char* M1, unsigned M1n,
               unsigned char K[64], unsigned char M2[64])
{
    mbedtls_mpi N, a, v, u, b, S, t;
    mbedtls_sha512_context c;
    unsigned char h[64], hn[64], hg[64], hu[64], pa[SRP_BYTES], sb[SRP_BYTES], nb[SRP_BYTES], m[64];
    unsigned i, sn;
    const unsigned char five = 5;
    int r;
    if (An == 0 || An > SRP_BYTES || M1n != 64)
        return -1;
    memset(pa, 0, sizeof(pa));
    memcpy(pa + SRP_BYTES - An, A, An);
    mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&a);
    mbedtls_mpi_init(&v);
    mbedtls_mpi_init(&u);
    mbedtls_mpi_init(&b);
    mbedtls_mpi_init(&S);
    mbedtls_mpi_init(&t);
    r = mbedtls_mpi_read_string(&N, 16, N_HEX);
    if (!r)
        r = mbedtls_mpi_read_binary(&a, pa, SRP_BYTES);
    if (!r)
        r = mbedtls_mpi_mod_mpi(&t, &a, &N);
    if (!r && mbedtls_mpi_cmp_int(&t, 0) == 0)
        r = -1;   /* A mod N = 0: refused */
    /* u = H(pad(A) | pad(B)) */
    sha512_start(&c);
    mbedtls_sha512_update(&c, pa, SRP_BYTES);
    mbedtls_sha512_update(&c, s->B, SRP_BYTES);
    sha512_end(&c, h);
    if (!r)
        r = mbedtls_mpi_read_binary(&u, h, 64);
    if (!r)
        r = mbedtls_mpi_read_binary(&v, s->v, SRP_BYTES);
    if (!r)
        r = mbedtls_mpi_read_binary(&b, s->b, sizeof(s->b));
    /* S = (A v^u)^b mod N */
    if (!r)
        r = mbedtls_mpi_exp_mod(&t, &v, &u, &N, 0);
    if (!r)
        r = mbedtls_mpi_mul_mpi(&t, &t, &a);
    if (!r)
        r = mbedtls_mpi_mod_mpi(&t, &t, &N);
    if (!r)
        r = mbedtls_mpi_exp_mod(&S, &t, &b, &N, 0);
    sn = (unsigned)mbedtls_mpi_size(&S);
    if (!r)
        r = mbedtls_mpi_write_binary(&S, sb, sn);
    if (!r)
        r = mbedtls_mpi_write_binary(&N, nb, SRP_BYTES);
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&a);
    mbedtls_mpi_free(&v);
    mbedtls_mpi_free(&u);
    mbedtls_mpi_free(&b);
    mbedtls_mpi_free(&S);
    mbedtls_mpi_free(&t);
    if (r)
        return -1;
    sha512_of(K, sb, sn, 0, 0);
    sha512_of(hn, nb, SRP_BYTES, 0, 0);
    sha512_of(hg, &five, 1, 0, 0);
    for (i = 0; i < 64; ++i)
        hn[i] ^= hg[i];
    sha512_of(hu, (const unsigned char*)s->user, (unsigned)strlen(s->user), 0, 0);
    sha512_start(&c);
    mbedtls_sha512_update(&c, hn, 64);
    mbedtls_sha512_update(&c, hu, 64);
    mbedtls_sha512_update(&c, s->salt, sizeof(s->salt));
    mbedtls_sha512_update(&c, pa, SRP_BYTES);
    mbedtls_sha512_update(&c, s->B, SRP_BYTES);
    mbedtls_sha512_update(&c, K, 64);
    sha512_end(&c, m);
    if (memcmp(m, M1, 64))
        return -1;
    sha512_start(&c);
    mbedtls_sha512_update(&c, pa, SRP_BYTES);
    mbedtls_sha512_update(&c, M1, 64);
    mbedtls_sha512_update(&c, K, 64);
    sha512_end(&c, M2);
    return 0;
}
