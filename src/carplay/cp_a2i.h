/* nbt_carplay: the NBT's audio manager client library (/opt/sys/lib/liba2i_client.so), loaded at run time.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 *
 * What is known of it (car tests 2026-11-07..20, see docs/ANDROID-AUTO.md): A2I_Client_Open(&handle, &config) with a
 * 76-byte configuration, then for output: GetBuffer, fill, Out_Write (the stream starts on the second write with the
 * "legacy" cookie), GetNotification = 0 when the next buffer is wanted; for input (microphone): Start, then
 * GetNotification = 0 when a buffer is filled, GetBuffer; Stop, Close.
 */
#ifndef NBT_CARPLAY_A2I_H
#define NBT_CARPLAY_A2I_H

#include <stddef.h>
#include <stdint.h>

#pragma pack(push, 4)
struct a2i_config {
    uint32_t magic;          /* 0x02000000 */
    uint32_t direction;      /* 0 output (we play), 1 input (we record) */
    uint32_t one;            /* 1 */
    uint32_t buffers;        /* buffers in flight */
    uint32_t buffer_bytes;
    uint32_t channels;
    uint32_t bytes_per_sample;
    uint32_t rate;
    uint32_t unused_20;
    const char* endpoint;    /* e.g. input_stereo_1 (an io-amp input), /output_multich_0 (microphone bus) */
    uint32_t interleaved;    /* 1 for more than one channel */
    uint32_t unused_2c;
    uint32_t unused_30;
    uint32_t cookie;         /* 0x05555555 (output), 0x0ABABABA (input) */
    uint32_t timeout_ms;
    uint32_t reserved[4];
};
#pragma pack(pop)

struct a2i {
    int (*open)(void** h, const struct a2i_config* c);
    int (*close)(void** h);
    int (*start)(void* h);
    int (*stop)(void* h);
    int (*notification)(void* h);
    int (*buffer)(void* h, void** b, size_t* n);
    int (*write)(void* h, const void* b, size_t n, uint32_t flags);
    const char* (*error)(int code);
};

int a2i_load(struct a2i* a);   /* 0 = the library and its functions are there */

#endif
