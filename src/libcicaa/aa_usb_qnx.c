/* USB side of the head unit on QNX 6.3 io-usb (libusbdi.so.2):
 *   1. connect to io-usb as a client driver, log every device/interface,
 *   2. pick an Android phone, ask AOA protocol (vendor req 51),
 *   3. send identity strings (52) and START (53) -> phone re-enumerates as accessory,
 *   4. attach to 18d1:2d00/2d01, open bulk IN/OUT pipes -> aa_transport_t.
 * The CIC itself has no AOA support: this file IS the AOA host implementation. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/usbdi.h>
#include "aa_core.h"
#include "aa_usb.h"

#define GOOGLE_VID 0x18d1
#define AOA_PID_ACC 0x2d00
#define AOA_PID_ACC_ADB 0x2d01
#define APPLE_VID 0x05ac

#define MAXDEV 32
static usbd_device_instance_t g_inst[MAXDEV];
static int g_ninst;
static int g_gen;  /* bumps on every insertion/removal */
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static aa_log_fn LG;

static void ins(struct usbd_connection *c, usbd_device_instance_t *in)
{
    (void)c;
    pthread_mutex_lock(&g_mx);
    if (g_ninst < MAXDEV) g_inst[g_ninst++] = *in;
    ++g_gen;
    pthread_mutex_unlock(&g_mx);
    if (LG) LG("USB INSERT path=%d devno=%d vid=%04x pid=%04x class=%02x/%02x/%02x cfg=%d if=%d",
               in->path, in->devno, (unsigned)in->ident.vendor, (unsigned)in->ident.device,
               (unsigned)in->ident.dclass, (unsigned)in->ident.subclass, (unsigned)in->ident.protocol,
               (int)in->config, (int)in->iface);
}
static void rem(struct usbd_connection *c, usbd_device_instance_t *in)
{
    int i;
    (void)c;
    pthread_mutex_lock(&g_mx);
    for (i = 0; i < g_ninst; ++i)
        if (g_inst[i].path == in->path && g_inst[i].devno == in->devno) {
            g_inst[i] = g_inst[--g_ninst];
            --i;
        }
    ++g_gen;
    pthread_mutex_unlock(&g_mx);
    if (LG) LG("USB REMOVE path=%d devno=%d", in->path, in->devno);
}

static struct usbd_connection *g_conn;
static struct usbd_device *g_acc;
static struct usbd_pipe *g_pin, *g_pout;
static struct usbd_urb *g_urb_in, *g_urb_out;
static unsigned char *g_bin, *g_bout;

static int ctl(struct usbd_device *dev, struct usbd_pipe *p, int dir_in, int req, int val, int idx,
               void *data, int len)
{
    struct usbd_urb *u = usbd_alloc_urb(0);
    unsigned char *b = len ? usbd_alloc(len) : 0;
    _uint32 st = 0, got = 0;
    int rc;
    (void)dev;
    if (!u || (len && !b)) return -100;
    if (!dir_in && len) memcpy(b, data, len);
    usbd_setup_vendor(u, dir_in ? URB_DIR_IN : URB_DIR_OUT, req, USB_TYPE_VENDOR | USB_RECIPIENT_DEVICE,
                      val, idx, b, len);
    rc = usbd_io(u, p, 0, 0, 2000);
    usbd_urb_status(u, &st, &got);
    if (dir_in && b && rc == 0) memcpy(data, b, got < (unsigned)len ? got : (unsigned)len);
    if (LG) LG("USB ctl req=%d dir=%s rc=%d status=0x%08x len=%u", req, dir_in ? "IN" : "OUT", rc,
               (unsigned)st, (unsigned)got);
    if (b) usbd_free(b);
    usbd_free_urb(u);
    if (rc) return rc;
    if ((st & USBD_URB_STATUS_MASK) != USBD_STATUS_CMP) return -101;
    return (int)got;
}

static struct usbd_pipe *open_ctl(struct usbd_device *dev)
{
    struct usbd_desc_node *node;
    usbd_device_descriptor_t *dd = usbd_device_descriptor(dev, &node);
    struct usbd_pipe *p = 0;
    if (!dd) return 0;
    if (usbd_open_pipe(dev, (usbd_descriptors_t *)dd, &p) != 0) return 0;
    return p;
}

static int find_inst(int vid_acc_only, usbd_device_instance_t *out)
{
    int i, found = 0;
    pthread_mutex_lock(&g_mx);
    for (i = 0; i < g_ninst && !found; ++i) {
        usbd_device_instance_t *d = &g_inst[i];
        unsigned v = d->ident.vendor, p = d->ident.device, c = d->ident.dclass;
        if (vid_acc_only) {
            if (v == GOOGLE_VID && (p == AOA_PID_ACC || p == AOA_PID_ACC_ADB) && d->iface == 0) { *out = *d; found = 1; }
        } else {
            /* phone candidates: not Apple, not hub/mass storage/HID/audio; MTP/PTP(6), vendor(ff), misc(0) */
            if (v != APPLE_VID && c != 0x09 && c != 0x08 && c != 0x03 && c != 0x01 &&
                (c == 0x06 || c == 0xff || c == 0x00 || c == 0xef) && d->iface == 0) { *out = *d; found = 1; }
        }
    }
    pthread_mutex_unlock(&g_mx);
    return found;
}

static int aoa_switch(usbd_device_instance_t *in)
{
    static const char *ids[6] = {"Android", "Android Auto", "Android Auto", "2.0.1",
                                 "https://www.android.com/auto/", "HU-CIC0000001"};
    struct usbd_device *dev = 0;
    struct usbd_pipe *p;
    unsigned char ver[2] = {0, 0};
    int rc, i, proto;
    rc = usbd_attach(g_conn, in, 0, &dev);
    LG("AOA attach vid=%04x pid=%04x rc=%d", (unsigned)in->ident.vendor, (unsigned)in->ident.device, rc);
    if (rc) return -1;
    p = open_ctl(dev);
    if (!p) { LG("AOA no control pipe"); usbd_detach(dev); return -1; }
    rc = ctl(dev, p, 1, 51, 0, 0, ver, 2);
    proto = ver[0] | (ver[1] << 8);
    LG("AOA GET_PROTOCOL rc=%d protocol=%d", rc, proto);
    if (rc < 2 || proto < 1) { usbd_close_pipe(p); usbd_detach(dev); return -2; }
    for (i = 0; i < 6; ++i) {
        rc = ctl(dev, p, 0, 52, 0, i, (void *)ids[i], (int)strlen(ids[i]) + 1);
        if (rc < 0) LG("AOA SEND_STRING %d failed %d", i, rc);
    }
    rc = ctl(dev, p, 0, 53, 0, 0, 0, 0);
    LG("AOA START rc=%d (el movil debe reconectarse como 18d1:2d00)", rc);
    usbd_close_pipe(p);
    usbd_detach(dev);
    return proto;
}

static int open_accessory(usbd_device_instance_t *in)
{
    struct usbd_desc_node *root = 0, *node;
    usbd_descriptors_t *d;
    int i, rc;
    rc = usbd_attach(g_conn, in, 0, &g_acc);
    LG("ACC attach rc=%d", rc);
    if (rc) return -1;
    usbd_interface_descriptor(g_acc, (_uint8)in->config, (_uint8)in->iface, (_uint8)in->alternate, &root);
    for (i = 0; (d = usbd_parse_descriptors(g_acc, root, USB_DESC_ENDPOINT, i, &node)) != 0; ++i) {
        unsigned addr = d->endpoint.bEndpointAddress, attr = d->endpoint.bmAttributes;
        LG("ACC endpoint addr=0x%02x attr=0x%02x maxpkt=%u", addr, attr, (unsigned)d->endpoint.wMaxPacketSize);
        if ((attr & 3) != USB_ATTRIB_BULK) continue;
        if ((addr & USB_ENDPOINT_IN) && !g_pin) usbd_open_pipe(g_acc, d, &g_pin);
        else if (!(addr & USB_ENDPOINT_IN) && !g_pout) usbd_open_pipe(g_acc, d, &g_pout);
    }
    LG("ACC pipes in=%p out=%p", (void *)g_pin, (void *)g_pout);
    if (!g_pin || !g_pout) return -2;
    g_urb_in = usbd_alloc_urb(0); g_urb_out = usbd_alloc_urb(0);
    g_bin = usbd_alloc(16384); g_bout = usbd_alloc(16384 + 64);
    return (g_urb_in && g_urb_out && g_bin && g_bout) ? 0 : -3;
}

/* usbd_io() is asynchronous on this io-usb (car log: bulk OUT returned rc=0 with
 * URB status 0x00000000 = in progress). Completion is signalled through the
 * callback; we wait on a condvar. IN keeps one URB outstanding across polls so
 * a timeout never aborts a transfer that may already carry data. */
typedef struct { pthread_mutex_t m; pthread_cond_t c; volatile int done; } xw_t;
static xw_t g_wout = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0};
static xw_t g_win = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0};
static int g_in_pending = 0;
static unsigned g_rem_off, g_rem_len;

static void xfer_cb(struct usbd_urb *u, struct usbd_pipe *p, void *h)
{
    xw_t *w = (xw_t *)h;
    (void)u; (void)p;
    pthread_mutex_lock(&w->m);
    w->done = 1;
    pthread_cond_broadcast(&w->c);
    pthread_mutex_unlock(&w->m);
}
/* 1 = completed, 0 = timeout */
static int xw_wait(xw_t *w, int ms)
{
    struct timespec ts;
    int r = 0;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&w->m);
    while (!w->done && r == 0) r = pthread_cond_timedwait(&w->c, &w->m, &ts);
    r = w->done;
    pthread_mutex_unlock(&w->m);
    return r;
}

static int t_send(void *ctx, const unsigned char *b, int n)
{
    _uint32 st = 0, got = 0;
    int rc;
    (void)ctx;
    if (n > 16384 + 64) return -1;
    memcpy(g_bout, b, n);
    g_wout.done = 0;
    usbd_setup_bulk(g_urb_out, URB_DIR_OUT, g_bout, n);
    rc = usbd_io(g_urb_out, g_pout, xfer_cb, &g_wout, USBD_TIME_INFINITY);
    if (rc) { LG("USB bulk out submit rc=%d", rc); return -1; }
    if (!xw_wait(&g_wout, 5000)) {
        LG("USB bulk out timeout (phone not reading)");
        usbd_abort_pipe(g_pout);
        xw_wait(&g_wout, 500);
        return -1;
    }
    usbd_urb_status(g_urb_out, &st, &got);
    if ((st & USBD_URB_STATUS_MASK) != USBD_STATUS_CMP) { LG("USB bulk out st=0x%08x", (unsigned)st); return -1; }
    return (int)got;
}
static int t_recv(void *ctx, unsigned char *b, int n, int timeout_ms)
{
    _uint32 st = 0, got = 0;
    int rc;
    (void)ctx;
    if (g_rem_len) {   /* leftover from a previous completion larger than caller's room */
        int k = (int)g_rem_len < n ? (int)g_rem_len : n;
        memcpy(b, g_bin + g_rem_off, k); g_rem_off += k; g_rem_len -= k;
        return k;
    }
    if (!g_in_pending) {
        g_win.done = 0;
        usbd_setup_bulk(g_urb_in, URB_DIR_IN | URB_SHORT_XFER_OK, g_bin, 16384);
        rc = usbd_io(g_urb_in, g_pin, xfer_cb, &g_win, USBD_TIME_INFINITY);
        if (rc) { LG("USB bulk in submit rc=%d", rc); return -1; }
        g_in_pending = 1;
    }
    if (!xw_wait(&g_win, timeout_ms)) return 0;     /* still pending: keep it */
    g_in_pending = 0;
    usbd_urb_status(g_urb_in, &st, &got);
    if ((st & USBD_URB_STATUS_MASK) != USBD_STATUS_CMP) { LG("USB bulk in st=0x%08x", (unsigned)st); return -1; }
    if ((int)got > n) { g_rem_off = n; g_rem_len = got - n; got = n; }
    memcpy(b, g_bin, got);
    return (int)got;
}

int aa_usb_open(aa_log_fn lg, aa_status_fn st, int wait_s, aa_transport_t *t)
{
    usbd_funcs_t funcs = {_USBDI_NFUNCS, ins, rem, 0};
    usbd_device_ident_t any = {USBD_CONNECT_WILDCARD, USBD_CONNECT_WILDCARD, USBD_CONNECT_WILDCARD,
                               USBD_CONNECT_WILDCARD, USBD_CONNECT_WILDCARD};
    usbd_connect_parm_t cp;
    usbd_hcd_info_t hi;
    usbd_device_instance_t in;
    int rc, s, switched = 0;
    LG = lg;
    memset(&cp, 0, sizeof cp);
    cp.vusb = USB_VERSION; cp.vusbd = USBD_VERSION;
    cp.ident = &any; cp.funcs = &funcs; cp.connect_wait = USBD_CONNECT_WAIT;
    rc = usbd_connect(&cp, &g_conn);
    lg("USB usbd_connect rc=%d (%s)", rc, rc ? strerror(rc) : "ok");
    if (rc) return -1;
    memset(&hi, 0, sizeof hi);
    if (usbd_hcd_info(g_conn, &hi) == 0)
        lg("USB hcd ctrl=%.8s caps=0x%x ndev=%d", hi.controller, (unsigned)hi.capabilities, hi.ndev);
    sleep(2);   /* let existing devices be reported */
    for (s = 0; s < wait_s; ++s) {
        if (find_inst(1, &in)) {
            lg("USB accessory present vid=%04x pid=%04x", (unsigned)in.ident.vendor, (unsigned)in.ident.device);
            st("AA: accesorio AOA detectado");
            sleep(1);
            if (open_accessory(&in) == 0) {
                t->ctx = 0; t->send = t_send; t->recv = t_recv;
                return 0;
            }
            return -3;
        }
        if (!switched && find_inst(0, &in)) {
            st("AA: movil detectado, cambiando a AOA");
            rc = aoa_switch(&in);
            if (rc > 0) switched = 1;
            else if (rc == -2) { lg("USB device is not AOA capable (vid=%04x)", (unsigned)in.ident.vendor); }
        }
        if (s % 10 == 0) {
            char m[64];
            snprintf(m, sizeof m, "AA: conecta el movil (%d s)", wait_s - s);
            st(m);
        }
        sleep(1);
    }
    lg("USB no accessory after %d s", wait_s);
    return -2;
}

void aa_usb_close(void)
{
    if (g_in_pending && g_pin) { usbd_abort_pipe(g_pin); xw_wait(&g_win, 500); g_in_pending = 0; }
    if (g_pin) usbd_close_pipe(g_pin);
    if (g_pout) usbd_close_pipe(g_pout);
    if (g_acc) usbd_detach(g_acc);
    if (g_urb_in) usbd_free_urb(g_urb_in);
    if (g_urb_out) usbd_free_urb(g_urb_out);
    if (g_bin) usbd_free(g_bin);
    if (g_bout) usbd_free(g_bout);
    g_pin = g_pout = 0; g_acc = 0; g_urb_in = g_urb_out = 0; g_bin = g_bout = 0;
    if (g_conn) usbd_disconnect(g_conn);
    g_conn = 0;
    g_ninst = 0;
}
