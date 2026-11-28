/* nbt_carplay: the cryptography AirPlay uses (Mbed TLS, Monocypher for Ed25519 / X25519).
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 */
#ifndef NBT_CARPLAY_CRYPTO_H
#define NBT_CARPLAY_CRYPTO_H

/* HKDF-SHA-512 with text salt and info */
void hkdf512(const unsigned char* ikm, unsigned ikmn, const char* salt, const char* info, unsigned char* out,
             unsigned outn);
/* ChaCha20-Poly1305 (IETF): out = ciphertext + 16-byte tag; open: in includes the tag, 0 = authentic */
int aead_seal(const unsigned char key[32], const unsigned char nonce[12], const unsigned char* aad, unsigned aadn,
              const unsigned char* in, unsigned n, unsigned char* out);
int aead_open(const unsigned char key[32], const unsigned char nonce[12], const unsigned char* aad, unsigned aadn,
              const unsigned char* in, unsigned n, unsigned char* out);
void nonce_label(unsigned char nonce[12], const char* label);          /* 4 zero bytes, 8 ASCII bytes */
void nonce_counter(unsigned char nonce[12], unsigned long long counter);   /* 4 zero bytes, 8 bytes LE */

void sha1_of(unsigned char out[20], const unsigned char* a, unsigned an, const unsigned char* b, unsigned bn);
void sha256_of(unsigned char out[32], const unsigned char* a, unsigned an, const unsigned char* b, unsigned bn);
void sha512_of(unsigned char out[64], const unsigned char* a, unsigned an, const unsigned char* b, unsigned bn);
int aes128_ctr(const unsigned char key[16], const unsigned char iv[16], const unsigned char* in, unsigned n,
               unsigned char* out);

void ed25519_keys(const unsigned char seed[32], unsigned char secret[64], unsigned char pub[32]);
void ed25519_sign(unsigned char sig[64], const unsigned char secret[64], const unsigned char* msg, unsigned n);
int ed25519_verify(const unsigned char sig[64], const unsigned char pub[32], const unsigned char* msg, unsigned n);
void x25519_keys(unsigned char secret[32], unsigned char pub[32]);   /* a new random pair */
void x25519_shared(unsigned char out[32], const unsigned char secret[32], const unsigned char peer[32]);

/* SRP-6a server side, 3072-bit group (RFC 5054), g = 5, SHA-512, as HomeKit / AirPlay pairing */
struct srp {
    unsigned char salt[16];
    unsigned char B[384];
    unsigned char b[32];
    unsigned char v[384];
    char user[32];
};
int srp_start(struct srp* s, const char* user, const char* password);
int srp_verify(struct srp* s, const unsigned char* A, unsigned An, const unsigned char* M1, unsigned M1n,
               unsigned char K[64], unsigned char M2[64]);   /* 0 = the client's proof is right */

#endif
