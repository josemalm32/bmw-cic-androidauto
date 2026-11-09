/* V13: owned bounded AVC queue. Native writer pulls on its own thread. */
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifndef CICAA_HOST_TEST
int access(const char *,int); /* unistd.h clashes with static lock */
#define F_OK 0
#endif
#include "aa_video.h"
#define AV_SLOTS 8
#define AV_LIMIT 262144
#define AV_PARAM 4096
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready=PTHREAD_COND_INITIALIZER;
static av_packet queue[AV_SLOTS];
static unsigned head,count,width,height;
static int opened,error,need_idr,native_live;
/* V16: native DSP decode is opt-in. With it off, validated packets are
 * ACKed and dropped so the AA session stays up without touching the DSP. */
static int drop_mode;
static unsigned char sps[AV_PARAM],pps[AV_PARAM];
static size_t nsps,npps;
static aa_log_fn logger;
void av_packet_free(av_packet *p) { free(p->data); memset(p,0,sizeof *p); }
void av_fail(const char *why) {
 pthread_mutex_lock(&lock); error=1; pthread_cond_broadcast(&ready); pthread_mutex_unlock(&lock);
 if(logger) logger("V13 ADAPTER_ERROR %s",why);
}
static size_t startcode(const unsigned char *p,size_t n,size_t i,size_t *prefix) {
 for(;i+2<n;i++) if(p[i]==0 && p[i+1]==0) {
  if(p[i+2]==1) {*prefix=3;return i;}
  if(i+3<n && p[i+2]==0 && p[i+3]==1) {*prefix=4;return i;}
 }
 *prefix=0;return n;
}
int av_open(aa_log_fn log,unsigned w,unsigned h) {
 pthread_mutex_lock(&lock);
 if(opened || native_live || !w || !h || w>4096 || h>2160) {pthread_mutex_unlock(&lock);return -1;}
 logger=log; width=w; height=h; head=count=0; nsps=npps=0; error=0; need_idr=1; opened=1;
 pthread_mutex_unlock(&lock);
#ifndef CICAA_HOST_TEST
  if(av_native_start(log,width,height)) {av_fail("native start unavailable");av_close();return -1;}
  native_live=1;
#endif
 if(log)log("V13 adapter OPEN %ux%u: Annex-B -> avcC / length4, max queue=%u",w,h,AV_SLOTS);
 return 0;
}
int av_push(int id,const unsigned char *in,size_t n) {
 unsigned char tsps[AV_PARAM],tpps[AV_PARAM]; size_t sn,pn,off,prefix,pos,next,np,len,total=0;
 uint64_t pts=0; int changed=0,saw_params=0,vcl=0,idr=0,rc=-1; av_packet config={0},frame={0};
 if(!in || (id!=0 && id!=1) || n>AV_LIMIT) return -1;
 off=id==0?8:0;if(n<=off)return -1;
 if(id==0) {size_t j;for(j=0;j<8;j++)pts=(pts<<8)|in[j];}
 in+=off;n-=off;
 pthread_mutex_lock(&lock);
 if(!opened || error)goto done;
 sn=nsps;pn=npps;memcpy(tsps,sps,sn);memcpy(tpps,pps,pn);
 pos=startcode(in,n,0,&prefix);if(pos==n)goto done;
 {size_t j;for(j=0;j<pos;j++)if(in[j])goto done;}
 /* Validate whole message and stage parameter changes before touching queue. */
 while(pos<n) {
  size_t begin=pos+prefix;unsigned type;
  next=startcode(in,n,begin,&np);len=next-begin;
  while(len && in[begin+len-1]==0)len--;
  if(!len || (in[begin]&0x80))goto done;
  type=in[begin]&31;if(!type || type>23)goto done;
  if(type==7 || type==8) {
   saw_params=1;
   if(len>AV_PARAM || len<(type==7?4u:2u))goto done;
   if(type==7) {if(sn!=len || memcmp(tsps,in+begin,len))changed=1;memcpy(tsps,in+begin,len);sn=len;}
   else {if(pn!=len || memcmp(tpps,in+begin,len))changed=1;memcpy(tpps,in+begin,len);pn=len;}
  } else {
   if(len>AV_LIMIT-4 || total>AV_LIMIT-4-len)goto done;
   total+=4+len;
   if(type>=1 && type<=5)vcl=1;
   if(type==5)idr=1;
  }
  pos=next;prefix=np;
 }
 if(!vcl && (total || !saw_params))goto done; /* never ACK and discard a standalone SEI/AUD */
 if(vcl && (!sn || !pn || ((need_idr || changed) && !idr)))goto done;
 if(id==1 && vcl)goto done; /* untimestamped media cannot acquire an invented PTS */
 if(changed && sn && pn) {
  config.size=11+sn+pn;config.data=malloc(config.size);if(!config.data)goto done;config.kind=AV_CONFIG;
  config.data[0]=1;memcpy(config.data+1,tsps+1,3);config.data[4]=0xff;config.data[5]=0xe1;
  config.data[6]=(unsigned char)(sn>>8);config.data[7]=(unsigned char)sn;memcpy(config.data+8,tsps,sn);
  config.data[8+sn]=1;config.data[9+sn]=(unsigned char)(pn>>8);config.data[10+sn]=(unsigned char)pn;
  memcpy(config.data+11+sn,tpps,pn);
 }
 if(vcl) {
  size_t used=0;frame.size=total;frame.data=malloc(total);if(!frame.data)goto done;frame.kind=AV_FRAME;frame.pts=pts;
  pos=startcode(in,n,0,&prefix);
  while(pos<n) {
   size_t begin=pos+prefix;unsigned type=in[begin]&31;
   next=startcode(in,n,begin,&np);len=next-begin;while(len && in[begin+len-1]==0)len--;
   if(type!=7 && type!=8) {
    frame.data[used++]=(unsigned char)(len>>24);frame.data[used++]=(unsigned char)(len>>16);
    frame.data[used++]=(unsigned char)(len>>8);frame.data[used++]=(unsigned char)len;
    memcpy(frame.data+used,in+begin,len);used+=len;
   }
   pos=next;prefix=np;
  }
 }
 if(drop_mode) {
  memcpy(sps,tsps,sn);nsps=sn;memcpy(pps,tpps,pn);npps=pn;
  if(frame.data)need_idr=0;
  rc=0;goto done; /* frees config/frame */
 }
 if(count+(config.data?1:0)+(frame.data?1:0)>AV_SLOTS)goto done;
 memcpy(sps,tsps,sn);nsps=sn;memcpy(pps,tpps,pn);npps=pn;
 if(changed)need_idr=1;
 if(config.data) {queue[(head+count++)%AV_SLOTS]=config;config.data=0;}
 if(frame.data) {queue[(head+count++)%AV_SLOTS]=frame;frame.data=0;need_idr=0;}
 pthread_cond_broadcast(&ready);rc=0;
done:
 av_packet_free(&config);av_packet_free(&frame);
 pthread_mutex_unlock(&lock);return rc;
}
int av_take(av_packet *p,int wait) {
 memset(p,0,sizeof *p);pthread_mutex_lock(&lock);
 while(wait && opened && !error && !count)pthread_cond_wait(&ready,&lock);
 if(!opened || error || !count) {pthread_mutex_unlock(&lock);return 0;}
 *p=queue[head];memset(&queue[head],0,sizeof queue[head]);head=(head+1)%AV_SLOTS;count--;
 pthread_mutex_unlock(&lock);return 1;
}
int av_failed(void) {
 int failed;pthread_mutex_lock(&lock);failed=opened && error;pthread_mutex_unlock(&lock);return failed;
}
void av_close(void) {
 pthread_mutex_lock(&lock);opened=0;pthread_cond_broadcast(&ready);pthread_mutex_unlock(&lock);
#ifndef CICAA_HOST_TEST
 if(native_live) {if(av_native_stop()) {av_fail("native stop did not finish; resources retained");return;}native_live=0;}
#endif
 pthread_mutex_lock(&lock);
 while(count) {av_packet_free(&queue[head]);head=(head+1)%AV_SLOTS;count--;}
 nsps=npps=0;pthread_mutex_unlock(&lock);
}
