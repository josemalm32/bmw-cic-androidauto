/* Internal PCM diagnostic sink. Included after aa_core's wire helpers.
 * No hardware playback: bounded capture + explicit draining after its limit.
 * Channels 4 media (48k stereo), 5 speech and 6 system (16k mono), PCM S16LE.
 */
#define AA_AUDIO_LIMIT (1024UL * 1024)
/* v54 AUDIO1: on the car, PCM goes to shm rings (aa_aring.h) for cicaalive -> DSP.
 * No more aa_audio_*.pcm files (they filled /mnt/hbdebug). Host test keeps files. */
#ifndef CICAA_HOST_TEST
#include <sys/mman.h>
#include "aa_aring.h"
static aring_hdr *g_aring;
static aring_hdr *aring_get(void)
{
    int fd; void *m;
    if (g_aring) return g_aring;
    fd = shm_open(ARING_NAME, O_RDWR | O_CREAT, 0666);
    if (fd < 0) return 0;
    if (ftruncate(fd, (off_t)ARING_BYTES)) { close(fd); return 0; }
    m = mmap(0, ARING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); close(fd);
    if (m == MAP_FAILED) return 0;
    g_aring = (aring_hdr *)m;
    if (g_aring->magic != ARING_MAGIC || g_aring->chsize != ARING_CH) {
        memset(g_aring, 0, sizeof *g_aring); g_aring->chsize = ARING_CH; g_aring->magic = ARING_MAGIC;
    }
    return g_aring;
}
static void aring_put(int c, const unsigned char *b, size_t n)
{
    aring_hdr *h = g_aring; uint8_t *d; uint32_t w;
    if (!h) return;
    d = aring_data(h, c); w = h->wpos[c];
    if (n > ARING_CH) { b += n - ARING_CH; w += (uint32_t)(n - ARING_CH); n = ARING_CH; }
    while (n) {
        uint32_t o = w & (ARING_CH - 1), k = ARING_CH - o;
        if (k > n) k = (uint32_t)n;
        memcpy(d + o, b, k); b += k; n -= k; w += k;
    }
    __asm__ __volatile__("" ::: "memory");
    h->wpos[c] = w;
}
#endif
typedef struct {
    int opened, ready, active, fd;
    unsigned long session, bytes, frames, captured;
} aa_audio_state;
static aa_audio_state aa_audio[3];

static void audio_close(void)
{
    int i;
    for (i=0;i<3;++i) {
        if (aa_audio[i].opened && aa_audio[i].fd>=0) close(aa_audio[i].fd);
#ifndef CICAA_HOST_TEST
        if (g_aring) g_aring->active[i]=0;
#endif
        aa_audio[i].fd=-1; aa_audio[i].opened=aa_audio[i].ready=aa_audio[i].active=0;
    }
}
static void audio_reset(void)
{
    int i; memset(aa_audio,0,sizeof aa_audio);
    for(i=0;i<3;++i) aa_audio[i].fd=-1;
}
/* Decode u32 fields, skip unknown protobuf fields, reject truncated/overflow data.
 * Missing field is reported independently (proto3 config defaults to zero). */
static int audio_var(const unsigned char *b,size_t n,size_t *o,unsigned long *v)
{
    unsigned i; *v=0;
    for(i=0;i<5;++i) { unsigned c;
        if(*o>=n) return -1;
        c=b[(*o)++];
        if(i==4 && (c&0xf0)) return -1;
        *v|=(unsigned long)(c&127)<<(7*i);
        if(!(c&128)) return 0;
    } return -1;
}
static int audio_uint(const unsigned char *b,size_t n,unsigned f,unsigned long *v,int *present)
{
    size_t o=0; unsigned long k,x; *v=0;*present=0;
    while(o<n) {
        if(audio_var(b,n,&o,&k)||!(k>>3)) return -1;
        if((k&7)==0) {
            if(audio_var(b,n,&o,&x)) return -1;
            if((k>>3)==f) {*v=x;*present=1;}
        } else {
            if((k>>3)==f) return -1;
            if((k&7)==2) {if(audio_var(b,n,&o,&x)) return -1;}
            else if((k&7)==1) x=8;
            else if((k&7)==5) x=4;
            else return -1;
            if(x>n-o) return -1;
            o+=(size_t)x;
        }
    } return 0;
}
/* v17 BT1: media (ch4) is NOT announced by default, so the phone plays music
 * over its Bluetooth A2DP link to the car. Speech (ch5), system (ch6) and the
 * mic (ch7) stay: Samsung ends the session with "No audio playback service"
 * if no audio sink is announced. usb_media.on restores the ch4 announcement. */
/* v54: user wants ALL AA audio over USB -> DSP (BT route steals focus); calls stay on
 * HFP. Media ch4 is announced by default now; usb_media.off disables it again. */
static int g_usb_media;
/* v56 AAC1: media ch4 announced as AAC-LC (codec 2) so the phone encodes; raw AUs go
 * to the DSP M4AC path exactly like the working MP4 test video. aac.off -> PCM again. */
static int g_aac;
static void audio_discovery(pb_t *sd)
{
    int i;
    g_usb_media=1;   /* all AA audio (media, voice, system) goes over USB to the DSP */
    g_aac=1;         /* media as AAC-LC: decoded by the DSP (M4AC) */
    for(i=0;i<3;++i) {
        pb_t ch,sub,cfg;
        if(i==0 && !g_usb_media) continue;
        memset(&ch,0,sizeof ch); memset(&sub,0,sizeof sub);memset(&cfg,0,sizeof cfg);
        pb_u(&cfg,1,i==0?48000:16000);pb_u(&cfg,2,16);pb_u(&cfg,3,i==0?2:1);
        pb_u(&sub,1,(i==0&&g_aac)?2:1);pb_u(&sub,2,i==0?3:i==1?1:2);pb_msg(&sub,3,&cfg);
        pb_u(&sub,5,1);pb_u(&ch,1,4+i);pb_msg(&ch,3,&sub);pb_msg(sd,1,&ch);
    }
    {
        pb_t ch,sub,cfg;
        memset(&ch,0,sizeof ch);memset(&sub,0,sizeof sub);memset(&cfg,0,sizeof cfg);
        /* AVInputChannel is field 5, not an additional output/sink. */
        pb_u(&cfg,1,16000);pb_u(&cfg,2,16);pb_u(&cfg,3,1);
        pb_u(&sub,1,1);pb_msg(&sub,2,&cfg);pb_u(&sub,3,1);
        pb_u(&ch,1,7);pb_msg(&ch,5,&sub);pb_msg(sd,1,&ch);
    }
}
/* Discovery permits startup; native capture is still unavailable. Never send
 * fabricated samples or claim an open succeeded without a capture backend. */
static int microphone_dispatch(aa_t *a,int id,const unsigned char *b,size_t n)
{
    unsigned long open;int present;pb_t r;
    if(id!=0x8005) return 0;
    if(audio_uint(b,n,1,&open,&present) || open>1) return 1;
    if(!open) {a->lg("AA microphone CLOSE (no native capture)");return 0;}
    memset(&r,0,sizeof r);pb_u(&r,1,0);pb_u(&r,2,1);
    a->lg("AA microphone OPEN unavailable: native capture not implemented");
    return send_msg(a,7,0,0x8006,r.b,r.n)!=0;
}
static int audio_open(aa_t *a,int ch)
{
    aa_audio_state *s=&aa_audio[ch-4]; char path[100];
    if(s->opened) return 0;
#ifdef CICAA_HOST_TEST
    snprintf(path,sizeof path,"aa_audio_%d.pcm",ch);
    s->fd=open(path,O_WRONLY|O_CREAT|O_TRUNC|0x8000,0600);
    if(s->fd<0) {a->lg("AA audio capture open failed ch=%d",ch);return -1;}
#else
    (void)path;
    if(!aring_get()) {a->lg("AA audio ring shm failed ch=%d",ch);return -1;}
    s->fd=-1;
#endif
    s->opened=1;
    a->lg("AA audio ch=%d -> %s",ch,
#ifdef CICAA_HOST_TEST
          "PCM file sink"
#else
          "V54 shm ring /cicaa_audio (cicaalive -> DSP)"
#endif
          );
    return 0;
}
/* Returns 1 on transport/sink/protocol failure, to end session cleanly. */
static int audio_dispatch(aa_t *a,int ch,int id,const unsigned char *b,size_t n)
{
    aa_audio_state *s=&aa_audio[ch-4];pb_t r;unsigned long cfg,session;int has;
    memset(&r,0,sizeof r);
    if(id==AV_SETUP_REQ) {
        int ok,rc=audio_uint(b,n,1,&cfg,&has);
        { char hx[64];size_t i,m=n<16?n:16;hx[0]=0;for(i=0;i<m;i++) sprintf(hx+i*3,"%02x ",b[i]);
          a->lg("V57 audio setup ch=%d raw len=%u: %s(parse rc=%d cfg=%lu has=%d opened=%d)",ch,(unsigned)n,hx,rc,cfg,has,s->opened); }
        ok=s->opened!=0;  /* v57: accept any config_index, answer config 0 */
        s->ready=ok;s->active=0;
        pb_u(&r,1,ok?2:1);pb_u(&r,2,1);if(ok) pb_u(&r,3,0);
        a->lg("AA audio setup ch=%d accepted=%d",ch,ok);
        return send_msg(a,ch,0,AV_SETUP_RESP,r.b,r.n)!=0;
    }
    if(id==AV_START) {
        if(!s->ready || audio_uint(b,n,1,&session,&has) || !has || session>0x7fffffffUL ||
           audio_uint(b,n,2,&cfg,&has)) {
            a->lg("AA audio invalid start ch=%d",ch);return 1;
        }
        s->session=session;s->active=1;
#ifndef CICAA_HOST_TEST
        if(g_aring){if(ch==4) g_aring->codec0=g_aac?2:1; g_aring->gen[ch-4]++;g_aring->active[ch-4]=1;}
#endif
        a->lg("AA audio start ch=%d session=%lu",ch,session);return 0;
    }
    if(id==AV_STOP) {s->active=0;
#ifndef CICAA_HOST_TEST
        if(g_aring) g_aring->active[ch-4]=0;
#endif
       a->lg("AA audio stop ch=%d bytes=%lu",ch,s->bytes);return 0;}
    if(id==AV_MEDIA || id==AV_MEDIA_TS) {
        size_t off=id==AV_MEDIA_TS?8:0, count,done=0;
        if(!s->active) {a->lg("AA audio ignored inactive ch=%d",ch);return 0;}
        if(n<off || !(n-off) || ((ch!=4||!g_aac) && (n-off)%(ch==4?4:2)) || (ch==4&&g_aac&&n-off>65535)) {
            a->lg("AA audio malformed PCM ch=%d len=%u",ch,(unsigned)n);return 1;
        }
        count=n-off;
#ifdef CICAA_HOST_TEST
        if(count>AA_AUDIO_LIMIT-s->captured) count=AA_AUDIO_LIMIT-s->captured;
        while(done<count) {
            int w=write(s->fd,b+off+done,count-done);
            if(w<=0) {a->lg("AA audio capture write failed ch=%d",ch);return 1;}
            done+=(size_t)w;
        }
#else
        (void)done;
        if(ch==4 && g_aac) {
            unsigned char hl[2]; hl[0]=(unsigned char)count; hl[1]=(unsigned char)(count>>8);
            aring_put(0,hl,2); aring_put(0,b+off,count);
            if(!s->frames || s->frames==100) { const unsigned char *q=b+off;
                a->lg("V56 AAC ch4 frame#%lu %u bytes: %02x %02x %02x %02x %02x %02x %02x %02x",s->frames,(unsigned)count,
                      q[0],count>1?q[1]:0,count>2?q[2]:0,count>3?q[3]:0,count>4?q[4]:0,count>5?q[5]:0,count>6?q[6]:0,count>7?q[7]:0); }
        } else {
            aring_put(ch-4,b+off,count);
            if(!s->frames) a->lg("AA audio ch=%d first PCM %u bytes",ch,(unsigned)count);
        }
#endif
        s->captured+=count;s->bytes+=(unsigned long)(n-off);s->frames++;
        pb_u(&r,1,s->session);pb_u(&r,2,1);
        return send_msg(a,ch,0,AV_ACK,r.b,r.n)!=0;
    }
    a->lg("AA audio unhandled ch=%d id=0x%x",ch,id);return 0;
}
