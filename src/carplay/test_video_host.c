/* Host test for the shared-memory video contract used by cicaalive.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <assert.h>
#include "cp_screen.c"

struct cp_options g_opt;
static unsigned key_requests;

long long ms_now(void) { return 123; }
void airplay_key_frame(void) { ++key_requests; }

int main(void)
{
    static const unsigned char cfg[] = {1, 66, 0, 31, 255, 225, 0, 1, 0x67, 1, 0, 1, 0x68};
    static const unsigned char idr[] = {0, 0, 0, 2, 0x65, 0x88};
    static const unsigned char delta[] = {0, 0, 0, 2, 0x41, 0x9a};
    const unsigned stride = (sizeof(ring_rec) + sizeof(idr) + 3u) & ~3u;
    ring_rec record;
    unsigned char *data;
    uint32_t wrap;

    /* Anonymous storage keeps the fixture isolated from any running receiver. */
    cic_ring = calloc(1, RING_BYTES);
    assert(cic_ring);
    data = (unsigned char *)(cic_ring + 1);
    g_opt.width = 800;
    g_opt.height = 480;
    cic_config(cfg, sizeof(cfg));
    assert(cic_ring->cfg_len == sizeof(cfg) && cic_ring->cfg_gen == 1);
    assert(!memcmp(cic_ring->cfg, cfg, sizeof(cfg)));
    assert(cic_ring->width == 800 && cic_ring->height == 480);

    cic_frame(idr, sizeof(idr), 1);
    assert(cic_ring->wpos == 0);
    cic_ring->reader_pid = 1;
    cic_frame(delta, sizeof(delta), 0);
    assert(cic_ring->wpos == 0);
    cic_frame(idr, sizeof(idr), 1);
    memcpy(&record, data, sizeof(record));
    assert(record.kind == REC_FRAME && record.idr == 1 && record.pts_us == 123000);
    assert(record.len == sizeof(idr) && cic_ring->wpos == stride);
    assert(!memcmp(data + sizeof(record), idr, sizeof(idr)));

    cic_ring->wpos = RING_DATA - 4;
    cic_ring->rpos = stride * 2;
    cic_frame(delta, sizeof(delta), 0);
    memcpy(&wrap, data + RING_DATA - 4, sizeof(wrap));
    assert(wrap == REC_WRAP && cic_ring->wpos == stride);
    memcpy(&record, data, sizeof(record));
    assert(!record.idr && !memcmp(data + sizeof(record), delta, sizeof(delta)));

    cic_ring->rpos = stride + 4;
    cic_frame(delta, sizeof(delta), 0);
    assert(cic_ring->frames_drop == 1 && key_requests == 1);
    cic_ring->rpos = stride * 4;
    cic_frame(delta, sizeof(delta), 0);
    assert(cic_ring->wpos == stride);
    cic_frame(idr, sizeof(idr), 1);
    assert(cic_ring->wpos == stride * 2);
    cic_config(cfg, 4);
    assert(cic_ring->cfg_len == 0);
    cic_frame(idr, sizeof(idr), 1);
    assert(cic_ring->wpos == stride * 2);
    free(cic_ring);
    puts("CIC video ring: configuration, IDR gating, wrap and backpressure passed");
    return 0;
}
