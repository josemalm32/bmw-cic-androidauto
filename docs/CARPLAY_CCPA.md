# CarPlay con el dongle Carlinkit CPC200-CCPA

El CIC no tiene chip MFi, así que no puede hablar CarPlay directamente con un iPhone. El dongle CPC200-CCPA ("Autokit", versión para radios Android) hace él la autenticación MFi, iAP2/AirPlay y la conexión Wi-Fi/Bluetooth con el teléfono. Por USB entrega H.264 y PCM con un protocolo sencillo. Ese protocolo lo documenta el proyecto [node-CarPlay](https://github.com/rhysmorgan134/node-CarPlay) (MIT).

No sirven los adaptadores "de cable a inalámbrico" (Carlinkit 2Air/3.0/4.0/5.0, Ottocast Mini Cube): esos se presentan como un iPhone y necesitan un coche con CarPlay de fábrica.

## Cómo funciona en libcicaa

| Parte | Dónde | Qué hace |
| --- | --- | --- |
| USB | `src/libcicaa/aa_usb_qnx.c` (`aa_usb_open2`) | Si ve `1314:1520/1521` abre sus pipes bulk y devuelve 1. Un teléfono Android sigue por AOA como antes. El dongle queda excluido del intento AOA. |
| Protocolo | `src/libcicaa/ccpa_core.c` (`ccpa_session`) | Secuencia de arranque, latido cada 2 s, lectura de mensajes, petición de fotograma clave cada 5 s en CarPlay. |
| Vídeo | `av_push()` | Mismo camino que Android Auto: `/cicaa_live` → `cicaalive` → DM642. |
| Audio | por defecto Bluetooth | Se manda `audioTransferOn`: el iPhone envía el audio por el Bluetooth del coche, porque el DM642 rechazó PCM. Con `ccpa_usbaudio.on` el PCM va al anillo `/cicaa_audio` (experimental). |
| Mando | `cicaa_input_pop()` | Giro → `left/right` (100/101), pulsar → `selectDown/selectUp` (104/105), atrás → 106, cruceta → 113/114/100/101, home → 200. |
| Micrófono | — | No implementado (CCPA sin micro). |

El backend se elige solo al pulsar Conectar en el plugin: primero el dongle, si no hay, un teléfono Android por cable. El dongle no necesita `hu.crt`/`hu.key`.

## Ficheros opcionales en `/mnt/hbdebug/aap/`

- `ccpa.off`: no buscar el dongle.
- `ccpa.txt`: `ancho alto [fps [dpi]]` pedidos al dongle (por defecto `800 480 30 140`).
- `ccpa_usbaudio.on`: audio del dongle por USB en vez de Bluetooth.

## Pruebas en orden

1. **PC**: instalar WinUSB para `1314:1520` con Zadig, `pip install pyusb`, `python tools/ccpa_probe.py --seconds 60`. Conectar el iPhone al dongle. Debe salir `Plugged` con tipo 3 (CarPlay) y vídeo. Reproducir con `ffplay -f h264 ccpa.h264`.
2. **Coche**: `logs.on`, instalar 1.2.0-ccpa, conectar el dongle y pulsar Conectar. En `cicaa.log`: `CCPA dongle present`, `CCPA open sent`, `CCPA plugged phoneType=3`, `CCPA video open`.
3. Comprobar giro, pulsación y atrás en CarPlay.
4. Audio: emparejar el iPhone con el Bluetooth del coche y comprobar música y navegación.
5. Regresión: teléfono Android por cable sin dongle → Android Auto como en 1.1.0.

## Pendiente / riesgos

- El USB del CIC debe alimentar el dongle (5 V 1 A según la ficha).
- CarPlay exige 800x480 como mínimo; el DM642 recorta las 80 columnas de la derecha (igual que con AA a 800).
- Si CarPlay no responde al mando, probar emulación táctil con cursor (`Touch` 0x05).
- Audio por USB al DSP: depende de que el DM642 acepte PCM, o de un codificador AAC en SH4.
- Micrófono para Siri y llamadas.
