/* Shared paths and switches. Every component (plugin, libcicaa, cicaalive,
 * dspload) uses only these. Nothing under AAP_DIR is written at runtime except
 * logs (only while AAP_LOGS_ON exists, "Logging" button) and the small mode files of
 * the plugin's buttons (zoom.on / wide.on, later aaaudio.on). */
#ifndef AAP_PATHS_H
#define AAP_PATHS_H
#define AAP_VERSION   "1.1.1"
#define AAP_DIR       "/mnt/hbdebug/aap"            /* HDD: binaries + certificate (installer) */
#define AAP_LOGS_ON   AAP_DIR "/logs.on"            /* exists = logging enabled */
#define AAP_LOGDIR    AAP_DIR "/logs"               /* log files (only when enabled) */
#define AAP_ZOOM_ON   AAP_DIR "/zoom.on"            /* screen mode Fullscreen (Zoom Mode button) */
#define AAP_WIDE_ON   AAP_DIR "/wide.on"            /* screen mode Stretched */
#define AAP_RIGHT_ON  AAP_DIR "/right.on"           /* screen mode Right: 960 px picture at the right edge */
#define AAP_FULL_ON   AAP_DIR "/full.on"            /* screen mode Full screen 8:3 (writes video.txt + geom.txt) */
#define AAP_FULL_TXT  AAP_DIR "/fullscreen.txt"     /* optional "margin_w margin_h dpi" for Full screen */
#define AAP_NOAUTO_ON AAP_DIR "/noauto.on"          /* exists = no autoconnect */
#define AAP_LANG_TXT  AAP_DIR "/lang.txt"           /* "de" = German status line */
#define AAP_AAUDIO_ON AAP_DIR "/aaaudio.on"         /* exists = AA audio over USB; missing = BT audio */
#define AAP_LIB       AAP_DIR "/libcicaa.so"
#define AAP_LIVE      AAP_DIR "/cicaalive"
#define AAP_DSPLOAD   AAP_DIR "/dspload"
#define AAP_DSP_IMG   AAP_DIR "/dvd_dm642_aa.bin"   /* DSP image patched at install from the car's own copy */
#define AAP_DSP_ORIG  "/mnt/EFS_RO/DSP/DM642/dvd_dm642.bin"
#define AAP_STOP      "/dev/shmem/aap.stop"         /* RAM: asks cicaalive to stop */
#define AAP_SEED      "/dev/shmem/aap.seed"         /* RAM: entropy chaining (CIC has no /dev/random) */
/* Optional read-only overrides (never created by the plugin): */
#define AAP_KEYMAP    AAP_DIR "/keymap.txt"         /* lines "cic_key android_key" */
#define AAP_VLEAD_TXT AAP_DIR "/vlead_ms.txt"       /* video lead in ms */
/* Video layout requested from the phone: 800x480 stream, AA draws its UI in a 720x360
 * area. width/height margin shrink the UI area, ui_config.margins (bottom/right) place
 * it in the top-left corner, so the plugin crops exactly (0,0,720,360).
 * Why 720 wide: the DM642 DVD decoder outputs at most 720 columns; with 800 columns the
 * right 80 were cut off (and full 800-px decoding needs more DSP patches, tested: heavy
 * white/black line noise). 720x360 + the AA status bar at the side looked best in the
 * car (sharp, no letterbox); 640x320/640x400 variants were less sharp.
 * The source rectangle is scaled 4:3 horizontally to 960x480 so the picture is not
 * distorted on the 1280x480 (wide pixel) CIC display. */
#define AAP_MARGIN_W  80
#define AAP_MARGIN_H  120
#define AAP_DPI       140
#define AAP_UI_MARGIN_BOTTOM 120
#define AAP_UI_MARGIN_RIGHT  80
/* sx sy sw sh (crop in the 800x480 stream), dx dy dw dh (on the 1280x480 display).
 * x = 256 leaves the CIC's left arc/menu column visible and centres the picture. */
#define AAP_GEOM_NORMAL     {0, 0, 720, 360, 160, 0,  960, 480}   /* centred, CIC menu hidden, black sides */
#define AAP_GEOM_LEFT       {0, 0, 720, 360,   0, 0,  960, 480}   /* same size at the left edge */
#define AAP_GEOM_RIGHT      {0, 0, 720, 360, 320, 0,  960, 480}   /* same size at the right edge */
#define AAP_GEOM_STRETCHED  {0, 0, 720, 360,   0, 0, 1280, 480}   /* full width, distorted */
#define AAP_GEOM_FULL       {0, 0, 720, 270,   0, 0, 1280, 480}   /* 8:3 area asked from the phone (video.txt), full screen */
#define AAP_FULL_MARGINS    80, 210, 116                          /* -> 720 x 270 top-left, dpi 116 */
/* Video lead: PTS = arrival + lead. Car tests: 50 ms gives clearly the most direct
 * response; 150/300 ms only add latency (the DSP shows frames in its own clock anyway).
 * vlead_ms.txt can still raise it. */
#define AAP_VLEAD_MS  50
/* cicaa_input(kind, code, value) kinds (plugin -> libcicaa queue) */
#define CICAA_IN_KEY      1   /* code = Android keycode, value 1 down / 0 up */
#define CICAA_IN_REL      2   /* rotary: code 65536, value = signed steps */
#define CICAA_IN_BYEBYE   7   /* Disconnect button: end the AA session properly */
#define CICAA_IN_LOCATION 9   /* code = latitude, value = longitude, CIC units (2^32 = 360 deg) */
#define AAP_GEOM_TXT  AAP_DIR "/geom.txt"           /* "sx sy sw sh dx dy dw dh": overrides the built-in geometry */
#define AAP_VIDEO_TXT AAP_DIR "/video.txt"          /* "margin_w margin_h [dpi]" asked from the phone */
#define AAP_DRIVER_TXT AAP_DIR "/driver.txt"        /* "left" (default) or "right": driver seat */
#define AAP_GPS_TXT   AAP_DIR "/gps.txt"            /* "off": do not send the CIC position to the phone */
#endif
