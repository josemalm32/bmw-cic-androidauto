/* lwIP operating system layer for nbt_carplay: POSIX threads (QNX 6.5, Linux).
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 */
#ifndef NBT_CARPLAY_LWIP_SYS_ARCH_H
#define NBT_CARPLAY_LWIP_SYS_ARCH_H

#define SYS_MBOX_NULL 0
#define SYS_SEM_NULL 0

struct sys_sem;
struct sys_mbox;
struct sys_mutex;
typedef struct sys_sem* sys_sem_t;
typedef struct sys_mbox* sys_mbox_t;
typedef struct sys_mutex* sys_mutex_t;
typedef unsigned long sys_thread_t;   /* pthread_t on QNX and Linux */
typedef int sys_prot_t;

#define sys_sem_valid(s) (*(s) != 0)
#define sys_sem_set_invalid(s) \
    do {                       \
        if (s)                 \
            *(s) = 0;          \
    } while (0)
#define sys_mbox_valid(m) (*(m) != 0)
#define sys_mbox_set_invalid(m) \
    do {                        \
        if (m)                  \
            *(m) = 0;           \
    } while (0)
#define sys_mutex_valid(m) (*(m) != 0)
#define sys_mutex_set_invalid(m) \
    do {                         \
        if (m)                   \
            *(m) = 0;            \
    } while (0)

/* one semaphore per thread (LWIP_NETCONN_SEM_PER_THREAD): a thread may read a socket while another one writes it */
sys_sem_t* cp_thread_sem(void);
void cp_thread_sem_free(void);
#define LWIP_NETCONN_THREAD_SEM_GET() cp_thread_sem()
#define LWIP_NETCONN_THREAD_SEM_ALLOC() ((void)cp_thread_sem())
#define LWIP_NETCONN_THREAD_SEM_FREE() cp_thread_sem_free()

#endif
