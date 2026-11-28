/* lwIP operating system layer for nbt_carplay: semaphores, mutexes, mailboxes and threads on POSIX threads.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lwip/opt.h"
#include "lwip/sys.h"

struct sys_sem {
    pthread_mutex_t m;
    pthread_cond_t c;
    unsigned count;
};

struct sys_mutex {
    pthread_mutex_t m;
};

struct sys_mbox {
    pthread_mutex_t m;
    pthread_cond_t not_empty, not_full;
    void** slot;
    int size, first, count;
};

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

u32_t sys_now(void)
{
    return (u32_t)now_ms();
}

void sys_init(void)
{
}

/* absolute CLOCK_REALTIME time ms from now (condition variables wait against the realtime clock) */
static void deadline(struct timespec* t, u32_t ms)
{
    clock_gettime(CLOCK_REALTIME, t);
    t->tv_sec += ms / 1000;
    t->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t->tv_nsec >= 1000000000L) {
        t->tv_sec++;
        t->tv_nsec -= 1000000000L;
    }
}

/* ---- semaphores ---- */

err_t sys_sem_new(sys_sem_t* sem, u8_t count)
{
    struct sys_sem* s = (struct sys_sem*)calloc(1, sizeof(*s));
    if (!s)
        return ERR_MEM;
    pthread_mutex_init(&s->m, 0);
    pthread_cond_init(&s->c, 0);
    s->count = count;
    *sem = s;
    return ERR_OK;
}

void sys_sem_signal(sys_sem_t* sem)
{
    struct sys_sem* s = *sem;
    pthread_mutex_lock(&s->m);
    s->count++;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
}

u32_t sys_arch_sem_wait(sys_sem_t* sem, u32_t timeout)
{
    struct sys_sem* s = *sem;
    const long long t0 = now_ms();
    struct timespec until;
    int r = 0;
    if (timeout)
        deadline(&until, timeout);
    pthread_mutex_lock(&s->m);
    while (!s->count) {
        if (timeout) {
            r = pthread_cond_timedwait(&s->c, &s->m, &until);
            if (r == ETIMEDOUT && !s->count) {
                pthread_mutex_unlock(&s->m);
                return SYS_ARCH_TIMEOUT;
            }
        } else
            pthread_cond_wait(&s->c, &s->m);
    }
    s->count--;
    pthread_mutex_unlock(&s->m);
    return (u32_t)(now_ms() - t0);
}

void sys_sem_free(sys_sem_t* sem)
{
    struct sys_sem* s = *sem;
    if (!s)
        return;
    pthread_cond_destroy(&s->c);
    pthread_mutex_destroy(&s->m);
    free(s);
    *sem = 0;
}

/* ---- mutexes ---- */

err_t sys_mutex_new(sys_mutex_t* mutex)
{
    struct sys_mutex* m = (struct sys_mutex*)calloc(1, sizeof(*m));
    if (!m)
        return ERR_MEM;
    pthread_mutex_init(&m->m, 0);
    *mutex = m;
    return ERR_OK;
}

void sys_mutex_lock(sys_mutex_t* mutex)
{
    pthread_mutex_lock(&(*mutex)->m);
}

void sys_mutex_unlock(sys_mutex_t* mutex)
{
    pthread_mutex_unlock(&(*mutex)->m);
}

void sys_mutex_free(sys_mutex_t* mutex)
{
    if (!*mutex)
        return;
    pthread_mutex_destroy(&(*mutex)->m);
    free(*mutex);
    *mutex = 0;
}

/* ---- mailboxes ---- */

err_t sys_mbox_new(sys_mbox_t* mbox, int size)
{
    struct sys_mbox* b = (struct sys_mbox*)calloc(1, sizeof(*b));
    if (size <= 0)
        size = 32;
    if (!b)
        return ERR_MEM;
    b->slot = (void**)calloc((size_t)size, sizeof(void*));
    if (!b->slot) {
        free(b);
        return ERR_MEM;
    }
    b->size = size;
    pthread_mutex_init(&b->m, 0);
    pthread_cond_init(&b->not_empty, 0);
    pthread_cond_init(&b->not_full, 0);
    *mbox = b;
    return ERR_OK;
}

static void put(struct sys_mbox* b, void* msg)
{
    b->slot[(b->first + b->count) % b->size] = msg;
    b->count++;
    pthread_cond_signal(&b->not_empty);
}

void sys_mbox_post(sys_mbox_t* mbox, void* msg)
{
    struct sys_mbox* b = *mbox;
    pthread_mutex_lock(&b->m);
    while (b->count == b->size)
        pthread_cond_wait(&b->not_full, &b->m);
    put(b, msg);
    pthread_mutex_unlock(&b->m);
}

err_t sys_mbox_trypost(sys_mbox_t* mbox, void* msg)
{
    struct sys_mbox* b = *mbox;
    err_t r = ERR_OK;
    pthread_mutex_lock(&b->m);
    if (b->count == b->size)
        r = ERR_MEM;
    else
        put(b, msg);
    pthread_mutex_unlock(&b->m);
    return r;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t* mbox, void* msg)
{
    return sys_mbox_trypost(mbox, msg);
}

static void* take(struct sys_mbox* b)
{
    void* msg = b->slot[b->first];
    b->first = (b->first + 1) % b->size;
    b->count--;
    pthread_cond_signal(&b->not_full);
    return msg;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t* mbox, void** msg, u32_t timeout)
{
    struct sys_mbox* b = *mbox;
    const long long t0 = now_ms();
    struct timespec until;
    void* m;
    if (timeout)
        deadline(&until, timeout);
    pthread_mutex_lock(&b->m);
    while (!b->count) {
        if (timeout) {
            if (pthread_cond_timedwait(&b->not_empty, &b->m, &until) == ETIMEDOUT && !b->count) {
                pthread_mutex_unlock(&b->m);
                if (msg)
                    *msg = 0;
                return SYS_ARCH_TIMEOUT;
            }
        } else
            pthread_cond_wait(&b->not_empty, &b->m);
    }
    m = take(b);
    pthread_mutex_unlock(&b->m);
    if (msg)
        *msg = m;
    return (u32_t)(now_ms() - t0);
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t* mbox, void** msg)
{
    struct sys_mbox* b = *mbox;
    void* m;
    pthread_mutex_lock(&b->m);
    if (!b->count) {
        pthread_mutex_unlock(&b->m);
        return SYS_MBOX_EMPTY;
    }
    m = take(b);
    pthread_mutex_unlock(&b->m);
    if (msg)
        *msg = m;
    return 0;
}

void sys_mbox_free(sys_mbox_t* mbox)
{
    struct sys_mbox* b = *mbox;
    if (!b)
        return;
    pthread_cond_destroy(&b->not_empty);
    pthread_cond_destroy(&b->not_full);
    pthread_mutex_destroy(&b->m);
    free(b->slot);
    free(b);
    *mbox = 0;
}

/* ---- threads ---- */

struct start {
    lwip_thread_fn fn;
    void* arg;
};

static void* run(void* p)
{
    struct start s = *(struct start*)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

sys_thread_t sys_thread_new(const char* name, lwip_thread_fn thread, void* arg, int stacksize, int prio)
{
    pthread_t t;
    pthread_attr_t a;
    struct start* s = (struct start*)malloc(sizeof(*s));
    (void)name;
    (void)prio;
    if (!s)
        abort();
    s->fn = thread;
    s->arg = arg;
    pthread_attr_init(&a);
    if (stacksize > 0)
        pthread_attr_setstacksize(&a, (size_t)stacksize);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, run, s))
        abort();
    pthread_attr_destroy(&a);
    return (sys_thread_t)t;
}

/* ---- one semaphore per thread ---- */

static pthread_key_t g_key;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void drop(void* p)
{
    sys_sem_t* slot = (sys_sem_t*)p;
    sys_sem_free(slot);
    free(slot);
}

static void make_key(void)
{
    pthread_key_create(&g_key, drop);
}

sys_sem_t* cp_thread_sem(void)
{
    sys_sem_t* slot;
    pthread_once(&g_once, make_key);
    slot = (sys_sem_t*)pthread_getspecific(g_key);
    if (!slot) {   /* freed by the key's destructor when the thread ends */
        slot = (sys_sem_t*)malloc(sizeof(*slot));
        if (!slot || sys_sem_new(slot, 0) != ERR_OK)
            abort();
        pthread_setspecific(g_key, slot);
    }
    return slot;
}

void cp_thread_sem_free(void)
{
}

/* ---- light protection (lwIP's own short critical sections) ---- */

static pthread_mutex_t g_protect;
static pthread_once_t g_protect_once = PTHREAD_ONCE_INIT;

static void make_protect(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_protect, &a);
    pthread_mutexattr_destroy(&a);
}

sys_prot_t sys_arch_protect(void)
{
    pthread_once(&g_protect_once, make_protect);
    pthread_mutex_lock(&g_protect);
    return 0;
}

void sys_arch_unprotect(sys_prot_t pval)
{
    (void)pval;
    pthread_mutex_unlock(&g_protect);
}
