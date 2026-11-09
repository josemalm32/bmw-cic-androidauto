#ifndef CIC_IBA_STATE_H
#define CIC_IBA_STATE_H
/* v31: runtime mode. Plugin .so static constructors are NOT run on the CIC
 * (-init cic_loaded only): keep the flag zero-initialised (.bss), 0 = DVD/cicdsp
 * (v30 default), 1 = IBA AuxIn SelectVideoFile (firmware builds the graph). */
static int g_iba_file_mode;
/* 0=DSP MP4, 1=AuxIn IBA, 2=DMB in the isolated helper. */
#define IBA_DVD_MODE (g_iba_file_mode!=1)
#define IBA_DMB_MODE (g_iba_file_mode==2)
enum IbaOp { IBA_CHECK=0, IBA_AUDIO, IBA_FILE, IBA_CONNECT, IBA_START,
 IBA_SHOW, IBA_STOP, IBA_DISCONNECT, IBA_RELEASE, IBA_WAIT,
 IBA_PLAYING, IBA_DONE, IBA_FAILED };
/* Single HMI-thread owner. Separate flags retain late ACKs during teardown.
 * Expiry blocks progression; it never pretends a remote request was cancelled. */
struct IbaState {
 int ready, stopping, failed, deck_ready;
 int sent[9], result[9]; unsigned handles[9]; long issued_at[9];
 IbaState():ready(0),stopping(0),failed(0),deck_ready(0) {
  for(int i=0;i<9;++i){sent[i]=result[i]=0;handles[i]=0;issued_at[i]=0;}
 }
 int owns_resources() const {
  if(sent[IBA_CONNECT] && result[IBA_CONNECT]!=-1 && result[IBA_DISCONNECT]!=1)return 1;
  return sent[IBA_AUDIO] && result[IBA_AUDIO]!=-1 && result[IBA_RELEASE]!=1;
 }
 void checked(){ready=1;}
 int untouched() const {
  for(int i=1;i<9;++i)if(sent[i])return 0;
  return !stopping&&!failed;
 }
 int restart() {
  if(!ready||pending()||owns_resources())return 0;
  if(sent[IBA_START]&&result[IBA_STOP]!=1)return 0;
  *this=IbaState();ready=1;return 1;
 }
 int pending() const {
  for(int i=1;i<9;++i) if(sent[i]&&!result[i])return 1;
  return 0;
 }
 IbaOp next() const {
  if(!ready)return IBA_CHECK;
  if(stopping) {
   if(pending())return IBA_WAIT;
   if(sent[IBA_START]&&!sent[IBA_STOP])return IBA_STOP;
   if(sent[IBA_STOP]&&result[IBA_STOP]!=1)return IBA_FAILED;
   if(result[IBA_CONNECT]==1&&!sent[IBA_DISCONNECT])return IBA_DISCONNECT;
   if(sent[IBA_DISCONNECT]&&result[IBA_DISCONNECT]!=1)return IBA_FAILED;
   if(result[IBA_AUDIO]==1&&!sent[IBA_RELEASE])return IBA_RELEASE;
   if(sent[IBA_RELEASE]&&result[IBA_RELEASE]!=1)return IBA_FAILED;
   return IBA_DONE;
  }
  if(failed)return IBA_FAILED;
  if(pending())return IBA_WAIT;
  /* v26: DVD mode keeps audio C4 (DM642 audio to speakers) but skips IBA file/deck. */
  if(!sent[IBA_AUDIO])return IBA_AUDIO;
  if(!IBA_DVD_MODE) {
   if(!sent[IBA_FILE])return IBA_FILE;
   if(!deck_ready)return IBA_WAIT;
  }
  if(!sent[IBA_CONNECT])return IBA_CONNECT;
  if(!sent[IBA_START])return IBA_START;
  if(!sent[IBA_SHOW])return IBA_SHOW;
  return IBA_PLAYING;
 }
 int issue(IbaOp op,unsigned handle,long now) {
  if(op<IBA_AUDIO||op>IBA_RELEASE||next()!=op||sent[op])return 0;
  sent[op]=1;handles[op]=handle;issued_at[op]=now;return 1;
 }
 int ack(IbaOp op,unsigned handle,int ok) {
  if(op<IBA_AUDIO||op>IBA_RELEASE||!sent[op]||result[op]||handles[op]!=handle)return 0;
  result[op]=ok?1:-1;if(!ok)failed=1;return 1;
 }
 void expire(long now) {
  for(int i=1;i<9;++i)if(sent[i]&&!result[i]&&(unsigned long)(now-issued_at[i])>10000UL)failed=1;
 }
};
#endif
