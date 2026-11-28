/* nbt_carplay: lockdown and the carkit service over the USB multiplexor.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 */
#ifndef NBT_CARPLAY_LOCKDOWN_H
#define NBT_CARPLAY_LOCKDOWN_H

struct usb;
struct stream;

/* multiplexor, pairing if needed ("Trust"), TLS session, com.apple.carkit.service (TLS): the stream for iAP2 */
int carkit_connect(struct usb* u, struct stream** out);
void carkit_close(void);
int stream_write(struct stream* s, const unsigned char* b, unsigned n);
int stream_read_some(struct stream* s, unsigned char* b, unsigned n, unsigned ms);   /* bytes, 0 = none, -1 */

#endif
