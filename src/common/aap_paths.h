/* Shared paths and switches. Every component (plugin, libcicaa, cicaalive,
 * dspload) uses only these. Nothing under AAP_DIR is written at runtime except
 * logs, and only while AAP_LOGS_ON exists (the plugin's "Logs" button). */
#ifndef AAP_PATHS_H
#define AAP_PATHS_H
#define AAP_VERSION   "1.0"
#define AAP_DIR       "/mnt/hbdebug/aap"            /* HDD: binaries + certificate (installer) */
#define AAP_LOGS_ON   AAP_DIR "/logs.on"            /* exists = logging enabled */
#define AAP_LOGDIR    AAP_DIR "/logs"               /* log files (only when enabled) */
#define AAP_LIB       AAP_DIR "/libcicaa.so"
#define AAP_LIVE      AAP_DIR "/cicaalive"
#define AAP_DSPLOAD   AAP_DIR "/dspload"
#define AAP_DSP_IMG   AAP_DIR "/dvd_dm642_aa.bin"   /* DSP image patched at install from the car's own copy */
#define AAP_DSP_ORIG  "/mnt/EFS_RO/DSP/DM642/dvd_dm642.bin"
#define AAP_STOP      "/dev/shmem/aap.stop"         /* RAM: asks cicaalive to stop */
#define AAP_SEED      "/dev/shmem/aap.seed"         /* RAM: entropy chaining (CIC has no /dev/random) */
/* Optional read-only overrides (never created by the plugin): */
#define AAP_GEOM_TXT  AAP_DIR "/geom.txt"           /* "sx sy sw sh dx dy dw dh" */
#define AAP_KEYMAP    AAP_DIR "/keymap.txt"         /* lines "cic_key android_key" */
#define AAP_VLEAD_TXT AAP_DIR "/vlead_ms.txt"       /* video lead in ms */
/* Video layout requested from the phone: 800x480 stream with these margins
 * (AA draws its UI in the remaining 640x320 area), shown 960x480 centred. */
#define AAP_MARGIN_W  160
#define AAP_MARGIN_H  160
#define AAP_GEOM      {80, 80, 640, 320, 160, 0, 960, 480}
#define AAP_VLEAD_MS  300
#endif
