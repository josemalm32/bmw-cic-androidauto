/* nbt_carplay: the iPhone on the car's USB port (QNX usbdi).
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 */
#ifndef NBT_CARPLAY_USB_H
#define NBT_CARPLAY_USB_H

#include <pthread.h>

struct usbd_connection;
struct usbd_device;
struct usbd_pipe;
struct usbd_urb;

/* one bulk pipe with its own transfer, buffer and completion (one thread at a time per pipe) */
struct usb_pipe {
    struct usbd_pipe* pipe;
    struct usbd_urb* urb;
    unsigned char* buf;
    unsigned size;
    int in, interrupt;
    pthread_mutex_t m;
    pthread_cond_t c;
    int done;
    int pending;   /* a transfer is with the USB stack (a read that timed out here stays there for the next call) */
    unsigned status, actual;
};

struct usb {
    struct usbd_connection* conn;
    struct usbd_device *mux_dev, *ncm_dev, *ncm_ctrl_dev;
    struct usb_pipe mux_in, mux_out, ncm_in, ncm_out, ncm_int;
    unsigned cfg, mux_ifc, ncm_ctrl, ncm_data, ncm_alt;
    unsigned ntb_in;   /* the largest NTB the iPhone sends us (our read size) */
    int have_int;      /* ncm_int: the NCM function's notifications */
    unsigned char mac[6];
    int have_mac;
};

int usb_open(struct usb* u, unsigned wait_s);   /* the iPhone in its CarPlay configuration, pipes open */
/* one transfer (the pipe's status in p->status): out = data sent; in = up to n bytes into data; *actual: bytes moved. 0 = done, ETIMEDOUT, other */
int usb_xfer(struct usb_pipe* p, void* data, unsigned n, unsigned ms, unsigned* actual);
int usb_gone(void);                             /* the iPhone was unplugged */
void usb_close(struct usb* u);

#endif
