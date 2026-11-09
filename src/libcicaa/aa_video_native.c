/* v47 LIVE1 native backend: NO decoding and NO MMF inside HmiMain.
 * A thread drains the V13 adapter queue (avcC + length4 frames) into a
 * shared-memory ring; the separate process cicaalive feeds the DM642.
 * Producer never blocks on the reader: drops frames when full / no reader. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include "aa_video.h"
#include "aa_ring.h"
#include <unistd.h>
static struct {
    pthread_t thread; int live; aa_log_fn log; ring_hdr *r; int need_idr;
    unsigned long logged_drop;
} n;
static uint64_t now_us(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;
}
static int frame_is_idr(const unsigned char *d,size_t len)
{
    size_t o=0;
    while(o+4<len) {
        uint32_t l=((uint32_t)d[o]<<24)|((uint32_t)d[o+1]<<16)|((uint32_t)d[o+2]<<8)|d[o+3];
        if(o+4+l>len || !l) break;
        if((d[o+4]&31)==5) return 1;
        o+=4+l;
    }
    return 0;
}
/* returns 0 written, 1 dropped. Records 4-aligned; size multiple of 4.
 * Empty when wpos==rpos, so writer always keeps a 4-byte gap. */
static int ring_put(ring_hdr *r,const av_packet *p)
{
    ring_rec h; uint32_t need=(uint32_t)((sizeof h+p->size+3)&~3u), w=r->wpos, rd=r->rpos;
    unsigned char *data=(unsigned char *)(r+1);
    if(!r->reader_pid) return 1;                 /* nobody listening */
    if(need>r->size/2) return 1;
    if(w>=rd) {
        if(need+(rd==0?4u:0u)<=r->size-w) ;      /* fits at end */
        else if(need+4<=rd) {                     /* wrap to start */
            uint32_t m=REC_WRAP;memcpy(data+w,&m,4);w=0;
        } else return 1;
    } else if(need+4>rd-w) return 1;
    memset(&h,0,sizeof h);h.kind=REC_FRAME;h.len=(uint32_t)p->size;h.idr=frame_is_idr(p->data,p->size);
    h.pts_us=now_us();
    memcpy(data+w,&h,sizeof h);memcpy(data+w+sizeof h,p->data,p->size);
    __asm__ __volatile__("" ::: "memory");
    r->wpos=(w+need)%r->size;
    return 0;
}
static void *worker(void *u)
{
    av_packet p;(void)u;
    while(av_take(&p,1)) {
        ring_hdr *r=n.r;
        if(p.kind==AV_CONFIG) {
            if(p.size<=RING_CFGMAX){memcpy(r->cfg,p.data,p.size);r->cfg_len=(uint32_t)p.size;__asm__ __volatile__("" ::: "memory");r->cfg_gen++;}
            n.log("V47 ring: avcC %u bytes gen=%u",(unsigned)p.size,(unsigned)r->cfg_gen);
            n.need_idr=1;
        } else if(p.kind==AV_FRAME) {
            int idr=frame_is_idr(p.data,p.size);
            r->frames_in++;
            if(n.need_idr && !idr) {r->frames_drop++;}
            else if(ring_put(r,&p)) {r->frames_drop++;n.need_idr=1;
                if(r->reader_pid && r->frames_drop-n.logged_drop>=30){n.logged_drop=r->frames_drop;n.log("V47 ring: drops=%u (reader slow)",(unsigned)r->frames_drop);}}
            else n.need_idr=0;
        }
        av_packet_free(&p);
    }
    return 0;
}
int av_native_start(aa_log_fn log,unsigned w,unsigned h)
{
    int fd; void *m;
    if(n.live) return -1;
    n.log=log;
    if(!n.r) {
        fd=shm_open(RING_NAME,O_RDWR|O_CREAT,0666);
        if(fd<0){log("V47 ring shm_open failed");return -1;}
        if(ftruncate(fd,(long)RING_BYTES)){log("V47 ring ftruncate failed");close(fd);return -1;}
        m=mmap(0,RING_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);close(fd);
        if(m==MAP_FAILED){log("V47 ring mmap failed");return -1;}
        n.r=m;
        if(n.r->magic!=RING_MAGIC){memset(n.r,0,sizeof *n.r);n.r->size=RING_DATA;n.r->magic=RING_MAGIC;}
    }
    n.r->width=w;n.r->height=h;n.r->cfg_len=0;n.need_idr=1;n.logged_drop=0;
    if(pthread_create(&n.thread,0,worker,0)) return -1;
    n.live=1;
    log("V47 LIVE1 ring %s %ux%u ready (reader pid=%u)",RING_NAME,w,h,(unsigned)n.r->reader_pid);
    return 0;
}
int av_native_stop(void)
{
    if(!n.live) return 0;
    if(pthread_join(n.thread,0)) return -1;   /* av_close() already woke av_take */
    n.live=0;
    if(n.r){n.r->cfg_len=0;n.r->cfg_gen++;}
    return 0;
}
/* aa_core asks: should we poke the phone for a new IDR (reader attached late)? */
int av_native_want_idr(void)
{
    if(!n.r || !n.r->want_idr) return 0;
    n.r->want_idr=0;return 1;
}
