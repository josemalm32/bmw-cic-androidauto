# Arquitectura

```
 móvil ──USB──> libcicaa.so (en HmiMain, hilo propio)
                 AOA -> versión AA -> TLS 1.2 -> service discovery
                 vídeo H.264 ──> shm /cicaa_live  (avcC + frames con longitud)
                 audio AAC/PCM ─> shm /cicaa_audio
                 entrada <── cicaa_input() (iDrive desde el plugin)
                                   │
 AAPlugin.so (plugin HMI) ─────────┤ abre la ruta nativa de vídeo y lanza
                                   v
                 cicaalive (proceso) ──dspipc──> DSP TI DM642 ──> capa de vídeo 26
                                                       └──> audio del coche (fuente C4)
```

## Plugin HMI (`src/plugin`)

* Lo carga HmiMain al arrancar desde `Plugins/AAP` (nombre de carpeta de 3 letras:
  el escáner de plugins del CIC solo guarda 4 bytes del nombre del directorio).
* Usa el `PluginApp` del firmware (`libAppFramework`) con handlers nativos
  (`raAction` → `Delegate{object, stub}`) y `updateModel` para el texto.
* **Ruta de vídeo nativa** (la misma que usa el reproductor de DVD de BMW), siempre
  en el hilo ProcUnit de HMI, al que se salta por el ejecutor de HMI:
  1. audio: conexión C4 (el audio del DM642 sale por los altavoces);
  2. vídeo: conexión fuente 8 → sink 1, `SetSource(8)`, `StartVideo`;
  3. lanza `cicaalive`, aplica la geometría (recorte de entrada / rectángulo de salida);
  4. capa lógica 26 opaca + `LayerToolkit` enable;
  5. la capa del menú (cid 256) se pone con alfa 0 para que HMI componga solo el vídeo.
  Al parar se deshace en orden inverso y se restaura la fuente y los rectángulos.
* Antes de llamar a cualquier dirección de HmiMain se comparan 32 bytes de código
  (`iba_firmware.h`, `iba_marshal.h`). Con otro firmware no se toca nada.
* **iDrive**: cuando hay vídeo se abre una página `terminalUI` (210), que recibe las
  teclas: 13/14 ruleta → `65536` relativo; 8 pulsar → `DPAD_CENTER`; 9..12
  direcciones → `DPAD_*`; 2 → `BACK`. Si se pierde el foco (MENU) o se pulsa
  Volver, se quita la imagen pero AA sigue conectado.

## libcicaa (`src/libcicaa`)

* io-usb → modo accesorio AOA → endpoints bulk → protocolo AA.
* TLS 1.2 con mbed TLS 3.6.7 (configuración reducida `cicaa_config.h`). El CIC no
  tiene `/dev/random`: la entropía sale del jitter de temporizadores, encadenada
  con una semilla en RAM (`/dev/shmem/aap.seed`).
* Service discovery: vídeo 800x480 a 30 fps con márgenes 160/160 (AA dibuja su UI
  en 640x320), dpi 140, sin pantalla táctil (interfaz de mando); audio media en
  AAC-LC 48 kHz y voz/sistema en PCM 16 kHz; micrófono anunciado, pero no implementado.
* Pide un IDR al arrancar el lector (cambio de foco de vídeo).

## cicaalive (`src/cicaalive`)

Escritor propio para el DSP, compatible con lo que hace `writer_dm642.so` de BMW:
`dspipc_create("dm642.0")`, memoria compartida de 0x840c, canales de comando
0x15/0x14 y de stream 0x20, paquetes 'MPEG' de 0x8400 bytes con entradas MRAE y
uno en vuelo cada vez.

* Vídeo `AVC1` (cabecera avcC + frames con prefijo de 4 bytes), PTS = llegada −
  inicio de reloj + `vlead` (500 ms).
* Audio `M4AC`: las AU AAC del móvil. El DSP solo arranca el reloj con
  «Audio Pipe ES buffer is Full», así que se rellena con AAC en silencio cuando
  el móvil no manda audio. Hay un colchón de ~0,35 s contra las ráfagas del USB.
* **Modo experimental sin PTS** (`vlead_ms.txt` = `nopts` o `pts-1`). En el DSP,
  `avSyncShowFrame` (≈0x81f19ad0) compara el PTS del frame con -1: si no tiene, va
  por «Show Vid no pts» y se muestra al decodificarse, sin comprobar retraso ni
  descartar («skip frame at output», 0x81f205cc). AA no necesita sincronía A/V.
  Sin probar en el coche.
* Si el DSP informa de un error de stream, reenvía la cabecera y espera un IDR.

## Parche DSP (`src/dspload`)

El decodificador H.264 del DM642 (`dvd_dm642.bin`, C64x) está limitado a 720x576.
Dos instrucciones `mvk` cambiadas:

| Offset (fichero) | Dirección C64x | Antes → después | Qué es |
|---|---|---|---|
| 0x094518 | 0x81f147e4 | `mvk 0x2d0,a4` → `mvk 0x320,a4` | `vddSetupDecoder`: ancho ≤ 720 → 800 |
| 0x1ddcfc | 0x819527ec | `mvk 0x2e,a5` → `mvk 0x33,a5` | SPS: `pic_width_in_mbs` < 46 → 51 |

`dspload patch` lo genera en el coche desde la imagen de la flash, comprobando el
tamaño y las palabras originales. `dspload on` crea un enlace de prefijo de
procnto (`pathmgr_symlink`) de la imagen original a la parcheada y reinicia el
DSP (`devctl 0x8004050c`); `dev-dspipc` recarga el firmware desde su ruta. El
enlace vive en RAM: al reiniciar el CIC vuelve la imagen original.

Otros límites del DSP, que no se tocan (por eso no hay pantalla completa a 1280):

| Dirección C64x | Qué es | Efecto |
|---|---|---|
| 0x81952814 | SPS: `mvk 0x654` | total de macrobloques ≤ 1620 (área de un PAL). 800x480 = 1500 cabe; 1280x720 = 3600 no |
| 0x81f148ac | `mvk 0x2d0,b12` | stride / anchura del pool de frames = 720. Con 800 el PAL sale rayado y solo se reservan 9 frames («ReEstablishVport: MEMORY ALLOCATION ERROR») |
| — | salida de vídeo | PAL de 720 px de ancho: lo que quede a la derecha de 720 no se ve. Por eso AA dibuja en 640 de ancho (margen 160) |

## Notas de ingeniería inversa del DSP

* **Formato de `dvd_dm642.bin`:** tabla de arranque TI (entrada 0x819de0a0,
  69 secciones `{tamaño, dirección, datos}`), en palabras de 32 bits big-endian
  de una memoria little-endian. Para desensamblar: invertir cada palabra y usar
  capstone `CS_ARCH_TMS320C64X`. Las cadenas se encuentran tras invertir; el código
  carga sus direcciones con un par `mvk` (16 bits bajos, con signo) + `mvklh`
  (`-0x7e74` = 0x818c…).
* **Fourcc** que el código del DSP sí construye (con pares `mvkl`/`mvkh`): audio
  `M4AC`, `M1A3`, `DTSA`, `DVDA`, `MLP `, `HAAC`, `PCMA`; vídeo `AVC1`, `MP1V`, `MP2V`.
  `RAWA`/`LPCM` solo existen en el parser MP4 del host. El selector de audio por
  fourcc está hacia 0x81f1a674 («unsupported audio stream 4cc=%c%c%c%c»). En el
  coche, PCM (`PCMA`/`DVDA`) fue rechazado; `M4AC` (AAC) funciona.
* **Sincronía A/V (`avSync*`):** `avSyncStartClock`, `avSyncShowFrame`,
  `avSyncSetPlaybackSpeed`, `avSyncUpdateClocks`, `avSyncFieldInterrupt`… El
  reloj arranca con «Audio Pipe ES buffer is Full» → «AVPipeReady» → «StartRefClk»;
  sin audio se queda en `waitForAudioData` (de ahí el AAC en silencio).
* **Descartes:** si un frame con PTS llega tarde a la salida, el DSP lo salta
  («avSync: skip frame at output, id=%d (%d frames to skip)», código hacia
  0x81f205cc). Son las congelaciones breves cuando la carga llega al 100%.
* **Frames sin PTS:** `avSyncShowFrame` (≈0x81f19ad0) compara el PTS del frame con
  -1; si es -1 imprime «Show Vid no pts ref=%d,frm=%d» y lo muestra sin comprobar
  si llega tarde. Es la base del modo experimental `nopts` / `pts-1` de cicaalive.
  También existen «SyncOffCount» y «Clk Start No PTS's», sin estudiar todavía.
* **Mensajes del DSP:** llegan por el canal de comandos dspipc 0x15. cicaalive pone
  el nivel de depuración 10 (`SetDbgLvl`, comando 0x82) porque necesita los
  mensajes del arranque del reloj; son unos 11 por segundo y le cuestan ciclos al
  DSP. Bajarlo tras el arranque es una optimización pendiente.
* **Carga y retraso** (v57, coche): carga del DSP del 60–100% con AA en marcha.
  El retraso viene sobre todo de `vlead` (500 ms de margen de PTS), que se
  necesita mientras el vídeo vaya sincronizado con el reloj del audio.

## Ficheros y escrituras en el coche

| Ruta | Quién | Cuándo |
|---|---|---|
| `Plugins/AAP/*` (flash) | instalador | al instalar |
| `/mnt/hbdebug/aap/*` | instalador | al instalar |
| `/mnt/hbdebug/aap/geom.txt`, `keymap.txt`, `vlead_ms.txt` | instalador | solo si están en la raíz del pendrive (`default` los borra); sin reinicio |
| `/mnt/hbdebug/aap/logs.on` | plugin | solo al pulsar Logs |
| `/mnt/hbdebug/aap/logs/*.log` | plugin, libcicaa, cicaalive | solo con logs ON |
| `/dev/shmem/aap.stop`, `aap.seed`, anillos `/cicaa_*` | todos | RAM, se pierden al reiniciar |
| enlace de prefijo de la imagen DSP | dspload | RAM |
