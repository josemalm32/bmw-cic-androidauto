/* nbt_carplay: wired CarPlay on the NBT. Common declarations of its parts (docs/CARPLAY.md).
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 * (license and the section 7 permission for the head unit's libraries: NOTICE.md, "GPLv3 parts")
 */
#ifndef NBT_CARPLAY_CP_H
#define NBT_CARPLAY_CP_H

#include <stddef.h>

/* ---- cp_util.c: log, time, bytes, random, files ---- */

void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void hexdump(const char* what, const void* b, unsigned n);
long long ms_now(void);
void sleep_ms(unsigned ms);

void put16(unsigned char* p, unsigned v);
void put32(unsigned char* p, unsigned v);
unsigned get16(const unsigned char* p);
unsigned get32(const unsigned char* p);
void put16le(unsigned char* p, unsigned v);
void put32le(unsigned char* p, unsigned v);
void put64le(unsigned char* p, unsigned long long v);
unsigned get16le(const unsigned char* p);
unsigned get32le(const unsigned char* p);
void tohex(char* out, const unsigned char* b, unsigned n);   /* lower case, out: 2n+1 */

int cp_random_init(void);
void cp_random(void* out, size_t n);
int cp_rng(void* ctx, unsigned char* out, size_t n);   /* Mbed TLS f_rng */
unsigned cp_rand32(void);

/* whole small files; state_*: in the persistent state directory (--state, default below) */
int file_read(const char* path, void* buf, unsigned size);   /* bytes read, -1 */
int file_write(const char* path, const void* data, unsigned n);   /* through name.tmp + rename */
int state_read(const char* name, void* buf, unsigned size);
int state_write(const char* name, const void* data, unsigned n);
void flag_set(const char* path, const char* text);   /* a RAM flag file (text may be 0: empty) */
void flag_clear(const char* path);

/* ---- options (nbt_carplay.c) ---- */

enum { STOP_NONE, STOP_USB, STOP_IAP2, STOP_NET, STOP_AIRPLAY, STOP_VIDEO };

struct cp_options {
    int width, height, fps;            /* the screen offered to the iPhone */
    int width_mm, height_mm;
    int stop_after;                    /* STOP_*: debugging, step by step */
    int seconds;                       /* with stop_after: how long that step runs */
    const char* state_dir;
    const char* fifo;                  /* H.264 Annex B for nbt_decoder */
    const char* input;                 /* key lines from nbt_knob */
    const char* pcap;                  /* NCM frames */
    const char* dump;                  /* H.264 copy */
    const char* audio_dump;            /* directory: one WAV per audio stream */
    int audio, mic;
    const char* media_out, *prompt_out;   /* A2I inputs: media, prompts (0 = none) */
    unsigned aux_source;               /* the car's main source of our media input (f6) */
    int knob_invert;
};
extern struct cp_options g_opt;
extern volatile int g_stop;            /* the whole program ends */
void cp_reached(int step);             /* a --stop-after step is done */

/* ---- cp_acp.c: the car's Apple authentication chip ---- */

int acp_open(void);
int acp_cert(const unsigned char** cert, unsigned* n);
int acp_sign(const unsigned char* challenge, unsigned clen, unsigned char* sig, unsigned* slen);
int acp_major(void);   /* authentication protocol major version (2 = 2.0B/C: SHA-1 digests) */

/* ---- cp_net_*.c: sockets on the iPhone link (lwIP) or the host's (tests) ---- */

struct net_addr {
    unsigned char ip[16];   /* IPv6, or IPv4 in the first 4 bytes (v4 = 1) */
    int v4;
    unsigned short port;
};
int net_init(void);
int net_tcp_listen(unsigned short port, unsigned short* bound);
int net_accept(int s, struct net_addr* peer, int timeout_ms);       /* socket, -2 timeout, -1 error */
int net_recv(int s, void* b, int n, int timeout_ms);                /* bytes, 0 closed, -1 error, -2 timeout */
int net_recv_all(int s, void* b, int n, int timeout_ms);            /* 0, -1 */
int net_send(int s, const void* b, int n);                          /* 0, -1 */
int net_udp(unsigned short port, unsigned short* bound);
int net_recvfrom(int s, void* b, int n, int timeout_ms, struct net_addr* from);
int net_sendto(int s, const void* b, int n, const struct net_addr* to);
int net_peer(int s, struct net_addr* a);
void net_close(int s);
const char* net_text(const struct net_addr* a, char* buf, unsigned size);

/* ---- cp_ncm.c: the CDC NCM link (NTB16) and its lwIP interface ---- */

typedef int (*ncm_send_fn)(void* ctx, const unsigned char* block, unsigned n);
int ncm_start(const unsigned char mac[6], ncm_send_fn send, void* ctx);
void ncm_input(const unsigned char* data, unsigned n);   /* bytes from the bulk IN pipe */
const char* ncm_address(void);                           /* our link-local IPv6 address as text */
void ncm_counts(unsigned long long* in, unsigned long long* out);   /* frames */
void ncm_pcap(const char* path);

/* ---- cp_iap2.c (iAP2 over the carkit service) ---- */

struct iap2_carplay {
    const char* address;     /* our IPv6 address on the NCM link */
    unsigned port;           /* AirPlay */
    const char* mac;         /* "aa:bb:cc:dd:ee:ff" */
    const char* public_key;  /* hex Ed25519 */
    const char* source_version;
    unsigned ncm_interface;  /* the NCM control interface (USB host transport component) */
};

struct stream;
int iap2_run(struct stream* s, const struct iap2_carplay* cp);   /* until the link ends */
int usb_gone(void);                                              /* cp_usb.c: the iPhone was unplugged */

/* ---- cp_airplay.c ---- */

#define AIRPLAY_PORT 7000
#define SOURCE_VERSION "950.7.1"
int airplay_identity(void);             /* load or make the Ed25519 identity */
const char* airplay_public_hex(void);
int airplay_start(unsigned short port);
void airplay_hid(unsigned uid, const unsigned char* report, unsigned n);
void airplay_siri(void);
void airplay_key_frame(void);
int airplay_connected(void);            /* the event channel is up */

#define HID_KNOB 0x2a2a2a2bu
#define HID_MEDIA 0x2a2a2a2cu
#define HID_PHONE 0x2a2a2a2du

/* ---- cp_screen.c ---- */

int screen_open(const unsigned char key[32], unsigned short* port);
void screen_close(void);

/* ---- cp_audio.c / cp_mic.c ---- */

int audio_start(void);    /* the A2I outputs */
void audio_stop(void);
int audio_stream_open(int type, const char* audio_type, unsigned long long format, unsigned long long connection,
                      const unsigned char key[32], unsigned short* data_port, unsigned short* control_port);
void audio_stream_close(int type);   /* -1: all */
struct bp;
void audio_feedback(struct bp* streams, unsigned long long ntp);
int audio_rate(unsigned long long format, int* channels);
void mic_start(const struct net_addr* to, const unsigned char key[32], int rate, int channels, int payload_type,
               int frames_per_packet);
void mic_stop(void);

/* ---- cp_input.c: nbt_knob's key lines ---- */

void input_start(void);

#endif
