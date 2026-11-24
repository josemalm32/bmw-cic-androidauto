#ifndef AA_USB_H
#define AA_USB_H
#include "aa_core.h"
/* 0 = accessory bulk transport ready in *t; <0 = failure (see log) */
int aa_usb_open(aa_log_fn lg, aa_status_fn st, int wait_s, aa_transport_t *t);
/* same, but with want_ccpa also accepts a Carlinkit CPC200-CCPA dongle (1314:1520/1521):
 * returns 1 when the dongle's bulk pipes are open in *t, 0 for an AOA phone. */
int aa_usb_open2(aa_log_fn lg, aa_status_fn st, int wait_s, int want_ccpa, aa_transport_t *t);
void aa_usb_close(void);
#endif
