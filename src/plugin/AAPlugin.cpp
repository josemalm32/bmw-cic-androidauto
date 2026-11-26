/* AAPlugin: Android Auto for the BMW CIC HIGH (QNX 6.3.2, SH4, HmiMain 9283417A).
 *
 * One HMI page: [Connect] [Disconnect] [Zoom Mode] [Switch Audio] [Logging] + one status
 * line "Screen: ...".
 *
 *  Connect  -> dlopen libcicaa.so (AA protocol over USB, TLS, channels) and start it.
 *           -> patched DM642 DSP image is linked in RAM (dspload on) once per boot.
 *           -> when the first phone video arrives: native HMI video route
 *              (audio C4, video source 8 -> sink 1, StartVideo, geometry, layer 26,
 *              menu layer alpha 0) and the cicaalive process feeds the DSP.
 *           -> our TerminalUI page (210) takes the iDrive: rotary/push/directions go
 *              to the phone. MENU leaves AA (it keeps running); pressing
 *              Connect again brings it back. The AA "exit" icon also leaves.
 *  Disconnect -> ends the AA session on the phone too (ByeByeRequest).
 *  Switch Audio -> BT Audio (default: phone music over Bluetooth, CIC source kept) or
 *              AA Audio (all AA audio over USB -> DSP); takes effect at the next Connect.
 *  Zoom Mode -> Normal / Fullscreen / Stretched (see screen_mode()).
 *  Keys     -> in AA Audio mode the steering-wheel and panel skip keys go to the phone
 *              as media next/previous (see key_hook()).
 *  GPS      -> the CIC position (RHMI data model vehicle.location) goes to the phone
 *              while AA runs (see onLocation()).
 *  Logging  -> creates/removes /mnt/hbdebug/aap/logs.on. Nothing is written
 *              anywhere unless it is ON; then logs go to /mnt/hbdebug/aap/logs/.
 *
 * Private HmiMain ABI (fixed addresses) is checked against code signatures before
 * any call; on mismatch nothing is touched. Addresses are written for the E9x build
 * and translated for other known builds (iba_profiles.h). Native HMI services are only used on
 * the HMI ProcUnit thread (marshalled through the HMI executor). */
namespace bmw {
class IDispatcher;
class Bundle;
class Parcel;
class Variant { public: unsigned int type() const; };
template <class K, class V> class Map {
public:
    void* getAssocAt(const K&, unsigned int&) const;
};
template <class T> class BasicString {
public:
    explicit BasicString(const T*);
    BasicString(const BasicString&);
    ~BasicString();
    const T* c_str() const;
    unsigned int length() const;
private:
    T* data;
};
/* Verified layout: operator() == stub(object, parcel). object != 0. */
template <class R, class A1, class A2, class A3, class A4> class Delegate {
public:
    void* object;
    R (*stub)(void*, A1);
};
class PluginApp {
public:
    void updateModel(unsigned, int);
    PluginApp(IDispatcher&, BasicString<char>, const Bundle& dependencies, const Bundle& parameters);
    void addHandler(unsigned, const Delegate<bool, const Parcel&, void, void, void>&);
    /* addHandlerWithMap is resolved at run time (map_handlers_init): the 2010 framework
       (E90 C13, B1009) does not have it. */
    void addHandler(unsigned, unsigned, const Delegate<bool, const Parcel&, void, void, void>&);
    void updateModel(unsigned, const char*);
    void triggerEvent(unsigned);
private:
    unsigned int reserved[128];   /* deliberately oversized (native layout >= 268 B) */
};
}

#include "aap_paths.h"

/* QNX 6.3 libc (no C++ runtime headers available for this target). */
extern "C" {
int open(const char*, int, ...);
int write(int, const void*, unsigned int);
int close(int);
int read(int, void*, unsigned int);
int unlink(const char*);
int mkdir(const char*, int);
int access(const char*, int);
struct timespec { long tv_sec; long tv_nsec; };
int clock_gettime(int, struct timespec*);
int nanosleep(const struct timespec*, struct timespec*);
unsigned int sleep(unsigned int);
int pthread_create(int*, const void*, void* (*)(void*), void*);
int pthread_detach(int);
int pthread_join(int, void**);
int pthread_self(void);
int pthread_mutex_init(void*, const void*);
int pthread_mutex_lock(void*);
int pthread_mutex_unlock(void*);
void* dlopen(const char*, int);
void* dlsym(void*, const char*);
int spawnl(int, const char*, const char*, ...);
int waitpid(int, int*, int);
int shm_open(const char*, int, ...);
void* mmap(void*, unsigned int, int, int, int, long);
int* __get_errno_ptr(void);
}
#define errno (*__get_errno_ptr())
#define ECHILD 10
#define EINTR 4
#define P_NOWAIT 1
#define WNOHANG 0x0040
#define O_WRONLY 0x0001
#define O_APPEND 0x0008
#define O_CREAT  0x0100
#define O_TRUNC  0x0200

typedef char pointer_must_be_32_bits[sizeof(void*) == 4 ? 1 : -1];
typedef bmw::Delegate<bool, const bmw::Parcel&, void, void, void> Handler;
typedef char delegate_must_be_two_words[sizeof(Handler) == 8 ? 1 : -1];

/* IDs must match AAPlugin.xml. 185 and 204 keep their numbers from the first release
 * but have new jobs (185 was Logs, 204 the toolbar Back button of the AA page). */
enum {
    ACTION_BACK_MAIN = 3,          /* hmiAction -> page 10 */
    ACTION_CONNECT = 181, ACTION_ZOOM = 185, ACTION_LOGS = 204, ACTION_DISCONNECT = 187,
    ACTION_AUDIO = 189,
    MODEL_STATUS = 182,            /* connection status text (kept, no label on the page) */
    MODEL_SCREEN = 186,            /* "Screen: <mode>[ | Logs ON]" line */
    PAGE_MAIN = 10, PAGE_MAIN_FIRST = 11,
    AA_SCREEN = 210, AA_KEYS = 211,
    EVENT_OPEN_AA = 202, EVENT_MAIN = 206
};

/* ---------------- small helpers (no printf in the plugin) ---------------- */
static unsigned int put_int(char* buf, unsigned int pos, int v) {
    char tmp[12]; unsigned int n = 0;
    unsigned int u = v < 0 ? (unsigned int)(-v) : (unsigned int)v;
    if (v < 0) buf[pos++] = '-';
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (n) buf[pos++] = tmp[--n];
    return pos;
}
static unsigned int put_str(char* buf, unsigned int pos, const char* s) {
    while (*s) buf[pos++] = *s++;
    return pos;
}
static unsigned int str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static void copy_str(char* to, const char* from, unsigned cap) {
    unsigned i = 0; for (; from && from[i] && i + 1 < cap; ++i) to[i] = from[i]; to[i] = 0;
}
static long mono_ms() {
    struct timespec t; clock_gettime(2 /* CLOCK_MONOTONIC */, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}
static int file_exists(const char* p) { return access(p, 0) == 0; }

/* ---------------- logging: only while logs.on exists ---------------- */
static volatile int g_logs = 0;
static int alog(const char* s) {
    if (!g_logs) return 1;
    int f = open(AAP_LOGDIR "/plugin.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (f < 0) return 1;
    unsigned n = 0; while (s[n]) ++n;
    write(f, s, n); write(f, "\n", 1); close(f);
    return 1;
}
static void alog_val(const char* tag, int value) {
    if (!g_logs) return;
    char b[112]; unsigned n = put_str(b, 0, tag); n = put_str(b, n, "="); n = put_int(b, n, value); b[n] = 0; alog(b);
}

/* ---------------- UI: serialise worker-thread updates ---------------- */
static bmw::PluginApp* g_app = 0;
static unsigned long g_ui_mtx[16];
static volatile int g_ui_ready = 0;
static int g_hmi_tid = -1;
struct UiGuard {
    int locked;
    UiGuard() : locked(0) { if (g_ui_ready && pthread_self() != g_hmi_tid) { pthread_mutex_lock(g_ui_mtx); locked = 1; } }
    ~UiGuard() { if (locked) pthread_mutex_unlock(g_ui_mtx); }
};
static inline void ui(bmw::PluginApp* a, unsigned m, const char* v) { UiGuard g; a->updateModel(m, v ? v : ""); }
static int spawn(void* (*fn)(void*)) {
    int tid = 0; int rc = pthread_create(&tid, 0, fn, 0);
    if (rc == 0) pthread_detach(tid);
    return rc;
}
static const char* snap(const char* src, char* buf, unsigned int cap) {
    unsigned int i = 0;
    if (src) for (; i + 1 < cap && src[i]; ++i) buf[i] = src[i];
    buf[i] = 0; return buf;
}
static void status_line(bmw::PluginApp* app);
static void add(bmw::PluginApp* app, unsigned id, bool (*stub)(void*, const bmw::Parcel&)) {
    Handler h; h.object = app; h.stub = stub; app->addHandler(id, h);
}

/* ---------------- libcicaa (AA protocol) ---------------- */
typedef int (*fn_int)(void);
typedef const char* (*fn_str)(void);
typedef int (*fn_input)(int, int, int);
typedef void (*fn_void)(void);
static fn_int g_aa_start = 0, g_aa_busy = 0, g_aa_proj = 0;
static fn_str g_aa_status = 0, g_aa_result = 0;
static fn_input g_aa_input = 0;
static fn_void g_aa_project = 0;
static volatile int g_aa_polling = 0;
static volatile int g_live_disp = 0;      /* AA video on screen */
static volatile int g_live_user = 0;      /* user wants AA on screen */
static volatile int g_focus_left = 0;     /* AA display was left by a focus loss (MENU, PDC...) */
static long g_live_disp_ms = 0;

/* ---------------- iDrive -> Android keycodes ---------------- */
/* TerminalUI (car-verified): field11 low16 = key, high16 = detents; field12 != 0 = down.
 * CIC keys: 8 push, 9 north, 10 south, 11 west, 12 east, 13/14 rotary, 2 back. */
static unsigned short g_keymap[64];
static void keymap_load() {
    for (int i = 0; i < 64; ++i) g_keymap[i] = 0;
    g_keymap[2] = 4;  g_keymap[8] = 23;                       /* BACK, DPAD_CENTER */
    g_keymap[9] = 19; g_keymap[10] = 20; g_keymap[11] = 21; g_keymap[12] = 22;   /* DPAD up/down/left/right */
    int f = open(AAP_KEYMAP, 0);
    if (f < 0) return;
    char b[512]; int n = read(f, b, sizeof b - 1); close(f);
    if (n <= 0) return;
    b[n] = 0;
    int vals[2], k = 0, v = 0, in = 0;
    for (int i = 0; i <= n; ++i) {
        char c = b[i];
        if (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); in = 1; continue; }
        if (in) { vals[k++] = v; v = 0; in = 0; }
        if (k == 2) { if (vals[0] < 64) g_keymap[vals[0]] = (unsigned short)vals[1]; k = 0; }
        if (c == '\n') k = 0;
    }
}
static void aa_forward_key(const unsigned* v, unsigned valid) {
    if (!g_live_disp || !g_aa_input || !(valid & (1u << 11))) return;
    unsigned key = v[11] & 0xffff, steps = v[11] >> 16; if (!steps) steps = 1;
    unsigned f12 = (valid & (1u << 12)) ? v[12] : 1;
    int down = f12 != 0;
    if (g_logs) {
        char b[80]; unsigned n = put_str(b, 0, "KEY "); n = put_int(b, n, (int)key);
        n = put_str(b, n, " steps="); n = put_int(b, n, (int)steps); n = put_str(b, n, " down="); n = put_int(b, n, down); b[n] = 0; alog(b);
    }
    if (key == 13) g_aa_input(CICAA_IN_REL, 65536, (int)steps);          /* rotary clockwise */
    else if (key == 14) g_aa_input(CICAA_IN_REL, 65536, -(int)steps);    /* rotary counter-clockwise */
    else if (key < 64 && g_keymap[key]) g_aa_input(CICAA_IN_KEY, g_keymap[key], down);
}
typedef bmw::Map<unsigned, bmw::Variant> KeyMap;
typedef bmw::Delegate<bool, const KeyMap&, void, void, void> KeyHandler;
static void key_dispatch(int focus, const unsigned* values, unsigned valid);
static void key_sample(int focus, const KeyMap* map) {
    unsigned values[16], valid = 0;
    for (unsigned i = 0; i < 16; ++i) values[i] = 0;
    /* const getAssocAt returns an existing node; Variant is node+12. Types 16/19 verified. */
    if (map && reinterpret_cast<const unsigned*>(map)[1] != 0) {
        for (unsigned i = 0; i < 16; ++i) {
            unsigned hash = 0;
            const unsigned char* node = static_cast<const unsigned char*>(map->getAssocAt(i, hash));
            if (!node) continue;
            const bmw::Variant* value = reinterpret_cast<const bmw::Variant*>(node + 12);
            const unsigned* container = *reinterpret_cast<const unsigned* const*>(value);
            if (!container) continue;
            unsigned type = value->type();
            if (type == 16 || type == 19) {
                values[i] = type == 19 ? reinterpret_cast<const unsigned char*>(container)[4] : container[1];
                if ((i == 11 && type == 16) || ((i == 12 || i == 13) && type == 19)) valid |= 1u << i;
            }
        }
    }
    key_dispatch(focus, values, valid);
}
static void key_dispatch(int focus, const unsigned* values, unsigned valid) {
    if (!focus) {
        /* First key on the (empty) AA page after a focus loss: bring the picture back
           instead of sending the key to the phone (fallback for the focus path below). */
        if (g_focus_left && !g_live_user && g_aa_polling) { g_live_user = 1; g_focus_left = 0; alog("key -> resume AA display"); return; }
        aa_forward_key(values, valid);
        return;
    }
    /* MENU / leaving our page while AA is shown -> back to the car UI (AA keeps running). */
    if (g_live_disp) {
        if ((unsigned long)(mono_ms() - g_live_disp_ms) > 2000UL) { g_live_user = 0; g_focus_left = 1; alog("focus lost -> leave AA display"); }
        return;
    }
    /* Focus came back to the AA page (end of PDC/reverse camera, BACK from the menu, an
       OPTION overlay closed...): resume the display by itself, the session never stopped. */
    if (g_focus_left && g_aa_polling) { g_focus_left = 0; g_live_user = 1; alog("focus back -> resume AA display"); }
}
static bool onKeys(void*, const KeyMap& map) { key_sample(0, &map); return false; }
static bool onFocus(void*, const KeyMap& map) { key_sample(1, &map); return false; }

/* ======================= native HMI video route ======================= */
#include "iba_state.h"
#include "iba_firmware.h"
#include "iba_marshal.h"
#include "iba_profiles.h"
/* HmiMain build profile (iba_profiles.h). 0 until iba_profile_select() found one;
 * until then iba_a() returns the E9x address unchanged. */
static const IbaProfile* g_iba_profile = 0;
static int g_iba_profile_done = 0;
static unsigned long iba_xlat(const IbaProfile* p, unsigned long a) {
    for (unsigned i = 0; p && i < p->nxlat; ++i) if (p->xlat[i].from == a) return p->xlat[i].to;
    return a;
}
static unsigned long iba_a(unsigned long a) { return iba_xlat(g_iba_profile, a); }
static int iba_sig_ok(const IbaProfile* p, unsigned long a, const unsigned char* bytes) {
    for (unsigned i = 0; i < p->nfix; ++i) if (p->fix[i].from == a) { bytes = p->fix[i].bytes; break; }
    const unsigned char* code = (const unsigned char*)iba_xlat(p, a);
    for (unsigned j = 0; j < 32; ++j) if (code[j] != bytes[j]) return 0;
    return 1;
}
/* First profile whose anchors and marshal fingerprints all match the running HmiMain. */
static const IbaProfile* iba_profile_select() {
    if (g_iba_profile_done) return g_iba_profile;
    g_iba_profile_done = 1;
    for (unsigned p = 0; p < IBA_N(kIbaProfiles); ++p) {
        const IbaProfile* pr = &kIbaProfiles[p];
        int bad = 0;
        for (unsigned i = 0; !bad && i < IBA_N(kIbaAnchors); ++i) bad = !iba_sig_ok(pr, kIbaAnchors[i].address, kIbaAnchors[i].bytes);
        for (unsigned i = 0; !bad && i < IBA_N(kIbaMarshalSigs); ++i) bad = !iba_sig_ok(pr, kIbaMarshalSigs[i].address, kIbaMarshalSigs[i].bytes);
        if (!bad) { g_iba_profile = pr; return pr; }
    }
    return 0;
}
static int auto_busy_control();
static void auto_tick(bmw::PluginApp*);
static int auto_is_shutdown();
static void auto_fail_control(const char*);
static const char* volatile g_iba_msg = "";
static void iba_ui(bmw::PluginApp*, const char* s) { g_iba_msg = s; alog(s); }
static IbaState g_iba;
static unsigned long g_iba_target[5];   /* CDelegateTarget: vptr + 16-byte list */
static int g_iba_target_live = 0, g_iba_checked = 0;
static int g_iba_tid = -1;
static int g_iba_sig_bad = 0;
static void *g_iba_cm = 0, *g_iba_aux = 0, *g_iba_vcm = 0, *g_iba_sink = 0;
static void* g_iba_mgr = 0;
static unsigned g_iba_sequence = 0;
static unsigned char g_iba_alpha = 0;
static int g_iba_layer = 0, g_iba_source_changed = 0, g_iba_previous_source = 0;
static int g_iba_suspended = 0;
static short g_iba_rect[2][4];
static int g_iba_restore_sent = 0, g_iba_restore_failed = 0;
static const char* const kIbaNames[] = {"check", "request audio C4", "select file",
 "connect video 8->1", "start video", "show layer", "stop video",
 "disconnect video", "release audio", "waiting for ACK", "video running", "finished", "failed"};
/* HmiMain LayerToolkit helper (enable layer 26), as the factory USB player uses it. */
static const unsigned long kLayerHelper = 0x08add9d4UL;
static void* iba_off(void* p, unsigned n) { return (char*)p + n; }
static int iba_before(const char* tag) {
    if (g_logs) { char b[112]; unsigned n = put_str(b, 0, "BEFORE "); n = put_str(b, n, tag); b[n] = 0; alog(b); }
    if (pthread_self() != g_iba_tid) { g_iba.failed = 1; return 0; }
    return 1;
}
static int iba_event(IbaOp op, unsigned h, int ok) {
    if (pthread_self() != g_iba_tid) { g_iba_suspended = 1; return 0; }
    int accepted = g_iba.ack(op, h, ok);
    if (accepted) { alog_val(kIbaNames[op], ok); }
    return accepted;
}
static void iba_audio_ack(void*, unsigned short h) { iba_event(IBA_AUDIO, h, 1); }
static void iba_audio_err(void*, unsigned short h, unsigned char) { iba_event(IBA_AUDIO, h, 0); }
static void iba_release_ack(void*, unsigned short h) { iba_event(IBA_RELEASE, h, 1); }
static void iba_release_err(void*, unsigned short h, unsigned char) { iba_event(IBA_RELEASE, h, 0); }
static void iba_file_ack(void*) {}
static void iba_file_err(void*, unsigned char) {}
static void iba_conn_ack(void*, unsigned short h, int src, int sink) { if (src == 8 && sink == 1) iba_event(IBA_CONNECT, h, 1); }
static void iba_conn_err(void*, unsigned short h, unsigned char) { iba_event(IBA_CONNECT, h, 0); }
static void iba_disconn_ack(void*, unsigned short h, int src, int sink) { if (src == 8 && sink == 1) iba_event(IBA_DISCONNECT, h, 1); }
static void iba_disconn_err(void*, unsigned short h, unsigned char) { iba_event(IBA_DISCONNECT, h, 0); }
static void iba_start_ack(void*, unsigned short h) { iba_event(IBA_START, h, 1); }
static void iba_start_err(void*, unsigned short h, unsigned char) { iba_event(IBA_START, h, 0); }
static void iba_stop_ack(void*, unsigned short h) { iba_event(IBA_STOP, h, 1); }
static void iba_stop_err(void*, unsigned short h, unsigned char) { iba_event(IBA_STOP, h, 0); }
static void iba_deck(void*, void*) {}
static int iba_signatures() {
    const IbaProfile* pr = iba_profile_select();
    if (!pr) {   /* report the first anchor that differs from the E9x build */
        for (unsigned i = 0; i < IBA_N(kIbaAnchors); ++i)
            if (!iba_sig_ok(&kIbaProfiles[0], kIbaAnchors[i].address, kIbaAnchors[i].bytes)) {
                g_iba_sig_bad = (int)kIbaAnchors[i].address; alog_val("SIGNATURE_MISMATCH", g_iba_sig_bad); break;
            }
        return 0;
    }
    if (g_logs) { char b[112]; unsigned n = put_str(b, 0, "HmiMain profile "); n = put_str(b, n, pr->name); b[n] = 0; alog(b); }
    return 1;
}
static int iba_list(void* proxy, unsigned offset, void* cb, int property = 0) {
    typedef int (*AddListener)(void*, void*, void*, unsigned);
    if (!iba_before("register listener")) return 0;
    return ((AddListener)(property ? iba_a(0x082c2268UL) : iba_a(0x082c2148UL)))(iba_off(proxy, offset), g_iba_target, cb, 0);
}
static int iba_check(bmw::PluginApp* app) {
    if (g_iba_checked) return g_iba.ready;
    if (g_iba_tid == -1) g_iba_tid = pthread_self();
    g_iba_checked = 1;
    if (!iba_before("firmware check")) { iba_ui(app, "Wrong HMI thread"); return 0; }
    if (!iba_signatures()) { iba_ui(app, "Different HmiMain: not compatible"); return 0; }
    typedef void* (*Factory)(unsigned);
    typedef void (*Ctor)(void*);
    ((Ctor)iba_a(0x08201c58UL))(g_iba_target); g_iba_target_live = 1;
    g_iba_cm = ((Factory)iba_a(0x08b9d1b0UL))(0);
    g_iba_aux = ((Factory)iba_a(0x08b64d18UL))(38);
    g_iba_vcm = ((Factory)iba_a(0x08ce0fc4UL))(1);
    g_iba_sink = ((Factory)iba_a(0x08ce328cUL))(1);
    if (!(g_iba_cm && g_iba_aux && g_iba_vcm && g_iba_sink)) { iba_ui(app, "CIC video services missing"); return 0; }
    if (*(unsigned long*)iba_off(g_iba_cm, 0x6c8) != iba_a(0x0920cbbcUL) ||
        *(unsigned long*)iba_off(g_iba_vcm, 0x6c8) != iba_a(0x092276c0UL) ||
        *(unsigned long*)iba_off(g_iba_vcm, 0x760) != iba_a(0x0922767cUL) ||
        *(unsigned long*)iba_off(g_iba_aux, 0x1de4) != iba_a(kIbaSelectVtable) ||
        *(unsigned long*)iba_off(g_iba_sink, 0xa48) != iba_a(kIbaStartVtable)) { iba_ui(app, "Unexpected service layout"); return 0; }
#define IBA_LIST(p, o, f) if (!iba_list(p, o, (void*)&f)) { iba_ui(app, "Could not register listener"); return 0; }
    IBA_LIST(g_iba_cm, 0x754, iba_audio_ack)
    IBA_LIST(g_iba_cm, 0x6f4, iba_audio_err)
    IBA_LIST(g_iba_cm, 0x7ec, iba_release_ack)
    IBA_LIST(g_iba_cm, 0x78c, iba_release_err)
    IBA_LIST(g_iba_aux, 0x1e70, iba_file_ack)
    IBA_LIST(g_iba_aux, 0x1e04, iba_file_err)
    if (!iba_list(g_iba_aux, 0xde0, (void*)&iba_deck, 1) ||
        !iba_list(g_iba_sink, 0x748, (void*)&iba_deck, 1) ||
        !iba_list(g_iba_sink, 0x7c8, (void*)&iba_deck, 1) ||
        !iba_list(g_iba_sink, 0xc80, (void*)&iba_deck, 1) ||
        !iba_list(g_iba_sink, 0xb78, (void*)&iba_deck, 1) ||
        !iba_list(g_iba_sink, 0xbfc, (void*)&iba_deck, 1) ||
        !iba_list(g_iba_vcm, 0x7f8, (void*)&iba_deck, 1)) { iba_ui(app, "Property subscription failed"); return 0; }
    IBA_LIST(g_iba_vcm, 0x754, iba_conn_ack)
    IBA_LIST(g_iba_vcm, 0x6f4, iba_conn_err)
    IBA_LIST(g_iba_vcm, 0x7ec, iba_disconn_ack)
    IBA_LIST(g_iba_vcm, 0x78c, iba_disconn_err)
    IBA_LIST(g_iba_sink, 0xad4, iba_start_ack)
    IBA_LIST(g_iba_sink, 0xa74, iba_start_err)
    IBA_LIST(g_iba_sink, 0xb6c, iba_stop_ack)
    IBA_LIST(g_iba_sink, 0xb0c, iba_stop_err)
#undef IBA_LIST
    g_iba.checked(); alog("CHECK_OK"); return 1;
}
static unsigned iba_handle() {
    typedef unsigned short (*Role)();
    unsigned h = ((((Role)iba_a(0x08d6ebbcUL))() & 0xffffU) << 4) | 0xf000U;
    return (h + (g_iba_sequence++ & 15U)) & 0xffffU;
}
static int iba_idle_sink() {
    typedef int (*Get)(void*, int*); int source = -1;
    if (!iba_before("read current video source")) return 0;
    typedef int (*ValidSource)(void*);
    if (!((ValidSource)iba_a(0x08d1ae64UL))(iba_off(g_iba_sink, 0xc80))) return 0;
    int rc = ((Get)iba_a(0x08ce4890UL))(iba_off(g_iba_sink, 0xc80), &source);
    alog_val("current source", source);
    int own = g_iba.result[IBA_CONNECT] == 1;
    if (!rc || (source != 0 && !(own && source == 8))) { iba_ui(g_app, "Video screen busy with another source"); return 0; }
    typedef int (*Valid)(void*);
    typedef int (*Table)(void*, int, int*);
    void* table = iba_off(g_iba_vcm, 0x7f8);
    if (!((Valid)iba_a(0x08d1ae64UL))(table)) return 0;
    int connected = -1;
    if (!((Table)iba_a(0x08ce160cUL))(table, 1, &connected) || connected != (own ? 8 : 0)) {
        /* car: connect ACK but table stays 0 until video starts; only that case continues */
        if (!(own && connected == 0 && source == 0)) { iba_ui(g_app, "Unexpected video table"); return 0; }
    }
    if (!own) g_iba_previous_source = source;
    return 1;
}
static int iba_rect_valid(unsigned offset) {
    typedef int (*Valid)(void*);
    return ((Valid)iba_a(0x08d1ae64UL))(iba_off(g_iba_sink, offset)) != 0;
}
static int iba_snapshot_rect() {
    if (!iba_before("snapshot rectangles")) return 0;
    if (!iba_rect_valid(0xb78) || !iba_rect_valid(0xbfc)) return 0;
    for (unsigned i = 0; i < 2; ++i) for (unsigned j = 0; j < 4; ++j)
        g_iba_rect[i][j] = ((short*)iba_off(g_iba_sink, (i ? 0xbfc : 0xb78) + 124))[j];
    return 1;
}
static int g_dsp_pid = 0;
static int g_dsp_attempted = 0;
/* Screen mode (Zoom Mode button), stored as files so it survives a CIC restart:
 *   none     -> 0 Normal:     picture centred (x 256..1216), CIC menu/arcs stay visible
 *   zoom.on  -> 1 Fullscreen: same size from x = 0, CIC menu layer hidden, rest black
 *   wide.on  -> 2 Stretched:  stretched to the full 1280 px width, CIC menu hidden
 * Built-in geometry (geom.txt is gone: the layout must match what libcicaa asks the
 * phone for, so a free text file only caused broken pictures). */
static int screen_mode() {
    if (file_exists(AAP_WIDE_ON)) return 2;
    return file_exists(AAP_ZOOM_ON) ? 1 : 0;
}
static void screen_mode_next() {
    int m = (screen_mode() + 1) % 3;
    unlink(AAP_ZOOM_ON); unlink(AAP_WIDE_ON);
    if (m) { int f = open(m == 1 ? AAP_ZOOM_ON : AAP_WIDE_ON, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (f >= 0) close(f); }
}
static void geom_load(short* g) {
    static const short def[3][8] = {AAP_GEOM_NORMAL, AAP_GEOM_FULLSCREEN, AAP_GEOM_STRETCHED};
    int m = screen_mode();
    for (int i = 0; i < 8; ++i) g[i] = def[m][i];
    /* geom.txt (optional, e.g. with video.txt on an E60) replaces the built-in geometry */
    int f = open(AAP_GEOM_TXT, 0);
    if (f < 0) return;
    char b[80]; int n = read(f, b, sizeof b - 1); close(f);
    if (n <= 0) return;
    b[n] = 0; int k = 0, v = 0, neg = 0, in = 0; short t[8];
    for (int i = 0; i <= n && k < 8; ++i) {
        char c = b[i];
        if (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); in = 1; }
        else if (c == '-' && !in) neg = 1;
        else if (in) { t[k++] = (short)(neg ? -v : v); v = 0; neg = 0; in = 0; }
    }
    if (k == 8) for (int i = 0; i < 8; ++i) g[i] = t[i];
}
static int iba_apply_geometry(bmw::PluginApp*) {
    /* The BMW DVD service never publishes norm/format here (we feed the DSP ourselves):
     * native PAL geometry is the fallback if the scaler refuses the AA rectangle. */
    typedef void* (*Factory)(); typedef int (*Geometry)(void*, short, short, short, short*);
    if (!iba_before("geometry")) return 0;
    void* defs = ((Factory)iba_a(0x08938a00UL))(); if (!defs) return 0;
    short native[2][4];
    int ok = ((Geometry)iba_a(0x08938b60UL))(defs, 1, 0, 0, native[0]) && ((Geometry)iba_a(0x08938b60UL))(defs, 2, 0, 0, native[1]);
    short g[8]; geom_load(g);
    typedef int (*Rect)(void*, short, short, short, short);
    int okin = ((Rect)iba_a(0x08ce4408UL))(iba_off(g_iba_sink, 0xb78), g[0], g[1], g[2], g[3]);
    int okout = okin && ((Rect)iba_a(0x08ce464cUL))(iba_off(g_iba_sink, 0xbfc), g[4], g[5], g[6], g[7]);
    if (!okout) {
        alog("AA geometry refused -> native DVD window");
        if (!ok) return 0;
        if (!((Rect)iba_a(0x08ce4408UL))(iba_off(g_iba_sink, 0xb78), native[0][0], native[0][1], native[0][2], native[0][3])) return 0;
        if (!((Rect)iba_a(0x08ce464cUL))(iba_off(g_iba_sink, 0xbfc), native[1][0], native[1][1], native[1][2], native[1][3])) return 0;
    }
    return 1;
}
static int iba_route_ours(bmw::PluginApp* app) {
    typedef int (*Valid)(void*); typedef int (*Get)(void*, int*); typedef int (*Table)(void*, int, int*);
    void* src = iba_off(g_iba_sink, 0xc80); void* table = iba_off(g_iba_vcm, 0x7f8);
    if (!((Valid)iba_a(0x08d1ae64UL))(src) || !((Valid)iba_a(0x08d1ae64UL))(table)) { iba_ui(app, "Waiting for video route"); return 0; }
    int current = -1, connection = -1;
    if (!((Get)iba_a(0x08ce4890UL))(src, &current) || !((Table)iba_a(0x08ce160cUL))(table, 1, &connection)) return 0;
    int own_table_lag = connection == 0 && g_iba.result[IBA_CONNECT] == 1;
    if ((current != 0 && current != 8) || (connection != 8 && !own_table_lag)) { iba_ui(app, "Video route changed by another client"); return 0; }
    return 1;
}
static int iba_restore_sink(bmw::PluginApp* app) {
    if (!g_iba_source_changed) return 1;
    if (g_iba_restore_failed) { iba_ui(app, "Video restore failed; restart the CIC"); return 0; }
    typedef int (*SetSource)(void*, int, unsigned char);
    typedef int (*Rect)(void*, short, short, short, short);
    if (!g_iba_restore_sent) {
        if (!iba_route_ours(app)) return 0;
        if (!iba_before("restore sink")) return 0;
        g_iba_restore_sent = 1;
        for (unsigned i = 0; i < 2; ++i) {
            short* r = g_iba_rect[i];
            if (!((Rect)(i ? iba_a(0x08ce464cUL) : iba_a(0x08ce4408UL)))(iba_off(g_iba_sink, i ? 0xbfc : 0xb78), r[0], r[1], r[2], r[3])) { g_iba_restore_failed = 1; return 0; }
        }
        if (!((SetSource)iba_a(0x08ce48e0UL))(iba_off(g_iba_sink, 0xc80), g_iba_previous_source, 0)) { g_iba_restore_failed = 1; return 0; }
    }
    typedef int (*Get)(void*, int*); int source = -1;
    int rc = ((Get)iba_a(0x08ce4890UL))(iba_off(g_iba_sink, 0xc80), &source);
    if (!rc || source != g_iba_previous_source || !iba_rect_valid(0xb78) || !iba_rect_valid(0xbfc)) return 0;
    for (unsigned i = 0; i < 2; ++i) for (unsigned j = 0; j < 4; ++j)
        if (g_iba_rect[i][j] != ((short*)iba_off(g_iba_sink, (i ? 0xbfc : 0xb78) + 124))[j]) return 0;
    g_iba_source_changed = 0; alog("sink restored"); return 1;
}
/* cicaalive feeds the DM642 from its own process (never decoded in HmiMain). */
static int live_spawn() {
    unlink(AAP_STOP);
    if (!iba_before("spawn cicaalive")) return 0;
    g_dsp_pid = spawnl(P_NOWAIT, AAP_LIVE, AAP_LIVE, "live", g_logs ? AAP_LOGDIR "/cicaalive.log" : "", (char*)0);
    alog_val("cicaalive pid", g_dsp_pid);
    if (g_dsp_pid <= 0) { g_dsp_pid = 0; return 0; }
    return 1;
}
static int live_prepare(bmw::PluginApp* app) {
    if (!g_dsp_attempted) {
        g_dsp_attempted = 1;
        if (!live_spawn()) { g_iba.failed = 1; iba_ui(app, "Could not start cicaalive"); return 0; }
        return 0;   /* next tick shows the layer */
    }
    if (!g_dsp_pid) return 0;
    int status = 0; int r = waitpid(g_dsp_pid, &status, WNOHANG);
    int error = r < 0 ? errno : 0;
    if (r == 0) return 1;
    if (r < 0 && error != ECHILD) { if (error != EINTR) g_iba.failed = 1; return 0; }
    g_dsp_pid = 0; g_iba.failed = 1; iba_ui(app, "cicaalive ended before showing video"); return 0;
}
static int live_reap() {
    if (!g_dsp_pid) return 1;
    int f = open(AAP_STOP, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (f >= 0) close(f);
    int st = 0; int r = waitpid(g_dsp_pid, &st, WNOHANG);
    int error = r < 0 ? errno : 0;
    if (r < 0 && error != ECHILD) return 0;
    if (r == g_dsp_pid || r < 0) { g_dsp_pid = 0; alog_val("cicaalive exit status", st); return 1; }
    return 0;
}
/* The HMI menu (cid 256) sits above the video layer (cid 28): give the logical layer
 * whose cid is 256 alpha 0 while AA is shown (HMI then composes cid 28 only). */
typedef int (*LmgrShow)(int*, int, int);
static LmgrShow g_lmgr_show = 0;
static int g_menu_hidden = 0, g_menu_idx = -1, g_menu_hold_on = 0, g_menu_reasserts = 0;
static unsigned char g_menu_saved = 255;
static long g_menu_last = 0;
static int text_ptr(unsigned long v) { return v >= 0x08000000UL && v < 0x09000000UL; }
static LmgrShow lmgr_show_fn() {
    if (g_lmgr_show) return g_lmgr_show;
    void* h = dlopen("libproject_qnxapp_collect_srvlayermanager_client.so", 2);
    if (!h) h = dlopen("/usr/lib/libproject_qnxapp_collect_srvlayermanager_client.so", 2);
    if (h) g_lmgr_show = (LmgrShow)dlsym(h, "lmgrShowDisplayables");
    return g_lmgr_show;
}
static int menu_find_layer() {
    if (!g_iba_mgr) return -1;
    for (int i = 0; i < 28; i++) {
        unsigned char* e = (unsigned char*)g_iba_mgr + 16 * i;
        void* obj = *(void**)(e + 12); int cid = -1;
        if (obj) {
            unsigned long* vt = *(unsigned long**)obj;
            if (text_ptr((unsigned long)vt) || ((unsigned long)vt >= 0x09000000UL && (unsigned long)vt < 0x0a000000UL))
                if (text_ptr(vt[17])) { typedef int (*Cid)(void*); cid = ((Cid)vt[17])(obj); }
        }
        if (cid == 256) return i;
    }
    return -1;
}
static void menu_hide_for_video() {
    g_menu_hidden = 1; g_menu_idx = -1; g_menu_hold_on = 0;
    if (g_iba_mgr) {
        int i = menu_find_layer();
        unsigned long* vt = *(unsigned long**)g_iba_mgr;
        if (i >= 0 && vt[14] == iba_a(0x08af0210UL) && vt[15] == iba_a(0x08af0278UL)) {
            typedef int (*GetAlpha)(void*, unsigned, unsigned char*);
            typedef void (*SetAlpha)(void*, unsigned, unsigned);
            unsigned char a = 255; if (((GetAlpha)vt[15])(g_iba_mgr, (unsigned)i, &a)) g_menu_saved = a;
            ((SetAlpha)vt[14])(g_iba_mgr, (unsigned)i, 0); g_menu_idx = i;
        }
    }
    LmgrShow f = lmgr_show_fn();
    if (f) { int cids[1] = {28}; f(cids, 1, 0); }
    if (g_menu_idx < 0) g_menu_hold_on = 1;   /* fallback: re-assert every 500 ms */
}
static void menu_hold(long now) {
    if (!g_menu_hidden || !g_menu_hold_on || !g_lmgr_show || now - g_menu_last < 500) return;
    g_menu_last = now; int cids[1] = {28}; g_lmgr_show(cids, 1, 0); g_menu_reasserts++;
}
static void menu_restore() {
    if (!g_menu_hidden) return;
    g_menu_hidden = 0;
    if (g_menu_idx >= 0 && g_iba_mgr) {
        unsigned long* vt = *(unsigned long**)g_iba_mgr;
        typedef void (*SetAlpha)(void*, unsigned, unsigned);
        ((SetAlpha)vt[14])(g_iba_mgr, (unsigned)g_menu_idx, g_menu_saved); g_menu_idx = -1;
    }
    LmgrShow f = lmgr_show_fn();
    if (f && g_menu_hold_on) { int cids[1] = {256}; f(cids, 1, 0); }
    g_menu_reasserts = 0; g_menu_last = 0; g_menu_hold_on = 0;
}
/* Geometry hold + fade-in (HMI thread, every auto_tick ~250 ms while the layer is on).
 * Car finding: right after StartVideo the DVD service sets the sink rectangles back to
 * its own PAL window, sometimes several times during the first second ("full screen for
 * a moment, then the small PAL window"). So the rectangles are compared with what we saw
 * last time and our geometry is applied again whenever someone changed them. The layer
 * is made visible after 4 ticks without a correction, at the latest after 12 ticks. */
static short g_geo_seen[2][4];
static int g_geo_stable = 0, g_geo_ticks = 0, g_geo_visible = 0;
static void geo_read(short r[2][4]) {
    for (unsigned i = 0; i < 2; ++i) for (unsigned j = 0; j < 4; ++j)
        r[i][j] = ((short*)iba_off(g_iba_sink, (i ? 0xbfc : 0xb78) + 124))[j];
}
static void geometry_hold(bmw::PluginApp* app) {
    if (!g_iba_layer || !g_iba_sink || !g_iba_mgr) {
        for (unsigned i = 0; i < 2; ++i) for (unsigned j = 0; j < 4; ++j) g_geo_seen[i][j] = 0;
        g_geo_stable = g_geo_ticks = g_geo_visible = 0;
        return;
    }
    short cur[2][4]; geo_read(cur);
    int changed = 0;
    for (unsigned i = 0; i < 2; ++i) for (unsigned j = 0; j < 4; ++j) if (cur[i][j] != g_geo_seen[i][j]) changed = 1;
    if (changed) { g_geo_stable = 0; iba_apply_geometry(app); geo_read(g_geo_seen); }
    ++g_geo_ticks; ++g_geo_stable;
    if (!g_geo_visible && (g_geo_stable >= 4 || g_geo_ticks >= 12)) {
        unsigned long* vt = *(unsigned long**)g_iba_mgr;
        typedef void (*SetAlpha)(void*, unsigned, unsigned);
        ((SetAlpha)vt[14])(g_iba_mgr, 26, 255);
        g_geo_visible = 1; alog("video layer visible");
    }
}
/* One step of the native flow (HMI thread only). */
static void iba_do(bmw::PluginApp* app, int stopping) {
    if (pthread_self() != g_iba_tid) { iba_ui(app, "Action outside the HMI thread"); return; }
    if (!g_iba.ready) { if (!stopping) iba_check(app); return; }
    if (g_iba_suspended) { iba_ui(app, "Callback outside the HMI thread"); return; }
    g_iba.expire(mono_ms());
    if (stopping) g_iba.stopping = 1;
    IbaOp op = g_iba.next();
    if (g_iba.stopping && (op == IBA_DISCONNECT || op == IBA_RELEASE || op == IBA_DONE) && !iba_restore_sink(app)) return;
    if (op >= IBA_WAIT) return;
    if (op == IBA_AUDIO) {
        if (!file_exists(AAP_LIVE)) { iba_ui(app, "cicaalive missing (reinstall)"); return; }
        if (!iba_idle_sink()) return;
        if (!iba_snapshot_rect()) return;
    }
    if (op == IBA_SHOW) {
        if (*(unsigned long*)iba_a(0x091fda28UL) != 0) { g_iba.failed = 1; iba_ui(app, "Video layer busy with another client"); return; }
        if (!iba_route_ours(app)) return;
        if (!live_prepare(app)) return;
        if (!iba_apply_geometry(app)) return;
        if (!iba_before("show layer26")) return;
        typedef void* (*Draw)(); void* draw = ((Draw)iba_a(0x08ae34e4UL))();
        if (!draw) { g_iba.failed = 1; return; }
        unsigned long* vt = *(unsigned long**)draw;
        if (vt[20] != iba_a(kIbaLayerManagerGetter)) { g_iba.failed = 1; return; }
        typedef void* (*Manager)(void*); g_iba_mgr = ((Manager)vt[20])(draw);
        if (!g_iba_mgr) { g_iba.failed = 1; return; }
        vt = *(unsigned long**)g_iba_mgr;
        if (vt[14] != iba_a(0x08af0210UL) || vt[15] != iba_a(0x08af0278UL)) { g_iba.failed = 1; return; }
        typedef int (*GetAlpha)(void*, unsigned, unsigned char*);
        if (!((GetAlpha)vt[15])(g_iba_mgr, 26, &g_iba_alpha)) { g_iba.failed = 1; return; }
        g_iba.issue(op, 0, mono_ms());
        typedef void (*SetAlpha)(void*, unsigned, unsigned);
        /* Layer 26 starts transparent; geometry_hold() makes it visible once the CIC has
           stopped changing the rectangles (no flash of a wrong-sized picture). */
        ((SetAlpha)vt[14])(g_iba_mgr, 26, 0);
        typedef void (*Enable)(int); ((Enable)iba_a(kLayerHelper))(1); g_iba_layer = 1;
        g_iba.ack(op, 0, 1);
        if (screen_mode() != 0) menu_hide_for_video();   /* Normal keeps the CIC menu visible */
        return;
    }
    if (g_iba.stopping && !live_reap()) return;
    if (op == IBA_STOP && !iba_route_ours(app)) return;
    if (op == IBA_STOP && g_iba_layer) {
        if (!iba_before("hide layer26")) return;
        menu_restore();
        typedef void (*Enable)(int); ((Enable)iba_a(kLayerHelper))(0); g_iba_layer = 0;
        unsigned long* vt = *(unsigned long**)g_iba_mgr;
        typedef void (*SetAlpha)(void*, unsigned, unsigned);
        ((SetAlpha)vt[14])(g_iba_mgr, 26, g_iba_alpha);
    }
    if (op == IBA_START && (!iba_idle_sink() || !iba_snapshot_rect())) return;
    if (!iba_before(kIbaNames[op])) return;
    unsigned h = iba_handle();
    if (!g_iba.issue(op, h, mono_ms())) return;
    int rc = 0;
    typedef int (*Connection)(void*, unsigned short, unsigned short);
    typedef int (*VideoConnection)(void*, unsigned short, int, int);
    typedef int (*SinkRun)(void*, unsigned short);
    if (op == IBA_AUDIO) rc = ((Connection)iba_a(0x08b9d308UL))(iba_off(g_iba_cm, 0x6c8), (unsigned short)h, 0xc4);
    else if (op == IBA_RELEASE) rc = ((Connection)iba_a(0x08b9d460UL))(iba_off(g_iba_cm, 0x760), (unsigned short)h, 0xc4);
    else if (op == IBA_CONNECT) rc = ((VideoConnection)iba_a(0x08ce111cUL))(iba_off(g_iba_vcm, 0x6c8), (unsigned short)h, 8, 1);
    else if (op == IBA_DISCONNECT) rc = ((VideoConnection)iba_a(0x08ce135cUL))(iba_off(g_iba_vcm, 0x760), (unsigned short)h, 8, 1);
    else if (op == IBA_START) {
        typedef int (*SetSource)(void*, int, unsigned char);
        g_iba_source_changed = 1;
        rc = ((SetSource)iba_a(0x08ce48e0UL))(iba_off(g_iba_sink, 0xc80), 8, 0);
        if (rc) rc = ((SinkRun)iba_a(0x08ce40e4UL))(iba_off(g_iba_sink, 0xa48), (unsigned short)h);
    } else if (op == IBA_STOP) rc = ((SinkRun)iba_a(0x08ce42c0UL))(iba_off(g_iba_sink, 0xae0), (unsigned short)h);
    alog_val(kIbaNames[op], rc);
    if (!rc) g_iba.ack(op, h, 0);
}
static void iba_restart() {
    if (g_dsp_pid || g_iba_layer || g_iba_source_changed || g_iba_restore_failed || g_iba_suspended) return;
    if (!g_iba.restart()) return;
    g_iba_restore_sent = 0; g_iba_previous_source = 0; g_dsp_attempted = 0;
}

/* HmiMain services are per thread: every native step is queued to the HMI ProcUnit
 * of the thread that ran plugin create, through the factory executor (as IBA does). */
static void* g_iba_pu = 0;
static int g_iba_marshal_ok = 0;
static volatile int g_iba_pending = 0;
static unsigned long g_iba_queue_mtx[16];
static int g_iba_queue_ready = 0;
static unsigned long g_iba_epoch = 0;
static unsigned long g_iba_run[16];
static void iba_run_noop(void*) {}
static unsigned long g_iba_run_vt[12];
static const unsigned long kExecVptr = 0x0922d2b8UL, kExecLoop = 0x08d5fce4UL;
typedef void* (*IbaCurrentPU)();
/* CProcUnit executor member: 0x3c8 on E9x-family builds, 0x3b8 on the 2010 C13 build
 * (C13 HmiMain: pu+0x3b8 passed to the executor add, same pattern as E9x pu+0x3c8). */
static unsigned long iba_pu_exec_off() { return g_iba_profile && g_iba_profile->xlat == kXlatC13 ? 0x3b8UL : 0x3c8UL; }
static void iba_capture_pu() {
    if (!iba_profile_select()) return;
    void* pu = ((IbaCurrentPU)iba_a(0x08d5c528UL))();
    if (!pu) return;
    unsigned long* ex = *(unsigned long**)iba_off(pu, iba_pu_exec_off());
    if ((unsigned long)ex != iba_a(kExecVptr) || ex[5] != iba_a(kExecLoop)) return;
    g_iba_pu = pu; g_iba_marshal_ok = 1;
}
static void iba_cb(void* epoch, int, void* runnable) {
    pthread_mutex_lock(g_iba_queue_mtx);
    if ((unsigned long)epoch != g_iba_epoch || auto_is_shutdown()) { g_iba_pending = 0; pthread_mutex_unlock(g_iba_queue_mtx); return; }
    pthread_mutex_unlock(g_iba_queue_mtx);
    int pu_ok = pthread_self() == g_hmi_tid && runnable == (void*)g_iba_run && ((IbaCurrentPU)iba_a(0x08d5c528UL))() == g_iba_pu;
    if (!pu_ok) { g_iba_msg = "HMI thread not verified"; auto_fail_control("CIC thread not verified."); }
    else { g_iba_tid = pthread_self(); auto_tick(g_app); }
    pthread_mutex_lock(g_iba_queue_mtx); g_iba_pending = 0; pthread_mutex_unlock(g_iba_queue_mtx);
}
static int iba_queue() {
    if (!g_iba_marshal_ok || !g_iba_queue_ready) return 0;
    pthread_mutex_lock(g_iba_queue_mtx);
    if (g_iba_pending) { pthread_mutex_unlock(g_iba_queue_mtx); return 0; }
    for (unsigned i = 0; i < 16; ++i) g_iba_run[i] = 0;
    for (unsigned i = 0; i < 12; ++i) g_iba_run_vt[i] = (unsigned long)&iba_run_noop;
    g_iba_run[0] = (unsigned long)g_iba_run_vt;
    g_iba_pending = 1;
    typedef int (*Add)(void*, void*, void*, int, void*);
    int id = ((Add)iba_a(0x08d5f9a4UL))(iba_off(g_iba_pu, iba_pu_exec_off()), (void*)g_iba_epoch, (void*)&iba_cb, 0, (void*)g_iba_run);
    if (id == -1) g_iba_pending = 0;
    pthread_mutex_unlock(g_iba_queue_mtx);
    return id != -1;
}

/* ---------------- automatic flow (start -> playing -> stop) ---------------- */
#include "auto_player.h"
static unsigned long g_auto_mtx[16];
static int g_auto_ready = 0, g_auto_shutdown = 1, g_auto_worker = 0, g_auto_joinable = 0, g_auto_tid = 0;
static int g_auto_command = 0, g_auto_need_tick = 0, g_auto_abort = 0;
static char g_auto_text[160];
static AutoPlayer g_auto_player;
static int auto_is_shutdown() {
    if (!g_auto_ready) return 1;
    pthread_mutex_lock(g_auto_mtx); int v = g_auto_shutdown; pthread_mutex_unlock(g_auto_mtx); return v;
}
static int auto_busy_control() {
    if (!g_auto_ready) return 0;
    pthread_mutex_lock(g_auto_mtx); int v = g_auto_worker; pthread_mutex_unlock(g_auto_mtx); return v;
}
static void auto_publish(const char* s) {
    pthread_mutex_lock(g_auto_mtx);
    int changed = !str_eq(g_auto_text, s);
    copy_str(g_auto_text, s, sizeof g_auto_text);
    pthread_mutex_unlock(g_auto_mtx);
    if (changed) alog(s);
}
static void auto_fail_control(const char* s) {
    pthread_mutex_lock(g_auto_mtx); g_auto_abort = 1; g_auto_need_tick = 0; g_auto_command = 0; pthread_mutex_unlock(g_auto_mtx);
    auto_publish(s);
}
static int auto_pending() {
    pthread_mutex_lock(g_iba_queue_mtx); int p = g_iba_pending; pthread_mutex_unlock(g_iba_queue_mtx); return p;
}
struct NativeBackend {
    bmw::PluginApp* app;
    explicit NativeBackend(bmw::PluginApp* a) : app(a) {}
    IbaState& state() { return g_iba; }
    bool held() { return g_dsp_pid || g_iba_layer || g_iba_source_changed || g_iba.owns_resources() || g_iba.pending(); }
    bool busy() { return held() || g_iba_restore_failed || g_iba_suspended; }
    bool unlimited() { return true; }
    bool preflight(int) { if (!file_exists(AAP_LIVE)) { message("cicaalive not installed."); return false; } return true; }
    void select(int) {}
    void prepare() { iba_check(app); }
    bool restart() { iba_restart(); return g_iba.untouched() != 0; }
    void step(int stopping, long) { iba_do(app, stopping); }
    const char* last_message() { return g_iba_msg[0] ? g_iba_msg : "Could not complete the operation."; }
    void message(const char* s) {
        /* AA video on screen -> open our TerminalUI page (iDrive keys come to us). */
        if (str_eq(s, "Player started.")) { g_live_disp = 1; g_live_disp_ms = mono_ms(); alog("display ON"); app->triggerEvent(EVENT_OPEN_AA); }
        else if (g_live_disp && (str_eq(s, "Stopped.") || str_eq(s, "Error; resources released.") || str_eq(s, "Stopping and releasing resources..."))) {
            g_live_disp = 0; alog("display OFF");
            if (g_live_user) app->triggerEvent(EVENT_MAIN);
        }
        auto_publish(s);
    }
    int helper_status() {
        if (!g_dsp_pid) return 2;
        int st = 0; int r = waitpid(g_dsp_pid, &st, WNOHANG); int error = r < 0 ? errno : 0;
        if (r == 0) return 0;
        if (r < 0 && error != ECHILD) return 0;
        g_dsp_pid = 0; alog_val("cicaalive exit", st);
        return r < 0 || st != 0 ? 2 : 1;
    }
};
static void auto_tick(bmw::PluginApp* app) {
    pthread_mutex_lock(g_auto_mtx);
    int command = g_auto_command; g_auto_command = 0; int abort = g_auto_abort;
    pthread_mutex_unlock(g_auto_mtx);
    NativeBackend b(app); long now = mono_ms();
    if (abort) { g_auto_player.phase = AUTO_BLOCKED; auto_publish("CIC not responding; restart the CIC."); }
    else {
        if (command == 1) g_auto_player.begin(b, 0, now);
        else if (command == 4) g_auto_player.stop(b, now);
        g_auto_player.tick(b, now);
    }
    menu_hold(mono_ms());
    geometry_hold(app);
    pthread_mutex_lock(g_auto_mtx);
    g_auto_need_tick = !g_auto_abort && g_auto_player.active();
    pthread_mutex_unlock(g_auto_mtx);
}
static void* auto_worker(void*) {
    struct timespec pause = {0, 250000000}; long waiting = 0; int queue_failed = 0;
    for (;;) {
        pthread_mutex_lock(g_auto_mtx); int shutdown = g_auto_shutdown, need = g_auto_need_tick || g_auto_command; pthread_mutex_unlock(g_auto_mtx);
        int finish = shutdown;
        if (!shutdown && !need && !auto_pending()) finish = 1;
        if (!finish && auto_pending()) {
            if (!waiting) waiting = mono_ms();
            if ((unsigned long)(mono_ms() - waiting) > 15000UL) {
                pthread_mutex_lock(g_auto_mtx); g_auto_abort = 1; g_auto_need_tick = 0; g_auto_command = 0; pthread_mutex_unlock(g_auto_mtx);
                auto_publish("CIC not responding."); finish = 1;
            }
        } else if (!finish) {
            waiting = 0;
            if (!iba_queue()) { if (++queue_failed >= 4) { auto_publish("Cannot communicate with the CIC."); finish = 1; } }
            else queue_failed = 0;
        }
        if (finish) {
            pthread_mutex_lock(g_auto_mtx);
            if (!g_auto_shutdown && g_auto_command) { g_auto_abort = 0; g_auto_need_tick = 1; waiting = 0; queue_failed = 0; }
            else { g_auto_worker = 0; pthread_mutex_unlock(g_auto_mtx); return 0; }
            pthread_mutex_unlock(g_auto_mtx);
        } else nanosleep(&pause, 0);
    }
}
/* command 1 = start display, 4 = stop display */
static void auto_request(int command) {
    if (!g_auto_ready || auto_is_shutdown()) return;
    int previous = 0;
    pthread_mutex_lock(g_auto_mtx);
    if (g_auto_worker) {
        if (command == 4) { g_auto_command = 4; g_auto_abort = 0; }
        pthread_mutex_unlock(g_auto_mtx);
        return;
    }
    if (g_auto_joinable) { previous = g_auto_tid; g_auto_joinable = 0; }
    g_auto_worker = 1; g_auto_command = command; g_auto_need_tick = 1; g_auto_abort = 0;
    pthread_mutex_unlock(g_auto_mtx);
    if (previous) pthread_join(previous, 0);
    pthread_mutex_lock(g_auto_mtx);
    int rc = pthread_create(&g_auto_tid, 0, &auto_worker, 0);
    if (rc) { g_auto_worker = 0; g_auto_need_tick = 0; g_auto_command = 0; }
    else g_auto_joinable = 1;
    pthread_mutex_unlock(g_auto_mtx);
}
static void auto_runtime_init() {
    if (!g_auto_ready) g_auto_ready = pthread_mutex_init(g_auto_mtx, 0) == 0;
    if (!g_iba_queue_ready) g_iba_queue_ready = pthread_mutex_init(g_iba_queue_mtx, 0) == 0;
    if (!g_auto_ready || !g_iba_queue_ready) return;
    pthread_mutex_lock(g_iba_queue_mtx); ++g_iba_epoch; if (!g_iba_epoch) ++g_iba_epoch; pthread_mutex_unlock(g_iba_queue_mtx);
    pthread_mutex_lock(g_auto_mtx); g_auto_shutdown = 0; g_auto_worker = 0; g_auto_command = 0; g_auto_need_tick = 0; g_auto_abort = 0; g_auto_text[0] = 0; pthread_mutex_unlock(g_auto_mtx);
    g_auto_player.reset();
}
static void auto_runtime_shutdown() {
    if (!g_auto_ready) return;
    pthread_mutex_lock(g_auto_mtx); g_auto_shutdown = 1; int tid = g_auto_joinable ? g_auto_tid : 0; g_auto_joinable = 0; pthread_mutex_unlock(g_auto_mtx);
    if (tid && tid != pthread_self()) pthread_join(tid, 0);
}

/* ======================= AA session ======================= */
/* Patched DSP image linked once per boot (RAM link; a CIC reboot restores stock). */
static int dsp_patch_ensure() {
    int pid = spawnl(P_NOWAIT, AAP_DSPLOAD, AAP_DSPLOAD, "linked", AAP_DSP_IMG, (char*)0);
    int st = -1;
    if (pid > 0) { for (int i = 0; i < 40 && waitpid(pid, &st, WNOHANG) != pid; ++i) { struct timespec t = {0, 50000000}; nanosleep(&t, 0); } }
    if (pid > 0 && st == 0) { alog("DSP patch already linked"); return 1; }
    if (!file_exists(AAP_DSP_IMG)) { alog("DSP image missing (reinstall)"); return 0; }
    pid = spawnl(P_NOWAIT, AAP_DSPLOAD, AAP_DSPLOAD, "on", AAP_DSP_IMG, (char*)0);
    if (pid <= 0) { alog("DSP patch: spawn failed"); return 0; }
    st = -1;
    for (int i = 0; i < 80; ++i) {               /* <= 20 s */
        if (waitpid(pid, &st, WNOHANG) == pid) break;
        struct timespec t = {0, 250000000}; nanosleep(&t, 0);
    }
    alog_val("DSP patch status", st);
    return st == 0;
}
static volatile unsigned* g_ring = 0;
/* 1 when libcicaa has published SPS/PPS (cfg_len > 0) for the running session.
 * 1.0 also waited for a NEW frame after the display stop. Android Auto sends no frame
 * while the screen does not change, so after OPTION/MENU -> Connect the picture often
 * did not come back until the map moved. cicaalive asks for an IDR when it attaches
 * (want_idr), and libcicaa now answers that request at once (input_drain). */
static int live_video_ready() {
    if (!g_ring) {
        int fd = shm_open("/cicaa_live", 0 /* O_RDONLY */);
        if (fd < 0) return 0;
        void* m = mmap(0, 4096, 0x100 /* QNX PROT_READ */, 1 /* MAP_SHARED */, fd, 0);
        close(fd);
        if (m == (void*)-1) return 0;
        g_ring = (volatile unsigned*)m;
    }
    /* ring_hdr: magic[0] size[1] wpos[2] rpos[3] reader_pid[4] want_idr[5] cfg_gen[6] cfg_len[7] frames_in[8] */
    if (g_ring[0] != 0x4c495631u) return 0;
    return g_ring[7] > 0;
}
static void status_show(bmw::PluginApp* app, const char* aa) {
    char line[200], v[160];
    pthread_mutex_lock(g_auto_mtx); copy_str(v, g_auto_text, sizeof v); pthread_mutex_unlock(g_auto_mtx);
    unsigned n = put_str(line, 0, aa ? aa : "");
    if (v[0] && !str_eq(v, "Player started.")) { n = put_str(line, n, " | "); n = put_str(line, n, v); }
    line[n] = 0;
    ui(app, MODEL_STATUS, line);
}
static void* aa_poll_thread(void*) {
    bmw::PluginApp* app = g_app;
    char st[96];
    int live_req = 0;
    g_live_user = 1; keymap_load();
    alog("=== AA session ===");
    if (!dsp_patch_ensure()) { ui(app, MODEL_STATUS, "Could not load the patched DSP (reinstall)"); }
    while (app) {
        sleep(1);
        status_show(app, snap(g_aa_status(), st, sizeof st));
        int proj = g_aa_proj ? g_aa_proj() : 1;
        int want = g_live_user && proj;
        if (want && !live_req && live_video_ready()) {
            live_req = 1; alog("AA video -> start display"); auto_request(1);
        } else if (!want && live_req) {
            live_req = 0; alog(proj ? "user left AA -> stop display" : "AA exit -> stop display");
            auto_request(4);
        }
        if (!g_aa_busy()) break;
    }
    if (live_req) { alog("AA ended -> stop display"); auto_request(4); }
    char rs[160]; snap(g_aa_result(), rs, sizeof rs);
    status_show(app, rs[0] ? rs : snap(g_aa_status(), st, sizeof st));
    g_aa_polling = 0;
    return 0;
}
static void location_activate(bmw::PluginApp* app);
static void keys_install();
static bool onConnect(void* obj, const bmw::Parcel&) {
    bmw::PluginApp* app = static_cast<bmw::PluginApp*>(obj);
    status_line(app);
    location_activate(app);
    keys_install();
    if (g_aa_polling) {             /* AA running in background -> bring it back */
        g_live_user = 1;
        if (g_aa_proj && !g_aa_proj() && g_aa_project) g_aa_project();
        /* Start the display right here as well: if the poll thread's earlier request was
           dropped (worker still closing after a quick OPTION -> Connect), pressing Connect
           again now always brings AA back. auto_request() ignores it while busy. */
        if (!g_live_disp && live_video_ready()) auto_request(1);
        return true;
    }
    if (auto_busy_control() || g_iba.owns_resources() || g_iba_layer || g_iba_source_changed) {
        ui(app, MODEL_STATUS, "Closing previous video, wait a few seconds");
        return true;
    }
    if (!g_aa_start) {
        if (!file_exists(AAP_LIB)) { ui(app, MODEL_STATUS, "libcicaa.so missing (install)"); return true; }
        void* h = dlopen(AAP_LIB, 2 /* RTLD_NOW */);
        if (!h) { ui(app, MODEL_STATUS, "Could not load libcicaa.so"); return true; }
        g_aa_start = (fn_int)dlsym(h, "cicaa_start");
        g_aa_busy = (fn_int)dlsym(h, "cicaa_busy");
        g_aa_status = (fn_str)dlsym(h, "cicaa_status");
        g_aa_result = (fn_str)dlsym(h, "cicaa_result");
        g_aa_input = (fn_input)dlsym(h, "cicaa_input");
        g_aa_proj = (fn_int)dlsym(h, "cicaa_projection");
        g_aa_project = (fn_void)dlsym(h, "cicaa_project");
        if (!(g_aa_start && g_aa_busy && g_aa_status && g_aa_result)) { g_aa_start = 0; ui(app, MODEL_STATUS, "libcicaa.so incompatible"); return true; }
    }
    if (g_logs) mkdir(AAP_LOGDIR, 0755);
    int rc = g_aa_start();
    if (rc != 0) { ui(app, MODEL_STATUS, rc == 1 ? "Already running" : "Error starting AA"); return true; }
    g_aa_polling = 1;
    if (spawn(&aa_poll_thread) != 0) { g_aa_polling = 0; ui(app, MODEL_STATUS, "Error creating thread"); return true; }
    ui(app, MODEL_STATUS, "Connect the phone via USB...");
    return true;
}
/* The one status line of the page: "Screen: Normal | BT Audio" (" | Logs ON" instead of
 * the audio mode while logging). Refreshed when the page opens (also via a programmed
 * favourite button), on Connect and after every button press. It also re-reads the
 * audio mode for the next display start. */
static void status_line(bmw::PluginApp* app) {
    static const char* const kMode[3] = {"Normal", "Fullscreen", "Stretched"};
    g_iba_keep_audio = file_exists(AAP_AAUDIO_ON);
    char b[96]; unsigned n = put_str(b, 0, "Screen: ");
    n = put_str(b, n, kMode[screen_mode()]);
    if (g_logs) n = put_str(b, n, " | Logs ON");
    else n = put_str(b, n, g_iba_keep_audio ? " | AA Audio" : " | BT Audio");
    b[n] = 0;
    ui(app, MODEL_SCREEN, b);
}
static bool onPageFocus(void* obj, const KeyMap&) { status_line(static_cast<bmw::PluginApp*>(obj)); return false; }
/* Disconnect: ask libcicaa to end the AA session properly (ByeByeRequest). The phone
 * closes AA, the poll thread sees the session end and stops the display. A later
 * Connect works without re-plugging the cable (libcicaa V59/V60). */
static bool onDisconnect(void*, const bmw::Parcel&) {
    if (g_aa_busy && g_aa_busy() && g_aa_input) { g_aa_input(CICAA_IN_BYEBYE, 0, 0); alog("Disconnect pressed"); }
    return true;
}
/* Switch Audio: BT Audio <-> AA Audio (aaaudio.on). The media channel is only announced
 * at service discovery, so a running session is ended (like Disconnect); the next
 * Connect uses the new mode. */
static bool onSwitchAudio(void* obj, const bmw::Parcel& p) {
    if (file_exists(AAP_AAUDIO_ON)) unlink(AAP_AAUDIO_ON);
    else { int f = open(AAP_AAUDIO_ON, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (f >= 0) close(f); }
    status_line(static_cast<bmw::PluginApp*>(obj));
    return onDisconnect(obj, p);
}
static bool onZoom(void* obj, const bmw::Parcel&) {
    screen_mode_next();
    status_line(static_cast<bmw::PluginApp*>(obj));
    return true;
}
static bool onLogs(void* obj, const bmw::Parcel&) {
    bmw::PluginApp* app = static_cast<bmw::PluginApp*>(obj);
    if (g_logs) { unlink(AAP_LOGS_ON); g_logs = 0; }
    else {
        mkdir(AAP_LOGDIR, 0755);
        int f = open(AAP_LOGS_ON, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (f >= 0) { close(f); g_logs = 1; alog("=== logs ON, AAPlugin " AAP_VERSION " ==="); }
    }
    status_line(app);
    return true;
}

/* ======================= CIC position -> phone ======================= */
/* AAPlugin.xml: <raDataRequestModel id="230" modelType="vehicle.location" />. The names
 * are the RHMI ones of this HmiMain (CLocationProvider; others: vehicle.speed,
 * vehicle.acceleration, vehicle.mileage_in_km, vehicle.tankrange, mmi.units, ...). The
 * value arrives as a wide string "lat,lon" in CIC units (2^32 = 360 degrees), about once
 * per second, also in an underground car park (dead reckoning), "0,0" right after a CIC
 * start. Like the Today/PIM apps: handler registered at create, the update activated
 * on Connect.
 * The PluginApp/Parcel/BasicString<ushort> functions are looked up with dlsym, exactly
 * as in the build tested in the car: if a firmware lacks one of them the plugin still
 * loads and only the position forwarding is off. */
enum { DATA_LOCATION = 230 };
typedef void (*AddDataUpdateHandler)(bmw::PluginApp*, unsigned, const Handler&);
typedef void (*ActivateDataUpdate)(bmw::PluginApp*, unsigned, bool);
typedef bool (*ReadWString)(const bmw::Parcel*, void*);
typedef void (*WStringFn)(void*);
typedef const unsigned short* (*WStringCStr)(const void*);
static struct { ActivateDataUpdate activate; ReadWString read; WStringFn ctor, dtor; WStringCStr c_str; int ok; } g_vd;
static unsigned g_gps_n = 0;
static long g_gps_log_ms = 0;
static int parse_int(const char** p, int* v) {
    const char* s = *p; int neg = 0, any = 0; unsigned u = 0;
    if (*s == '-') { neg = 1; ++s; }
    while (*s >= '0' && *s <= '9') { u = u * 10 + (unsigned)(*s++ - '0'); any = 1; }
    *v = neg ? -(int)u : (int)u; *p = s; return any;
}
static bool onLocation(void*, const bmw::Parcel& parcel) {
    unsigned ws[4] = {0, 0, 0, 0};          /* bmw::BasicString<unsigned short> (one pointer) */
    char text[64]; unsigned n = 0;
    ++g_gps_n;
    g_vd.ctor(ws);
    const unsigned short* w = g_vd.read(&parcel, ws) ? g_vd.c_str(ws) : 0;
    for (; w && w[n] && n + 1 < sizeof text; ++n) text[n] = (char)w[n];
    text[n] = 0;
    g_vd.dtor(ws);
    if (g_logs && (unsigned long)(mono_ms() - g_gps_log_ms) >= 1000UL) {
        char b[96]; unsigned k = put_str(b, 0, "GPS n="); k = put_int(b, k, (int)g_gps_n);
        k = put_str(b, k, " "); k = put_str(b, k, w ? text : "(read failed)"); b[k] = 0;
        alog(b); g_gps_log_ms = mono_ms();
    }
    const char* p = text; int lat, lon;
    if (!w || !parse_int(&p, &lat) || *p++ != ',' || !parse_int(&p, &lon)) return true;
    if (!lat && !lon) return true;           /* no position yet (right after a CIC start) */
    if (g_aa_input && g_aa_busy && g_aa_busy()) g_aa_input(CICAA_IN_LOCATION, lat, lon);
    return true;
}
static void location_init(bmw::PluginApp* app) {
    void* self = dlopen(0, 2);
    AddDataUpdateHandler add = 0;
    if (self) {
        add = (AddDataUpdateHandler)dlsym(self, "_ZN3bmw9PluginApp20addDataUpdateHandlerEjRKNS_8DelegateIbRKNS_6ParcelEvvvEE");
        g_vd.activate = (ActivateDataUpdate)dlsym(self, "_ZN3bmw9PluginApp18activateDataUpdateEjb");
        g_vd.read = (ReadWString)dlsym(self, "_ZNK3bmw6Parcel11readWStringERNS_11BasicStringItEE");
        g_vd.ctor = (WStringFn)dlsym(self, "_ZN3bmw11BasicStringItEC1Ev");
        g_vd.dtor = (WStringFn)dlsym(self, "_ZN3bmw11BasicStringItED1Ev");
        g_vd.c_str = (WStringCStr)dlsym(self, "_ZNK3bmw11BasicStringItE5c_strEv");
    }
    if (add && g_vd.activate && g_vd.read && g_vd.ctor && g_vd.dtor && g_vd.c_str) {
        Handler h; h.object = app; h.stub = &onLocation;
        add(app, DATA_LOCATION, h);
        g_vd.ok = 1;
    }
    alog_val("GPS data update", g_vd.ok);
}
static void location_activate(bmw::PluginApp* app) {
    if (g_vd.ok) g_vd.activate(app, DATA_LOCATION, true);
}

/* ======================= steering-wheel / panel skip keys -> phone ======================= */
/* In AA Audio mode the music comes from the phone over USB, but the CIC sends the skip
 * keys to its own active entertainment source: they never reach the plugin (TerminalUI
 * only gets the controller). HmiMain exports the proxy classes by name, so the plugin
 * replaces one entry of their vtables (in HmiMain's writable data, no code is patched):
 *   CPxyControlElementsMFL::CSearchSwitch1   steering-wheel search keys
 *   CPxyCECenterStack::CPushButtonSkip       |<< >>| under the CD/DVD slot
 * vtable[8] is the message handler (this, msg). The hook calls the original first (the
 * CIC behaves exactly as before), then reads the key state with the class getter
 * (this, int*): 1 / 2 = pressed, 0 = released, every state arrives twice.
 * Steering wheel: 1 = next, 2 = previous. Panel: 1 = previous, 2 = next.
 * Only in AA Audio mode and while a session runs, the state goes to the phone as media
 * key 87 NEXT / 88 PREVIOUS (down, then up on release); duplicates are dropped. In BT
 * Audio mode nothing is sent: the CIC's Bluetooth source gets the keys as always.
 * Safety: installed at the first Connect (not at create, so a mistake cannot cause a
 * boot loop), only if the vtable entry and the first 12 bytes of the getter match the
 * running HmiMain (addresses written for 9283417A, translated by iba_a() for the other
 * known builds); restored in aap_destroy. */
typedef int (*MsgHandler)(void*, void*);
typedef void (*KeyGetter)(void*, int*);
static const unsigned long kKeyGetterSig[3] = {0x2f962f86UL, 0x2fe62fc6UL, 0xc70e4f22UL};
struct KeyHook {
    const char* vtable; unsigned long orig, getter; MsgHandler hook;
    unsigned long* slot; const char* tag;
};
static int key_hook_mfl(void*, void*);
static int key_hook_panel(void*, void*);
static KeyHook g_kh[2] = {
    {"_ZTVN22CPxyControlElementsMFL14CSearchSwitch1E", 0x08ba0bb0UL, 0x08ba0b60UL, &key_hook_mfl, 0, "KEY mfl"},
    {"_ZTVN17CPxyCECenterStack15CPushButtonSkipE", 0x08b935dcUL, 0x08b9358cUL, &key_hook_panel, 0, "KEY panel"},
};
static int g_keys_tried = 0, g_key_held = 0;
static void key_forward(int v) {
    if (!g_iba_keep_audio || !g_aa_input || !g_aa_busy || !g_aa_busy()) return;
    int code = v == 1 ? 87 : v == 2 ? 88 : 0;             /* KEYCODE_MEDIA_NEXT / _PREVIOUS */
    if (!code) {                                           /* released */
        if (v == 0 && g_key_held) { g_aa_input(CICAA_IN_KEY, g_key_held, 0); g_key_held = 0; }
        return;
    }
    if (code == g_key_held) return;                        /* repeated state */
    if (g_key_held) g_aa_input(CICAA_IN_KEY, g_key_held, 0);
    g_aa_input(CICAA_IN_KEY, code, 1);
    g_key_held = code;
}
static int key_hook(int which, void* self, void* msg) {
    int rc = ((MsgHandler)g_kh[which].orig)(self, msg);   /* CIC first, unchanged */
    int v = -1;
    ((KeyGetter)g_kh[which].getter)(self, &v);
    alog_val(g_kh[which].tag, v);
    if (which == 1 && (v == 1 || v == 2)) v = 3 - v;      /* panel: 1 = previous, 2 = next */
    key_forward(v);
    return rc;
}
static int key_hook_mfl(void* self, void* msg) { return key_hook(0, self, msg); }
static int key_hook_panel(void* self, void* msg) { return key_hook(1, self, msg); }
static void keys_install() {
    if (g_keys_tried) return;
    g_keys_tried = 1;
    iba_profile_select();                                  /* iba_a() needs the build profile */
    void* self = dlopen(0, 2);
    for (int k = 0; k < 2; ++k) {
        KeyHook& h = g_kh[k];
        unsigned long* vt = self ? (unsigned long*)dlsym(self, h.vtable) : 0;
        unsigned long* slot = vt ? vt + 8 : 0;             /* vtable symbol + 32 bytes */
        h.orig = iba_a(h.orig); h.getter = iba_a(h.getter);  /* this HmiMain build */
        const unsigned long* g = (const unsigned long*)h.getter;
        int ok = slot && *slot == h.orig && g[0] == kKeyGetterSig[0] && g[1] == kKeyGetterSig[1] && g[2] == kKeyGetterSig[2];
        if (ok) { h.slot = slot; *slot = (unsigned long)h.hook; }   /* one aligned store */
        alog_val(k ? "keys panel hook" : "keys mfl hook", ok ? 1 : slot ? (int)*slot : 0);
    }
}
static void keys_restore() {
    for (int k = 0; k < 2; ++k) if (g_kh[k].slot) { *g_kh[k].slot = g_kh[k].orig; g_kh[k].slot = 0; }
}

/* bmw::PluginApp::addHandlerWithMap exists from the 2011 framework on (E9x 9283417A, E60,
 * F30). The 2010 one (E90 C13, B1009-500-1019C) has only the Parcel handlers, and its
 * HmiMain has no CTerminalUIApp, so the terminalUI key/focus maps are never sent there.
 * Linking the symbol would stop the plugin from loading on C13: it is looked up instead.
 * Without it the page event is taken through the Parcel handler (same ids) and the
 * terminalUI ids only log what arrives, for a first car test. Keys still work through
 * the panel / steering-wheel hooks (keys_install). */
typedef void (*AddMap1)(bmw::PluginApp*, unsigned, const KeyHandler&);
typedef void (*AddMap2)(bmw::PluginApp*, unsigned, unsigned, const KeyHandler&);
static bool onPageParcel(void* obj, const bmw::Parcel&) { status_line(static_cast<bmw::PluginApp*>(obj)); return false; }
static bool onTuiParcel(void*, const bmw::Parcel&) { alog("terminalUI parcel event (no map framework)"); return false; }
static void add2(bmw::PluginApp* app, unsigned id, unsigned sub, bool (*stub)(void*, const bmw::Parcel&)) {
    Handler h; h.object = app; h.stub = stub; app->addHandler(id, sub, h);
}
static void map_handlers_init(bmw::PluginApp* app, const KeyHandler& keys, const KeyHandler& focus, const KeyHandler& page) {
    AddMap1 m1 = 0; AddMap2 m2 = 0;
    /* Global scope first (HmiMain exports its own PluginApp, which is what a direct link
       bound to), then the library by name. */
    const char* kM1 = "_ZN3bmw9PluginApp17addHandlerWithMapEjRKNS_8DelegateIbRKNS_3MapIjNS_7VariantEEEvvvEE";
    const char* kM2 = "_ZN3bmw9PluginApp17addHandlerWithMapEjjRKNS_8DelegateIbRKNS_3MapIjNS_7VariantEEEvvvEE";
    void* hs[2] = {dlopen(0, 2 /* RTLD_NOW */), 0};
    for (int i = 0; i < 2 && !(m1 && m2); ++i) {
        if (i == 1) hs[1] = dlopen("libAppFramework.A.so", 2);
        if (!hs[i]) continue;
        m1 = (AddMap1)dlsym(hs[i], kM1);
        m2 = (AddMap2)dlsym(hs[i], kM2);
    }
    if (m1 && m2) {
        m1(app, AA_KEYS, keys);
        m2(app, AA_SCREEN, 1, focus);
        m2(app, PAGE_MAIN, 1, page);          /* page opened -> fresh status line */
        m2(app, PAGE_MAIN_FIRST, 1, page);
        return;
    }
    alog("no addHandlerWithMap (2010 framework): parcel handlers");
    add(app, AA_KEYS, &onTuiParcel);
    add2(app, AA_SCREEN, 1, &onTuiParcel);
    add2(app, PAGE_MAIN, 1, &onPageParcel);
    add2(app, PAGE_MAIN_FIRST, 1, &onPageParcel);
}

/* E90 C13 (B1009) iDrive keys. Its HmiMain has no CTerminalUIApp, so the terminalUI key
 * map never reaches the plugin. CTerminalUIState::onKeyInputEvent(component, key, f12, f13)
 * (0x08ced5bc) builds exactly that map (fields 11, 12, 13, as on E9x) and is bound as a
 * signal slot through its GOT entry 0x08f857d0 (R_SH_GLOB_DAT). The entry is replaced
 * before the AA terminalUI state is created, so the state connects our function; it
 * calls the original first and then feeds the same values to key_dispatch(). Only with
 * the C13 profile and only if the GOT entry and the first 32 bytes of the function
 * match; otherwise nothing is touched. */
typedef void (*TuiKeyFn)(void*, void*, int, int, int);
static unsigned long* g_tui_slot = 0;
static unsigned long g_tui_orig = 0;
static const unsigned char kTuiKeySig[32] = {
    0x86,0x2f,0x96,0x2f,0xa6,0x2f,0xb6,0x2f,0xc6,0x2f,0xd6,0x2f,0xe6,0x2f,0xcb,0xff,
    0x22,0x4f,0x52,0xc7,0x51,0xdc,0x0c,0x3c,0xa8,0x7f,0xa8,0x7f,0xf3,0x6e,0x43,0x6b};
static void tui_key_hook(void* self, void* component, int key, int f12, int f13) {
    ((TuiKeyFn)g_tui_orig)(self, component, key, f12, f13);   /* CIC first, unchanged */
    unsigned v[16];
    for (int i = 0; i < 16; ++i) v[i] = 0;
    v[11] = (unsigned)key; v[12] = (unsigned)(f12 & 0xff); v[13] = (unsigned)(f13 & 0xff);
    key_dispatch(0, v, (1u << 11) | (1u << 12) | (1u << 13));
}
static void tui_hook_install() {
    const IbaProfile* p = iba_profile_select();
    if (!p || p->xlat != kXlatC13) return;
    unsigned long* slot = (unsigned long*)0x08f857d0UL;
    const unsigned char* code = (const unsigned char*)0x08ced5bcUL;
    int ok = *slot == 0x08ced5bcUL;
    for (int i = 0; ok && i < 32; ++i) ok = code[i] == kTuiKeySig[i];
    if (ok) { g_tui_orig = *slot; g_tui_slot = slot; *slot = (unsigned long)&tui_key_hook; }   /* one aligned store */
    alog_val("tui key hook", ok ? 1 : (int)*slot);
}
static void tui_hook_restore() {
    if (g_tui_slot) { *g_tui_slot = g_tui_orig; g_tui_slot = 0; }
}

extern "C" void* aap_create(bmw::IDispatcher& dispatcher, const char* name,
                            const bmw::Bundle& dependencies, const bmw::Bundle& parameters) {
    if (!name || !*name) return 0;
    bmw::PluginApp* app = new bmw::PluginApp(dispatcher, bmw::BasicString<char>(name), dependencies, parameters);
    if (!app) return 0;
    g_logs = file_exists(AAP_LOGS_ON);
    add(app, ACTION_CONNECT, &onConnect);
    add(app, ACTION_DISCONNECT, &onDisconnect);
    add(app, ACTION_ZOOM, &onZoom);
    add(app, ACTION_AUDIO, &onSwitchAudio);
    add(app, ACTION_LOGS, &onLogs);
    KeyHandler keys = {app, &onKeys}, focus = {app, &onFocus}, page = {app, &onPageFocus};
    map_handlers_init(app, keys, focus, page);
    tui_hook_install();                                  /* E90 C13 only */
    location_init(app);
    g_hmi_tid = pthread_self();
    g_ui_ready = (pthread_mutex_init(g_ui_mtx, 0) == 0);
    g_app = app;
    auto_runtime_init();
    iba_capture_pu();     /* HMI ProcUnit of the loading thread, for native calls */
    /* No updateModel here: the adapter is not started yet (XML text= is the prefix). */
    return app;
}

extern "C" void aap_destroy(void* app) {
    if (!app) return;
    keys_restore();
    tui_hook_restore();
    auto_runtime_shutdown();
    if (g_iba_target_live && pthread_self() == g_iba_tid) {
        typedef void (*Dtor)(void*); ((Dtor)iba_a(0x08d6ed6cUL))(g_iba_target); g_iba_target_live = 0;
    }
    g_app = 0;
    typedef void (*DeletingDestructor)(void*);
    DeletingDestructor* nativeVtable = *reinterpret_cast<DeletingDestructor**>(app);
    nativeVtable[1](app);   /* native deleting destructor: framework allocator + cleanup */
}

extern "C" {
struct CicPluginAppInfo {
    unsigned int opaque[3];
    void* (*create)(bmw::IDispatcher&, const char*, const bmw::Bundle&, const bmw::Bundle&);
    void (*destroy)(void*);
};
typedef char descriptor_must_be_20_bytes[sizeof(CicPluginAppInfo) == 20 ? 1 : -1];
CicPluginAppInfo pluginAppInfo = {{0, 0, 0}, &aap_create, &aap_destroy};
}
