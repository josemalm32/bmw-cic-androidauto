/* v47 LIVE1: shared-memory ring HmiMain(libcicaa) -> cicaalive (own DSP writer).
 * Single producer (libcicaa ring thread), single consumer (cicaalive).
 * Producer never blocks: drops when full and then waits for next IDR.
 * Latest avcC kept in header so a late reader can start without a config record. */
#ifndef AA_RING_H
#define AA_RING_H
#include <stdint.h>
#define RING_NAME   "/cicaa_live"
#define RING_MAGIC  0x4c495631u   /* 'LIV1' */
#define RING_DATA   (2u*1024u*1024u)
#define RING_CFGMAX 512u
#define REC_FRAME   2u
#define REC_WRAP    0xffffu
typedef struct {
    uint32_t magic, size;
    volatile uint32_t wpos, rpos;     /* byte offsets in data[] */
    volatile uint32_t reader_pid;     /* set by cicaalive while attached, 0 when gone */
    volatile uint32_t want_idr;       /* reader asks producer for a fresh IDR */
    volatile uint32_t cfg_gen, cfg_len;
    volatile uint32_t frames_in, frames_drop, width, height;
    uint8_t cfg[RING_CFGMAX];         /* avcC */
    uint32_t pad[8];
} ring_hdr;
typedef struct { uint32_t kind, len, idr, pad; uint64_t pts_us; } ring_rec; /* + len bytes, 4-aligned */
#define RING_BYTES (sizeof(ring_hdr)+RING_DATA)
#endif
