/* v54 AUDIO1: shared-memory PCM rings libcicaa (HmiMain) -> cicaalive.
 * One byte ring per AA audio channel: 0 = media ch4 (48 kHz stereo S16LE),
 * 1 = speech ch5 and 2 = system ch6 (16 kHz mono S16LE).
 * Producer never blocks: wpos is a monotonic byte counter, it simply overwrites.
 * Reader keeps its own monotonic rpos; if wpos - rpos > ARING_CH it jumps ahead. */
#ifndef AA_ARING_H
#define AA_ARING_H
#include <stdint.h>
#define ARING_NAME  "/cicaa_audio"
#define ARING_MAGIC 0x41554431u   /* 'AUD1' */
#define ARING_CH    (256u*1024u)  /* bytes per channel ring (power of two) */
typedef struct {
    uint32_t magic, chsize;
    volatile uint32_t wpos[3];     /* monotonic bytes written */
    volatile uint32_t active[3];   /* AA stream started (AV_START) and not stopped */
    volatile uint32_t gen[3];      /* bumps on every AV_START */
    volatile uint32_t reader_pid;
    volatile uint32_t codec0;      /* v56: ch0 payload: 0/1 = PCM bytes, 2 = AAC records [u16 LE len][raw AU] */
    uint32_t pad[5];
} aring_hdr;
#define ARING_BYTES (sizeof(aring_hdr) + 3u * ARING_CH)
static inline uint8_t *aring_data(aring_hdr *h, int c) { return (uint8_t *)(h + 1) + (uint32_t)c * ARING_CH; }
#endif
