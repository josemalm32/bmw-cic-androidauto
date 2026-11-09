#ifndef AA_USB_H
#define AA_USB_H
#include "aa_core.h"
/* 0 = accessory bulk transport ready in *t; <0 = failure (see log) */
int aa_usb_open(aa_log_fn lg, aa_status_fn st, int wait_s, aa_transport_t *t);
void aa_usb_close(void);
#endif
