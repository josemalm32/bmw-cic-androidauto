/* Carlinkit CPC200-CCPA ("Autokit") dongle backend.
 * The dongle does the MFi authentication, iAP2/AirPlay (CarPlay) or AAP (Android Auto)
 * and the Wi-Fi/Bluetooth link to the phone itself. Over USB bulk it speaks a small
 * framed protocol (documented by the MIT-licensed node-CarPlay project):
 *   header = magic 0x55AA55AA | length | type | ~type   (4 x u32 LE), then payload.
 * We get H.264 Annex-B (VideoData 0x06) and PCM (AudioData 0x07) and send heartbeats,
 * commands (0x08: buttons/knob, frame request) and touch (0x05). No microphone here. */
#ifndef CCPA_CORE_H
#define CCPA_CORE_H
#include "aa_core.h"

/* Runs the dongle session until the dongle is unplugged, the transport fails or the
 * plugin's Disconnect button. Returns highest stage reached:
 * 0 none, 1 dongle opened, 2 phone plugged, 3 video data. */
int ccpa_session(aa_transport_t *t, aa_log_fn lg, aa_status_fn st, char *summary, size_t summary_len);

#endif
