/* lwIP compiler / platform definitions for nbt_carplay (QNX 6.5 x86, also Linux for the host tests).
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 */
#ifndef NBT_CARPLAY_LWIP_CC_H
#define NBT_CARPLAY_LWIP_CC_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

/* struct timeval from the system (stdlib.h / unistd.h may already bring it) */
#define LWIP_TIMEVAL_PRIVATE 0
#define LWIP_ERRNO_STDINCLUDE 1

#define LWIP_PLATFORM_DIAG(x) \
    do {                      \
        printf x;             \
    } while (0)
#define LWIP_PLATFORM_ASSERT(x)                                                  \
    do {                                                                         \
        printf("lwip: assertion \"%s\" failed at %s:%d\n", x, __FILE__, __LINE__); \
        fflush(stdout);                                                          \
        abort();                                                                 \
    } while (0)

unsigned cp_rand32(void);
#define LWIP_RAND() ((u32_t)cp_rand32())

#endif
