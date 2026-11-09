#ifndef AA_VIDEO_H
#define AA_VIDEO_H
#include <stddef.h>
#include <stdint.h>
#include "aa_core.h"
enum { AV_CONFIG=1, AV_FRAME=2 };
typedef struct { unsigned char *data; size_t size; uint64_t pts; int kind; } av_packet;
int av_open(aa_log_fn log,unsigned width,unsigned height);
int av_push(int id,const unsigned char *data,size_t size);
int av_take(av_packet *packet,int wait);
void av_packet_free(av_packet *packet);
void av_close(void);
int av_failed(void);
void av_fail(const char *reason);
int av_native_start(aa_log_fn log,unsigned width,unsigned height);
int av_native_stop(void);
int av_native_want_idr(void);
#endif
