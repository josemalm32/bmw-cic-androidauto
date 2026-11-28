/* nbt_carplay: the iPhone on the car's USB port (QNX usbdi), as nbt_carplay_probe found it on the car (2026-11-23).
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 *
 * 1. wait for an Apple device; attach to one of its interfaces (the stock iPod stack may hold others)
 * 2. if no configuration has both the USB multiplexor (ff/fe/02) and CDC NCM (02/0d): vendor request 0x52 (IN, value
 *    0, index 4, 1 byte); the iPhone re-connects with more configurations (iPhone 15 Pro Max: 6; the 6th = PTP +
 *    Apple Mobile Device + Apple USB Ethernet + NCM)
 * 3. select that configuration. The USB stack reported its interfaces only to the next connection on the car (the
 *    probe had to be run again): when they do not come within 3 s, connect to the USB stack again
 * 4. attach to the multiplexor's interface and open its pipes there (through another interface's attachment every
 *    transfer failed with ENOENT); attach to the first NCM function's data interface, select its alternate setting 1
 *    and open its bulk pipes; the MAC address: the NCM function's iMACAddress string
 * Transfers wait for their completion callback (usbd_io returns at once). An unplug is seen by the removal callback.
 *
 * On CIC, a busy stock iPod interface stays with its owner. Retry briefly, then
 * ask the operator to disconnect and reconnect the iPhone.
 */
#include <sys/usbdi.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "cp.h"
#include "cp_usb.h"

#define APPLE 0x05ac

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static usbd_device_instance_t g_inst[64];
static unsigned g_count;
static unsigned g_devno = 0xffff, g_path = 0xffff;
static volatile int g_gone;
static volatile int g_armed;   /* the iPhone is set up: from now on its removal is an unplug */
static int g_quiet;            /* attach failures not logged (while trying again) */

static void inserted(struct usbd_connection* c, usbd_device_instance_t* d)
{
    (void)c;
    pthread_mutex_lock(&g_m);
    if (g_count < sizeof(g_inst) / sizeof(g_inst[0]))
        g_inst[g_count++] = *d;
    pthread_mutex_unlock(&g_m);
    say("usb: %04x:%04x device %u configuration %u interface %u", (unsigned)d->ident.vendor,
        (unsigned)d->ident.device, (unsigned)d->devno, (unsigned)d->config, (unsigned)d->iface);
}

static void removed(struct usbd_connection* c, usbd_device_instance_t* d)
{
    (void)c;
    /* not while setting it up: after the CarPlay request and the configuration change it re-connects (car
     * 2026-11-23: that removal was taken as an unplug and everything stopped) */
    if (g_armed && d->devno == g_devno && d->path == g_path && !g_gone) {
        g_gone = 1;
        say("usb: the iPhone was unplugged");
    }
}

int usb_gone(void)
{
    return g_gone;
}

static unsigned count(void)
{
    unsigned n;
    pthread_mutex_lock(&g_m);
    n = g_count;
    pthread_mutex_unlock(&g_m);
    return n;
}

static int connect_stack(struct usb* u)
{
    usbd_connect_parm_t parm;
    static usbd_funcs_t funcs;
    static usbd_device_ident_t ident;
    int rc;
    memset(&parm, 0, sizeof(parm));
    memset(&funcs, 0, sizeof(funcs));
    ident.vendor = APPLE;
    ident.device = ident.dclass = ident.subclass = ident.protocol = USBD_CONNECT_WILDCARD;
    funcs.nentries = _USBDI_NFUNCS;
    funcs.insertion = inserted;
    funcs.removal = removed;
    parm.vusb = USB_VERSION;
    parm.vusbd = USBD_VERSION;
    parm.ident = &ident;
    parm.funcs = &funcs;
    parm.connect_wait = 1;
    pthread_mutex_lock(&g_m);
    g_count = 0;
    pthread_mutex_unlock(&g_m);
    rc = usbd_connect(&parm, &u->conn);
    if (rc != EOK) {
        say("usb: connection to the USB stack failed: %s", strerror(rc));
        u->conn = 0;
        return -1;
    }
    return 0;
}

/* more than `after` instances within `seconds`, then 1 s for the rest */
static int wait_for(unsigned after, unsigned seconds)
{
    unsigned t;
    for (t = 0; t < seconds * 10 && !g_stop; ++t) {
        if (count() > after) {
            sleep(1);
            return 0;
        }
        usleep(100000);
    }
    return -1;
}

/* the first instance (from `first` on) matching cfg / ifc (any: ~0u) we can attach to */
static struct usbd_device* attach(struct usb* u, unsigned first, unsigned cfg, unsigned ifc, usbd_device_instance_t* used)
{
    unsigned i;
    const unsigned n = count();
    for (i = first; i < n; ++i) {
        struct usbd_device* dev = 0;
        usbd_device_instance_t inst;
        int rc;
        pthread_mutex_lock(&g_m);
        inst = g_inst[i];
        pthread_mutex_unlock(&g_m);
        if ((cfg != ~0u && inst.config != cfg) || (ifc != ~0u && inst.iface != ifc))
            continue;
        rc = usbd_attach(u->conn, &inst, 0, &dev);
        if (rc == EOK) {
            if (used)
                *used = inst;
            g_devno = inst.devno;
            g_path = inst.path;
            return dev;
        }
        if (!g_quiet)
            say("usb: attach to configuration %u interface %u: %s", (unsigned)inst.config, (unsigned)inst.iface,
            strerror(rc));
    }
    return 0;
}

static int carplay_request(struct usbd_device* dev)
{
    struct usbd_desc_node* node;
    usbd_descriptors_t* d = usbd_parse_descriptors(dev, 0, USB_DESC_DEVICE, 0, &node);
    struct usbd_pipe* pipe = 0;
    struct usbd_urb* urb = usbd_alloc_urb(0);
    unsigned char* buf = (unsigned char*)usbd_alloc(8);
    _Uint32t status = 0, actual = 0;
    int rc = -1;
    if (!d || !urb || !buf || usbd_open_pipe(dev, d, &pipe) != EOK) {
        say("usb: no control pipe");
        goto out;
    }
    memset(buf, 0, 8);
    usbd_setup_vendor(urb, URB_DIR_IN | URB_SHORT_XFER_OK, 0x52, USB_TYPE_VENDOR | USB_RECIPIENT_DEVICE | USB_DIRECTION_HOST,
                      0, 4, buf, 1);
    rc = usbd_io(urb, pipe, 0, 0, 1000);
    usbd_urb_status(urb, &status, &actual);
    say("usb: CarPlay request (vendor 0x52): result %d, %u byte(s) back", rc, (unsigned)actual);
out:
    if (pipe)
        usbd_close_pipe(pipe);
    if (urb)
        usbd_free_urb(urb);
    if (buf)
        usbd_free(buf);
    return rc;
}

/* the highest configuration with the multiplexor and NCM: its number, the multiplexor's interface, the first NCM
 * function's control and data interfaces */
static int find_carplay(struct usb* u, struct usbd_device* dev)
{
    struct usbd_desc_node* node;
    unsigned cfg, ifc, found = 0;
    for (cfg = 1; cfg <= 16; ++cfg) {
        unsigned mux = 99, ctrl = 99, data = 99;
        if (!usbd_configuration_descriptor(dev, (unsigned char)cfg, &node))
            continue;
        for (ifc = 0; ifc < 16; ++ifc) {
            usbd_interface_descriptor_t* i = usbd_interface_descriptor(dev, (unsigned char)cfg, (unsigned char)ifc, 0, &node);
            if (!i)
                continue;
            if (i->bInterfaceClass == 0xff && i->bInterfaceSubClass == 0xfe && i->bInterfaceProtocol == 0x02)
                mux = ifc;
            if (i->bInterfaceClass == 0x02 && i->bInterfaceSubClass == 0x0d && ctrl == 99)
                ctrl = ifc;
            if (i->bInterfaceClass == 0x0a && ctrl != 99 && data == 99 && ifc == ctrl + 1)
                data = ifc;
        }
        if (mux != 99 && ctrl != 99 && data != 99) {
            u->cfg = cfg;
            u->mux_ifc = mux;
            u->ncm_ctrl = ctrl;
            u->ncm_data = data;
            found = 1;
        }
    }
    return found ? 0 : -1;
}

/* the bulk endpoints of an interface's alternate setting into p_in / p_out */
static int open_bulk(struct usbd_device* dev, unsigned cfg, unsigned ifc, unsigned alt, struct usb_pipe* p_in,
                     struct usb_pipe* p_out, unsigned size)
{
    struct usbd_desc_node *in_node, *ep_node;
    usbd_descriptors_t *e, *bin = 0, *bout = 0;
    int k, rc;
    if (!usbd_interface_descriptor(dev, (unsigned char)cfg, (unsigned char)ifc, (unsigned char)alt, &in_node))
        return -1;
    for (k = 0; (e = usbd_parse_descriptors(dev, in_node, USB_DESC_ENDPOINT, k, &ep_node)) != 0; ++k)
        if ((e->endpoint.bmAttributes & 3) == 2) {
            if (e->endpoint.bEndpointAddress & 0x80)
                bin = e;
            else
                bout = e;
        }
    if (!bin || !bout) {
        say("usb: interface %u alternate %u has no bulk pipes", ifc, alt);
        return -1;
    }
    rc = usbd_open_pipe(dev, bin, &p_in->pipe);
    if (rc == EOK)
        rc = usbd_open_pipe(dev, bout, &p_out->pipe);
    say("usb: interface %u pipes %02x in / %02x out: %s", ifc, bin->endpoint.bEndpointAddress,
        bout->endpoint.bEndpointAddress, rc == EOK ? "open" : strerror(rc));
    if (rc != EOK)
        return -1;
    p_in->in = 1;
    p_out->in = 0;
    p_in->size = p_out->size = size;
    p_in->urb = usbd_alloc_urb(0);
    p_out->urb = usbd_alloc_urb(0);
    p_in->buf = (unsigned char*)usbd_alloc(size);
    p_out->buf = (unsigned char*)usbd_alloc(size);
    pthread_mutex_init(&p_in->m, 0);
    pthread_cond_init(&p_in->c, 0);
    pthread_mutex_init(&p_out->m, 0);
    pthread_cond_init(&p_out->c, 0);
    return p_in->urb && p_out->urb && p_in->buf && p_out->buf ? 0 : -1;
}

/* a class request to the NCM control interface (bmRequestType a1 in / 21 out) on the default pipe: bytes moved, -1 */
static int ncm_request(struct usbd_device* dev, int in, unsigned request, unsigned value, unsigned index,
                       unsigned char* data, unsigned n)
{
    struct usbd_desc_node* node;
    usbd_descriptors_t* d = usbd_parse_descriptors(dev, 0, USB_DESC_DEVICE, 0, &node);
    struct usbd_pipe* pipe = 0;
    struct usbd_urb* urb = usbd_alloc_urb(0);
    unsigned char* buf = (unsigned char*)usbd_alloc(n ? n : 1);
    _Uint32t status = 0, actual = 0;
    int rc = -1;
    if (!d || !urb || !buf || usbd_open_pipe(dev, d, &pipe) != EOK)
        goto out;
    if (!in && n)
        memcpy(buf, data, n);
    usbd_setup_vendor(urb, in ? URB_DIR_IN | URB_SHORT_XFER_OK : URB_DIR_OUT, (unsigned short)request,
                      in ? 0xa1 : 0x21, (unsigned short)value, (unsigned short)index, buf, n);
    rc = usbd_io(urb, pipe, 0, 0, 1000);
    usbd_urb_status(urb, &status, &actual);
    if (rc == EOK) {
        if (in)
            memcpy(data, buf, actual);
        rc = (int)actual;
    } else {
        say("usb: NCM request %02x: %s, status %08x", request, strerror(rc), (unsigned)status);
        rc = -1;
    }
out:
    if (pipe)
        usbd_close_pipe(pipe);
    if (urb)
        usbd_free_urb(urb);
    if (buf)
        usbd_free(buf);
    return rc;
}

/* NTB parameters (GET_NTB_PARAMETERS 80) logged; the largest NTB the iPhone sends set to 16384 (SET_NTB_INPUT_SIZE
 * 86), the read size known to work on the multiplexor's pipes. Before the data interface's alternate setting 1, as
 * Linux's cdc_ncm. */
static void ncm_parameters(struct usb* u, struct usbd_device* dev)
{
    unsigned char p[28], v[4];
    unsigned in_max = 0;
    const int n = ncm_request(dev, 1, 0x80, 0, u->ncm_ctrl, p, sizeof(p));
    u->ntb_in = 16384;
    if (n >= 28) {
        in_max = get32le(p + 4);
        say("usb: NCM formats %04x, in: max %u, divisor %u, remainder %u, alignment %u; out: max %u, divisor %u, "
            "remainder %u, alignment %u, %u datagrams",
            get16le(p + 2), in_max, get16le(p + 8), get16le(p + 10), get16le(p + 12), get32le(p + 16),
            get16le(p + 20), get16le(p + 22), get16le(p + 24), get16le(p + 26));
    } else
        say("usb: no NCM parameters (%d bytes)", n);
    /* Car 2026-11-24: this iPhone sent blocks "ncmx" / "ncxr" (no NTB16, no NTB32) when no format was chosen. Linux's
     * cdc_ncm chooses the 16 bit format (SET_NTB_FORMAT 84, value 0) whenever the device also offers 32 bit. */
    if (n >= 28 && (get16le(p + 2) & 2))
        say("usb: NCM 16 bit format: %s", ncm_request(dev, 0, 0x84, 0, u->ncm_ctrl, v, 0) >= 0 ? "set" : "not set");
    if (in_max && in_max < u->ntb_in)
        u->ntb_in = in_max;
    put32le(v, u->ntb_in);
    say("usb: NCM input size %u: %s", u->ntb_in, ncm_request(dev, 0, 0x86, 0, u->ncm_ctrl, v, 4) >= 0 ? "set" : "not set");
}

/* the NCM control interface's interrupt endpoint (notifications), if it can be had: only read and logged */
static void ncm_notifications(struct usb* u)
{
    struct usbd_desc_node *in_node, *ep_node;
    usbd_descriptors_t* e;
    int k, rc;
    u->ncm_ctrl_dev = attach(u, 0, u->cfg, u->ncm_ctrl, 0);
    if (!u->ncm_ctrl_dev ||
        !usbd_interface_descriptor(u->ncm_ctrl_dev, (unsigned char)u->cfg, (unsigned char)u->ncm_ctrl, 0, &in_node))
        return;
    for (k = 0; (e = usbd_parse_descriptors(u->ncm_ctrl_dev, in_node, USB_DESC_ENDPOINT, k, &ep_node)) != 0; ++k)
        if ((e->endpoint.bmAttributes & 3) == 3 && (e->endpoint.bEndpointAddress & 0x80))
            break;
    if (!e)
        return;
    rc = usbd_open_pipe(u->ncm_ctrl_dev, e, &u->ncm_int.pipe);
    say("usb: NCM notifications %02x: %s", e->endpoint.bEndpointAddress, rc == EOK ? "open" : strerror(rc));
    if (rc != EOK)
        return;
    u->ncm_int.in = u->ncm_int.interrupt = 1;
    u->ncm_int.size = 64;
    u->ncm_int.urb = usbd_alloc_urb(0);
    u->ncm_int.buf = (unsigned char*)usbd_alloc(64);
    pthread_mutex_init(&u->ncm_int.m, 0);
    pthread_cond_init(&u->ncm_int.c, 0);
    u->have_int = u->ncm_int.urb && u->ncm_int.buf;
}

/* the NCM function's MAC address: the CDC Ethernet functional descriptor (24 / 0F) of its control interface names a
 * string of 12 hex digits */
static void read_mac(struct usb* u, struct usbd_device* dev)
{
    struct usbd_desc_node *ifc_node, *node;
    usbd_descriptors_t* d;
    int k;
    if (!usbd_interface_descriptor(dev, (unsigned char)u->cfg, (unsigned char)u->ncm_ctrl, 0, &ifc_node))
        return;
    for (k = 0; (d = usbd_parse_descriptors(dev, ifc_node, 0x24, k, &node)) != 0; ++k) {
        const unsigned char* b = (const unsigned char*)d;
        const char* s;
        unsigned i, v;
        if (b[0] < 4 || b[2] != 0x0f || !b[3])
            continue;
        s = usbd_string(dev, b[3], 0);
        if (!s || strlen(s) < 12)
            break;
        for (i = 0; i < 6; ++i) {
            if (sscanf(s + 2 * i, "%2x", &v) != 1)
                return;
            u->mac[i] = (unsigned char)v;
        }
        u->have_mac = 1;
        say("usb: NCM MAC address %s", s);
        return;
    }
    say("usb: no NCM MAC address in the descriptors");
}

int usb_open(struct usb* u, unsigned wait_s)
{
    struct usbd_device* dev;
    usbd_device_instance_t used;
    unsigned before, tries;
    int rc;
    memset(u, 0, sizeof(*u));
    if (connect_stack(u))
        return -1;
    say("usb: waiting up to %u s for an iPhone", wait_s);
    if (wait_for(0, wait_s)) {
        say("usb: no iPhone");
        return -1;
    }
    dev = attach(u, 0, ~0u, ~0u, &used);
    if (!dev) {
        g_quiet = 1;
        for (tries = 0; tries < 30 && !g_stop && !dev; ++tries) {
            usleep(100000);
            dev = attach(u, 0, ~0u, ~0u, &used);
        }
        g_quiet = 0;
        if (!dev) {
            say("usb: iPhone interfaces unavailable; unplug the phone and retry with iPod playback stopped");
            return -1;
        }
    }
    if (find_carplay(u, dev)) {   /* not yet: the CarPlay configurations */
        before = count();
        carplay_request(dev);
        usbd_detach(dev);
        if (wait_for(before, 15) || !(dev = attach(u, before, ~0u, ~0u, &used)) || find_carplay(u, dev)) {
            say("usb: no CarPlay configuration (multiplexor + NCM) after the request");
            return -1;
        }
    }
    say("usb: CarPlay configuration %u: multiplexor interface %u, NCM interfaces %u / %u", u->cfg, u->mux_ifc,
        u->ncm_ctrl, u->ncm_data);
    read_mac(u, dev);
    if (used.config != u->cfg) {
        before = count();
        rc = usbd_select_config(dev, (unsigned char)u->cfg);   /* dev stays attached, as in the probe */
        say("usb: select configuration %u: %s", u->cfg, rc == EOK ? "OK" : strerror(rc));
        if (wait_for(before, 3)) {   /* not reported to this connection: a new one */
            say("usb: the new interfaces were not reported: connecting to the USB stack again");
            usbd_disconnect(u->conn);
            u->conn = 0;
            dev = 0;
            if (connect_stack(u) || wait_for(0, 5))
                return -1;
        }
    }
    /* the first attachment is kept; if it is already one of the interfaces we need, it is used for it */
    if (dev && used.config == u->cfg && used.iface == u->mux_ifc)
        u->mux_dev = dev;
    else if (dev && used.config == u->cfg && used.iface == u->ncm_data)
        u->ncm_dev = dev;
    for (tries = 0; tries < 30 && !u->mux_dev && !(u->mux_dev = attach(u, 0, u->cfg, u->mux_ifc, 0)); ++tries)
        usleep(200000);
    if (!u->mux_dev) {
        say("usb: the multiplexor interface %u is not there", u->mux_ifc);
        return -1;
    }
    if (open_bulk(u->mux_dev, u->cfg, u->mux_ifc, 0, &u->mux_in, &u->mux_out, 16384))
        return -1;
    for (tries = 0; tries < 30 && !u->ncm_dev && !(u->ncm_dev = attach(u, 0, u->cfg, u->ncm_data, 0)); ++tries)
        usleep(200000);
    if (!u->ncm_dev) {
        say("usb: the NCM data interface %u is not there (held by another driver?)", u->ncm_data);
        return -1;
    }
    ncm_parameters(u, u->mux_dev);
    ncm_notifications(u);
    u->ncm_alt = 1;
    rc = usbd_select_interface(u->ncm_dev, (unsigned char)u->ncm_data, (unsigned char)u->ncm_alt);
    say("usb: NCM data interface %u alternate %u: %s", u->ncm_data, u->ncm_alt, rc == EOK ? "OK" : strerror(rc));
    if (rc != EOK || open_bulk(u->ncm_dev, u->cfg, u->ncm_data, u->ncm_alt, &u->ncm_in, &u->ncm_out, u->ntb_in + 2048))
        return -1;
    if (!u->have_mac) {   /* our own, locally administered */
        cp_random(u->mac, 6);
        u->mac[0] = (unsigned char)((u->mac[0] & 0xfe) | 0x02);
    }
    g_armed = 1;
    return 0;
}

static void done(struct usbd_urb* urb, struct usbd_pipe* pipe, void* arg)
{
    struct usb_pipe* p = (struct usb_pipe*)arg;
    _Uint32t status = 0, actual = 0;
    (void)pipe;
    usbd_urb_status(urb, &status, &actual);
    pthread_mutex_lock(&p->m);
    p->status = (unsigned)status;
    p->actual = (unsigned)actual;
    p->done = 1;
    pthread_cond_broadcast(&p->c);
    pthread_mutex_unlock(&p->m);
}

static int wait_done(struct usb_pipe* p, unsigned ms)
{
    struct timespec t;
    int r = 0, d;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&p->m);
    while (!p->done && (r == 0 || r == EINTR))
        r = pthread_cond_timedwait(&p->c, &p->m, &t);
    d = p->done;
    pthread_mutex_unlock(&p->m);
    return d;
}

/* Transfers are given to the USB stack WITHOUT a time limit, and the waiting is done here. With a limit the stack
 * (io-usb) keeps a timer inside the transfer's memory; when this program ended while such transfers were still
 * running (three readers with 1 s limits, always), the stack at times kept the timer of a transfer whose memory it
 * had already released and crashed on its next timer tick - and with it the whole head unit restarted (car
 * 2026-11-26 17:32: io-usb core, SIGSEGV in its timer list on an unmapped transfer page). So: no timers in the
 * stack; a read that brings nothing in time stays with the stack and the next call goes on waiting for it; only a
 * write that does not finish is aborted; usb_close() ends everything that is still running before it disconnects. */
static volatile int g_closing;

int usb_xfer(struct usb_pipe* p, void* data, unsigned n, unsigned ms, unsigned* actual)
{
    int rc = EOK;
    *actual = 0;
    if (n > p->size || g_closing || (g_gone && !p->pending))
        return EINVAL;
    if (!p->pending) {
        if (!p->in)
            memcpy(p->buf, data, n);
        p->done = 0;
        if (p->interrupt)
            usbd_setup_interrupt(p->urb, URB_DIR_IN | URB_SHORT_XFER_OK, p->buf, n);
        else
            usbd_setup_bulk(p->urb, p->in ? URB_DIR_IN | URB_SHORT_XFER_OK : URB_DIR_OUT, p->buf, n);
        rc = usbd_io(p->urb, p->pipe, done, p, USBD_TIME_INFINITY);
        if (rc != EOK)
            return rc;
        p->pending = 1;
    }
    if (!wait_done(p, ms)) {
        if (p->in)
            return ETIMEDOUT;   /* still with the stack: the next call waits for the same transfer */
        usbd_abort_pipe(p->pipe);
        if (!wait_done(p, 2000)) {   /* the stack still owns the buffer: no further transfer on this pipe */
            say("usb: a transfer did not end after its abort");
            return EIO;
        }
        rc = ETIMEDOUT;
    }
    p->pending = 0;
    *actual = p->actual;
    if (p->in && p->actual)
        memcpy(data, p->buf, p->actual);
    if (rc == EOK && p->status && !p->actual && p->in) {
        rc = ETIMEDOUT;   /* a read that ends with a status and nothing in it: as "nothing came" */
        if (p->status & 0x0a000000u)   /* ended with an error or aborted: the callers try again at once */
            usleep(50000);
    }
    return rc;
}

void usb_close(struct usb* u)
{
    struct usb_pipe* all[5];
    unsigned i, round, left = 0;
    all[0] = &u->mux_in;
    all[1] = &u->mux_out;
    all[2] = &u->ncm_in;
    all[3] = &u->ncm_out;
    all[4] = &u->ncm_int;
    g_closing = 1;   /* no new transfers */
    /* what is still with the stack is ended and waited for BEFORE the connection goes (see usb_xfer); twice, for a
     * reader that was just handing a transfer over */
    for (round = 0; round < 2 && u->conn; ++round) {
        left = 0;
        for (i = 0; i < 5; ++i) {
            struct usb_pipe* p = all[i];
            if (!p->pipe || !p->pending || p->done)
                continue;
            usbd_abort_pipe(p->pipe);
            if (!wait_done(p, 2000))
                ++left;
        }
        usleep(100000);
    }
    if (left)   /* without timers in the stack nothing of ours is left behind in it either way */
        say("usb: %u transfer(s) did not end before the disconnect", left);
    if (u->conn)
        usbd_disconnect(u->conn);
    u->conn = 0;
}
