/* AAPlugin: Android Auto for the BMW CIC HIGH (QNX 6.3.2, SH4, HmiMain 9283417A).
 *
 * One HMI page: [Conectar Android Auto] [Logs ON/OFF] + one status line.
 *
 *  Conectar -> dlopen libcicaa.so (AA protocol over USB, TLS, channels) and start it.
 *           -> patched DM642 DSP image is linked in RAM (dspload on) once per boot.
 *           -> when the first phone video arrives: native HMI video route
 *              (audio C4, video source 8 -> sink 1, StartVideo, geometry, layer 26,
 *              menu layer alpha 0) and the cicaalive process feeds the DSP.
 *           -> our TerminalUI page (210) takes the iDrive: rotary/push/directions go
 *              to the phone. MENU or Volver leaves AA (it keeps running); pressing
 *              Conectar again brings it back. The AA "exit" icon also leaves.
 *  Logs     -> creates/removes /mnt/hbdebug/aap/logs.on. Nothing is written
 *              anywhere unless it is ON; then logs go to /mnt/hbdebug/aap/logs/.
 *
 * Private HmiMain ABI (fixed addresses) is checked against code signatures before
 * any call; on mismatch nothing is touched. Native HMI services are only used on
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
    void addHandlerWithMap(unsigned, const Delegate<bool, const Map<unsigned, Variant>&, void, void, void>&);
    void addHandlerWithMap(unsigned, unsigned, const Delegate<bool, const Map<unsigned, Variant>&, void, void, void>&);
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

/* IDs must match AAPlugin.xml. */
enum {
    ACTION_BACK_MAIN = 3,          /* hmiAction -> page 10 */
    ACTION_CONNECT = 181, ACTION_LOGS = 185,
    MODEL_STATUS = 182, MODEL_LOGS = 186,
    AA_SCREEN = 210, AA_KEYS = 211, ACTION_AA_BACK = 204,
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
    if (key == 13) g_aa_input(2, 65536, (int)steps);          /* rotary clockwise */
    else if (key == 14) g_aa_input(2, 65536, -(int)steps);    /* rotary counter-clockwise */
    else if (key < 64 && g_keymap[key]) g_aa_input(1, g_keymap[key], down);
}
typedef bmw::Map<unsigned, bmw::Variant> KeyMap;
typedef bmw::Delegate<bool, const KeyMap&, void, void, void> KeyHandler;
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
    if (!focus) { aa_forward_key(values, valid); return; }
    /* MENU / leaving our page while AA is shown -> back to the car UI (AA keeps running). */
    if (g_live_disp && (unsigned long)(mono_ms() - g_live_disp_ms) > 2000UL) { g_live_user = 0; alog("focus lost -> leave AA display"); }
}
static bool onKeys(void*, const KeyMap& map) { key_sample(0, &map); return false; }
static bool onFocus(void*, const KeyMap& map) { key_sample(1, &map); return false; }
static bool onAaBack(void* obj, const bmw::Parcel&) {
    if (g_live_disp) { g_live_user = 0; alog("Volver -> leave AA display"); }
    static_cast<bmw::PluginApp*>(obj)->triggerEvent(EVENT_MAIN);
    return true;
}

/* ======================= native HMI video route ======================= */
#include "iba_state.h"
#include "iba_firmware.h"
#include "iba_marshal.h"
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
static const char* const kIbaNames[] = {"comprobar", "pedir audio C4", "seleccionar fichero",
 "conectar video 8->1", "arrancar video", "mostrar capa", "parar video",
 "retirar video", "retirar audio", "esperando ACK", "video en marcha", "terminado", "fallo"};
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
    for (unsigned i = 0; i < sizeof(kIbaAnchors) / sizeof(kIbaAnchors[0]); ++i) {
        const unsigned char* bytes = (const unsigned char*)kIbaAnchors[i].address;
        for (unsigned j = 0; j < 32; ++j)
            if (bytes[j] != kIbaAnchors[i].bytes[j]) { g_iba_sig_bad = (int)kIbaAnchors[i].address; alog_val("SIGNATURE_MISMATCH", g_iba_sig_bad); return 0; }
    }
    return 1;
}
static int iba_list(void* proxy, unsigned offset, void* cb, int property = 0) {
    typedef int (*AddListener)(void*, void*, void*, unsigned);
    if (!iba_before("register listener")) return 0;
    return ((AddListener)(property ? 0x082c2268UL : 0x082c2148UL))(iba_off(proxy, offset), g_iba_target, cb, 0);
}
static int iba_check(bmw::PluginApp* app) {
    if (g_iba_checked) return g_iba.ready;
    if (g_iba_tid == -1) g_iba_tid = pthread_self();
    g_iba_checked = 1;
    if (!iba_before("firmware check")) { iba_ui(app, "Hilo HMI distinto"); return 0; }
    if (!iba_signatures()) { iba_ui(app, "HmiMain distinto: no compatible"); return 0; }
    typedef void* (*Factory)(unsigned);
    typedef void (*Ctor)(void*);
    ((Ctor)0x08201c58UL)(g_iba_target); g_iba_target_live = 1;
    g_iba_cm = ((Factory)0x08b9d1b0UL)(0);
    g_iba_aux = ((Factory)0x08b64d18UL)(38);
    g_iba_vcm = ((Factory)0x08ce0fc4UL)(1);
    g_iba_sink = ((Factory)0x08ce328cUL)(1);
    if (!(g_iba_cm && g_iba_aux && g_iba_vcm && g_iba_sink)) { iba_ui(app, "Faltan servicios de video del CIC"); return 0; }
    if (*(unsigned long*)iba_off(g_iba_cm, 0x6c8) != 0x0920cbbcUL ||
        *(unsigned long*)iba_off(g_iba_vcm, 0x6c8) != 0x092276c0UL ||
        *(unsigned long*)iba_off(g_iba_vcm, 0x760) != 0x0922767cUL ||
        *(unsigned long*)iba_off(g_iba_aux, 0x1de4) != kIbaSelectVtable ||
        *(unsigned long*)iba_off(g_iba_sink, 0xa48) != kIbaStartVtable) { iba_ui(app, "Layout de servicios distinto"); return 0; }
#define IBA_LIST(p, o, f) if (!iba_list(p, o, (void*)&f)) { iba_ui(app, "No se pudo registrar listener"); return 0; }
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
        !iba_list(g_iba_vcm, 0x7f8, (void*)&iba_deck, 1)) { iba_ui(app, "Suscripcion de propiedades fallo"); return 0; }
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
    unsigned h = ((((Role)0x08d6ebbcUL)() & 0xffffU) << 4) | 0xf000U;
    return (h + (g_iba_sequence++ & 15U)) & 0xffffU;
}
static int iba_idle_sink() {
    typedef int (*Get)(void*, int*); int source = -1;
    if (!iba_before("read current video source")) return 0;
    typedef int (*ValidSource)(void*);
    if (!((ValidSource)0x08d1ae64UL)(iba_off(g_iba_sink, 0xc80))) return 0;
    int rc = ((Get)0x08ce4890UL)(iba_off(g_iba_sink, 0xc80), &source);
    alog_val("current source", source);
    int own = g_iba.result[IBA_CONNECT] == 1;
    if (!rc || (source != 0 && !(own && source == 8))) { iba_ui(g_app, "Pantalla de video ocupada por otra fuente"); return 0; }
    typedef int (*Valid)(void*);
    typedef int (*Table)(void*, int, int*);
    void* table = iba_off(g_iba_vcm, 0x7f8);
    if (!((Valid)0x08d1ae64UL)(table)) return 0;
    int connected = -1;
    if (!((Table)0x08ce160cUL)(table, 1, &connected) || connected != (own ? 8 : 0)) {
        /* car: connect ACK but table stays 0 until video starts; only that case continues */
        if (!(own && connected == 0 && source == 0)) { iba_ui(g_app, "Tabla de video distinta"); return 0; }
    }
    if (!own) g_iba_previous_source = source;
    return 1;
}
static int iba_rect_valid(unsigned offset) {
    typedef int (*Valid)(void*);
    return ((Valid)0x08d1ae64UL)(iba_off(g_iba_sink, offset)) != 0;
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
static void geom_load(short* g) {
    static const short def[8] = AAP_GEOM;
    for (int i = 0; i < 8; ++i) g[i] = def[i];
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
    void* defs = ((Factory)0x08938a00UL)(); if (!defs) return 0;
    short native[2][4];
    int ok = ((Geometry)0x08938b60UL)(defs, 1, 0, 0, native[0]) && ((Geometry)0x08938b60UL)(defs, 2, 0, 0, native[1]);
    short g[8]; geom_load(g);
    typedef int (*Rect)(void*, short, short, short, short);
    int okin = ((Rect)0x08ce4408UL)(iba_off(g_iba_sink, 0xb78), g[0], g[1], g[2], g[3]);
    int okout = okin && ((Rect)0x08ce464cUL)(iba_off(g_iba_sink, 0xbfc), g[4], g[5], g[6], g[7]);
    if (!okout) {
        alog("AA geometry refused -> native DVD window");
        if (!ok) return 0;
        if (!((Rect)0x08ce4408UL)(iba_off(g_iba_sink, 0xb78), native[0][0], native[0][1], native[0][2], native[0][3])) return 0;
        if (!((Rect)0x08ce464cUL)(iba_off(g_iba_sink, 0xbfc), native[1][0], native[1][1], native[1][2], native[1][3])) return 0;
    }
    return 1;
}
static int iba_route_ours(bmw::PluginApp* app) {
    typedef int (*Valid)(void*); typedef int (*Get)(void*, int*); typedef int (*Table)(void*, int, int*);
    void* src = iba_off(g_iba_sink, 0xc80); void* table = iba_off(g_iba_vcm, 0x7f8);
    if (!((Valid)0x08d1ae64UL)(src) || !((Valid)0x08d1ae64UL)(table)) { iba_ui(app, "Esperando ruta de video"); return 0; }
    int current = -1, connection = -1;
    if (!((Get)0x08ce4890UL)(src, &current) || !((Table)0x08ce160cUL)(table, 1, &connection)) return 0;
    int own_table_lag = connection == 0 && g_iba.result[IBA_CONNECT] == 1;
    if ((current != 0 && current != 8) || (connection != 8 && !own_table_lag)) { iba_ui(app, "Ruta de video cambiada por otro cliente"); return 0; }
    return 1;
}
static int iba_restore_sink(bmw::PluginApp* app) {
    if (!g_iba_source_changed) return 1;
    if (g_iba_restore_failed) { iba_ui(app, "Fallo restaurando video; reinicia el CIC"); return 0; }
    typedef int (*SetSource)(void*, int, unsigned char);
    typedef int (*Rect)(void*, short, short, short, short);
    if (!g_iba_restore_sent) {
        if (!iba_route_ours(app)) return 0;
        if (!iba_before("restore sink")) return 0;
        g_iba_restore_sent = 1;
        for (unsigned i = 0; i < 2; ++i) {
            short* r = g_iba_rect[i];
            if (!((Rect)(i ? 0x08ce464cUL : 0x08ce4408UL))(iba_off(g_iba_sink, i ? 0xbfc : 0xb78), r[0], r[1], r[2], r[3])) { g_iba_restore_failed = 1; return 0; }
        }
        if (!((SetSource)0x08ce48e0UL)(iba_off(g_iba_sink, 0xc80), g_iba_previous_source, 0)) { g_iba_restore_failed = 1; return 0; }
    }
    typedef int (*Get)(void*, int*); int source = -1;
    int rc = ((Get)0x08ce4890UL)(iba_off(g_iba_sink, 0xc80), &source);
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
        if (!live_spawn()) { g_iba.failed = 1; iba_ui(app, "No se pudo arrancar cicaalive"); return 0; }
        return 0;   /* next tick shows the layer */
    }
    if (!g_dsp_pid) return 0;
    int status = 0; int r = waitpid(g_dsp_pid, &status, WNOHANG);
    int error = r < 0 ? errno : 0;
    if (r == 0) return 1;
    if (r < 0 && error != ECHILD) { if (error != EINTR) g_iba.failed = 1; return 0; }
    g_dsp_pid = 0; g_iba.failed = 1; iba_ui(app, "cicaalive termino antes de mostrar video"); return 0;
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
        if (i >= 0 && vt[14] == 0x08af0210UL && vt[15] == 0x08af0278UL) {
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
/* One step of the native flow (HMI thread only). */
static void iba_do(bmw::PluginApp* app, int stopping) {
    if (pthread_self() != g_iba_tid) { iba_ui(app, "Accion fuera del hilo HMI"); return; }
    if (!g_iba.ready) { if (!stopping) iba_check(app); return; }
    if (g_iba_suspended) { iba_ui(app, "Callback fuera del hilo HMI"); return; }
    g_iba.expire(mono_ms());
    if (stopping) g_iba.stopping = 1;
    IbaOp op = g_iba.next();
    if (g_iba.stopping && (op == IBA_DISCONNECT || op == IBA_RELEASE || op == IBA_DONE) && !iba_restore_sink(app)) return;
    if (op >= IBA_WAIT) return;
    if (op == IBA_AUDIO) {
        if (!file_exists(AAP_LIVE)) { iba_ui(app, "Falta cicaalive (reinstalar)"); return; }
        if (!iba_idle_sink()) return;
        if (!iba_snapshot_rect()) return;
    }
    if (op == IBA_SHOW) {
        if (*(unsigned long*)0x091fda28UL != 0) { g_iba.failed = 1; iba_ui(app, "Capa de video ocupada por otro cliente"); return; }
        if (!iba_route_ours(app)) return;
        if (!live_prepare(app)) return;
        if (!iba_apply_geometry(app)) return;
        if (!iba_before("show layer26")) return;
        typedef void* (*Draw)(); void* draw = ((Draw)0x08ae34e4UL)();
        if (!draw) { g_iba.failed = 1; return; }
        unsigned long* vt = *(unsigned long**)draw;
        if (vt[20] != kIbaLayerManagerGetter) { g_iba.failed = 1; return; }
        typedef void* (*Manager)(void*); g_iba_mgr = ((Manager)vt[20])(draw);
        if (!g_iba_mgr) { g_iba.failed = 1; return; }
        vt = *(unsigned long**)g_iba_mgr;
        if (vt[14] != 0x08af0210UL || vt[15] != 0x08af0278UL) { g_iba.failed = 1; return; }
        typedef int (*GetAlpha)(void*, unsigned, unsigned char*);
        if (!((GetAlpha)vt[15])(g_iba_mgr, 26, &g_iba_alpha)) { g_iba.failed = 1; return; }
        g_iba.issue(op, 0, mono_ms());
        typedef void (*SetAlpha)(void*, unsigned, unsigned);
        ((SetAlpha)vt[14])(g_iba_mgr, 26, 255);
        typedef void (*Enable)(int); ((Enable)kLayerHelper)(1); g_iba_layer = 1;
        g_iba.ack(op, 0, 1);
        menu_hide_for_video();
        return;
    }
    if (g_iba.stopping && !live_reap()) return;
    if (op == IBA_STOP && !iba_route_ours(app)) return;
    if (op == IBA_STOP && g_iba_layer) {
        if (!iba_before("hide layer26")) return;
        menu_restore();
        typedef void (*Enable)(int); ((Enable)kLayerHelper)(0); g_iba_layer = 0;
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
    if (op == IBA_AUDIO) rc = ((Connection)0x08b9d308UL)(iba_off(g_iba_cm, 0x6c8), (unsigned short)h, 0xc4);
    else if (op == IBA_RELEASE) rc = ((Connection)0x08b9d460UL)(iba_off(g_iba_cm, 0x760), (unsigned short)h, 0xc4);
    else if (op == IBA_CONNECT) rc = ((VideoConnection)0x08ce111cUL)(iba_off(g_iba_vcm, 0x6c8), (unsigned short)h, 8, 1);
    else if (op == IBA_DISCONNECT) rc = ((VideoConnection)0x08ce135cUL)(iba_off(g_iba_vcm, 0x760), (unsigned short)h, 8, 1);
    else if (op == IBA_START) {
        typedef int (*SetSource)(void*, int, unsigned char);
        g_iba_source_changed = 1;
        rc = ((SetSource)0x08ce48e0UL)(iba_off(g_iba_sink, 0xc80), 8, 0);
        if (rc) rc = ((SinkRun)0x08ce40e4UL)(iba_off(g_iba_sink, 0xa48), (unsigned short)h);
    } else if (op == IBA_STOP) rc = ((SinkRun)0x08ce42c0UL)(iba_off(g_iba_sink, 0xae0), (unsigned short)h);
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
static void iba_capture_pu() {
    for (unsigned i = 0; i < sizeof(kIbaMarshalSigs) / sizeof(kIbaMarshalSigs[0]); ++i) {
        const unsigned char* b = (const unsigned char*)kIbaMarshalSigs[i].address;
        for (unsigned j = 0; j < 32; ++j) if (b[j] != kIbaMarshalSigs[i].bytes[j]) return;
    }
    void* pu = ((IbaCurrentPU)0x08d5c528UL)();
    if (!pu) return;
    unsigned long* ex = *(unsigned long**)iba_off(pu, 0x3c8);
    if ((unsigned long)ex != kExecVptr || ex[5] != kExecLoop) return;
    g_iba_pu = pu; g_iba_marshal_ok = 1;
}
static void iba_cb(void* epoch, int, void* runnable) {
    pthread_mutex_lock(g_iba_queue_mtx);
    if ((unsigned long)epoch != g_iba_epoch || auto_is_shutdown()) { g_iba_pending = 0; pthread_mutex_unlock(g_iba_queue_mtx); return; }
    pthread_mutex_unlock(g_iba_queue_mtx);
    int pu_ok = pthread_self() == g_hmi_tid && runnable == (void*)g_iba_run && ((IbaCurrentPU)0x08d5c528UL)() == g_iba_pu;
    if (!pu_ok) { g_iba_msg = "Hilo HMI no verificado"; auto_fail_control("Hilo del CIC no verificado."); }
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
    int id = ((Add)0x08d5f9a4UL)(iba_off(g_iba_pu, 0x3c8), (void*)g_iba_epoch, (void*)&iba_cb, 0, (void*)g_iba_run);
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
    bool preflight(int) { if (!file_exists(AAP_LIVE)) { message("Falta cicaalive instalado."); return false; } return true; }
    void select(int) {}
    void prepare() { iba_check(app); }
    bool restart() { iba_restart(); return g_iba.untouched() != 0; }
    void step(int stopping, long) { iba_do(app, stopping); }
    const char* last_message() { return g_iba_msg[0] ? g_iba_msg : "No se pudo completar la operacion."; }
    void message(const char* s) {
        /* AA video on screen -> open our TerminalUI page (iDrive keys come to us). */
        if (str_eq(s, "Reproductor iniciado.")) { g_live_disp = 1; g_live_disp_ms = mono_ms(); alog("display ON"); app->triggerEvent(EVENT_OPEN_AA); }
        else if (g_live_disp && (str_eq(s, "Detenido.") || str_eq(s, "Error; recursos liberados.") || str_eq(s, "Deteniendo y liberando recursos..."))) {
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
    if (abort) { g_auto_player.phase = AUTO_BLOCKED; auto_publish("El CIC no responde; reinicia el CIC."); }
    else {
        if (command == 1) g_auto_player.begin(b, 0, now);
        else if (command == 4) g_auto_player.stop(b, now);
        g_auto_player.tick(b, now);
    }
    menu_hold(mono_ms());
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
                auto_publish("El CIC no responde."); finish = 1;
            }
        } else if (!finish) {
            waiting = 0;
            if (!iba_queue()) { if (++queue_failed >= 4) { auto_publish("No se puede comunicar con el CIC."); finish = 1; } }
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
    if (!file_exists(AAP_DSP_IMG)) { alog("DSP image missing (reinstalar)"); return 0; }
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
static unsigned g_ring_base = 0xffffffffu;
/* 1 when libcicaa has published SPS/PPS and a new frame this session. */
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
    if (g_ring_base == 0xffffffffu) { g_ring_base = g_ring[8]; return 0; }
    return g_ring[7] > 0 && g_ring[8] != g_ring_base;
}
static void status_show(bmw::PluginApp* app, const char* aa) {
    char line[200], v[160];
    pthread_mutex_lock(g_auto_mtx); copy_str(v, g_auto_text, sizeof v); pthread_mutex_unlock(g_auto_mtx);
    unsigned n = put_str(line, 0, aa ? aa : "");
    if (v[0] && !str_eq(v, "Reproductor iniciado.")) { n = put_str(line, n, " | "); n = put_str(line, n, v); }
    line[n] = 0;
    ui(app, MODEL_STATUS, line);
}
static void* aa_poll_thread(void*) {
    bmw::PluginApp* app = g_app;
    char st[96];
    int live_req = 0;
    g_ring_base = 0xffffffffu; g_live_user = 1; keymap_load();
    alog("=== AA session ===");
    if (!dsp_patch_ensure()) { ui(app, MODEL_STATUS, "No se pudo cargar el DSP parcheado (reinstalar)"); }
    while (app) {
        sleep(1);
        status_show(app, snap(g_aa_status(), st, sizeof st));
        int proj = g_aa_proj ? g_aa_proj() : 1;
        int want = g_live_user && proj;
        if (want && !live_req && live_video_ready()) {
            live_req = 1; alog("AA video -> start display"); auto_request(1);
        } else if (!want && live_req) {
            live_req = 0; alog(proj ? "user left AA -> stop display" : "AA exit -> stop display");
            auto_request(4); g_ring_base = 0xffffffffu;
        }
        if (!g_aa_busy()) break;
    }
    if (live_req) { alog("AA ended -> stop display"); auto_request(4); }
    char rs[160]; snap(g_aa_result(), rs, sizeof rs);
    status_show(app, rs[0] ? rs : snap(g_aa_status(), st, sizeof st));
    g_aa_polling = 0;
    return 0;
}
static bool onConnect(void* obj, const bmw::Parcel&) {
    bmw::PluginApp* app = static_cast<bmw::PluginApp*>(obj);
    if (g_aa_polling) {             /* AA running in background -> bring it back */
        g_live_user = 1;
        if (g_aa_proj && !g_aa_proj() && g_aa_project) g_aa_project();
        ui(app, MODEL_STATUS, "Volviendo a Android Auto...");
        return true;
    }
    if (auto_busy_control() || g_iba.owns_resources() || g_iba_layer || g_iba_source_changed) {
        ui(app, MODEL_STATUS, "Cerrando video anterior, espera unos segundos");
        return true;
    }
    if (!g_aa_start) {
        if (!file_exists(AAP_LIB)) { ui(app, MODEL_STATUS, "Falta libcicaa.so (instalar)"); return true; }
        void* h = dlopen(AAP_LIB, 2 /* RTLD_NOW */);
        if (!h) { ui(app, MODEL_STATUS, "No se pudo cargar libcicaa.so"); return true; }
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
    if (rc != 0) { ui(app, MODEL_STATUS, rc == 1 ? "Ya en marcha" : "Error arrancando AA"); return true; }
    g_aa_polling = 1;
    if (spawn(&aa_poll_thread) != 0) { g_aa_polling = 0; ui(app, MODEL_STATUS, "Error creando hilo"); return true; }
    ui(app, MODEL_STATUS, "Conecta el movil por USB...");
    return true;
}
static void logs_show(bmw::PluginApp* app) { ui(app, MODEL_LOGS, g_logs ? "ON (" AAP_LOGDIR ")" : "OFF"); }
static bool onLogs(void* obj, const bmw::Parcel&) {
    bmw::PluginApp* app = static_cast<bmw::PluginApp*>(obj);
    if (g_logs) { unlink(AAP_LOGS_ON); g_logs = 0; }
    else {
        mkdir(AAP_LOGDIR, 0755);
        int f = open(AAP_LOGS_ON, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (f >= 0) { close(f); g_logs = 1; alog("=== logs ON, AAPlugin " AAP_VERSION " ==="); }
    }
    logs_show(app);
    return true;
}

extern "C" void* aap_create(bmw::IDispatcher& dispatcher, const char* name,
                            const bmw::Bundle& dependencies, const bmw::Bundle& parameters) {
    if (!name || !*name) return 0;
    bmw::PluginApp* app = new bmw::PluginApp(dispatcher, bmw::BasicString<char>(name), dependencies, parameters);
    if (!app) return 0;
    g_logs = file_exists(AAP_LOGS_ON);
    add(app, ACTION_CONNECT, &onConnect);
    add(app, ACTION_LOGS, &onLogs);
    add(app, ACTION_AA_BACK, &onAaBack);
    KeyHandler keys = {app, &onKeys}, focus = {app, &onFocus};
    app->addHandlerWithMap(AA_KEYS, keys);
    app->addHandlerWithMap(AA_SCREEN, 1, focus);
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
    auto_runtime_shutdown();
    if (g_iba_target_live && pthread_self() == g_iba_tid) {
        typedef void (*Dtor)(void*); ((Dtor)0x08d6ed6cUL)(g_iba_target); g_iba_target_live = 0;
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
