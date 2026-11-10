# AAPlugin: Android Auto para BMW CIC HIGH

Plugin HMI nativo que hace funcionar **Android Auto por USB** en la pantalla de un
BMW con **CIC HIGH** (SuperH SH4, QNX 6.3.2). Funciona sin hardware adicional:
usa el decodificador de vídeo DSP del propio CIC (TI DM642), el audio del coche y
el mando iDrive.

> [English version](README.md)

## Estado

| Función | Estado |
|---|---|
| Conexión AA por USB (AOA + TLS) | ✅ |
| Vídeo en pantalla (800x480 → 960x480 centrado) | ✅ |
| Audio AA (música, navegación, sistema) por los altavoces | ✅ (AAC decodificado en el DSP) |
| iDrive: ruleta, pulsar, 4 direcciones, atrás | ✅ |
| Salir / volver a AA (MENU, Volver, icono de salida de AA) | ✅ |
| Llamadas | por Bluetooth del coche (HFP), como siempre |
| Micrófono / Asistente / búsqueda por voz | ❌ no implementado |
| Pantalla completa 1280x480 | ❌ límite del DSP (ver [docs/ARQUITECTURA.md](docs/ARQUITECTURA.md)) |

Problemas conocidos: algún corte o congelación breve de imagen cuando el DSP va al
100% (mapas con mucho movimiento) y ~0,5 s de retraso.

## Compatibilidad

* **Solo CIC HIGH con HmiMain 9283417A, es decir, software del CIC versión C1A** (probado en un E9x). El plugin llama a
  funciones internas de HmiMain por dirección fija; antes de usarlas compara su
  código con firmas. Si tu HmiMain es otro, el plugin no toca nada y muestra
  "HmiMain distinto: no compatible".
* Imagen DSP `dvd_dm642.bin` de 2363004 bytes (la que acompaña ese firmware). El
  instalador comprueba los bytes exactos antes de parchear.
* NBT / EVO / CIC MID no están soportados.

## Uso en el coche

En **Multimedia → Android Auto** aparece una pantalla con:

* **Conectar Android Auto**: arranca AA. Conecta el móvil al USB; en unos segundos
  aparece AA. La ruleta, el pulsador y las direcciones del iDrive van al móvil.
  **MENU** o **Volver** te devuelven al coche sin cortar AA; volver a pulsar
  *Conectar* vuelve a AA.
* **Logs ON/OFF**: activa o desactiva el registro. **Por defecto no se escribe
  nada en ningún sitio.** Con logs ON se escribe en `/mnt/hbdebug/aap/logs/`; la
  próxima vez que arranques el CIC con el USB de instalación puesto, los logs se
  copian al USB (`AALOGS/`) y se borran del CIC.
* **Estado**: línea con lo que está pasando.

## Instalación

1. Compila (ver abajo) o descarga una release; obtendrás una carpeta `usb/`.
2. Copia **el contenido** de `usb/` a la raíz de un pendrive FAT32:
   `copie_scr.sh` y la carpeta `AAP/`.
3. Añade a la raíz del pendrive **tu certificado de head unit de Android Auto**
   en PEM, con estos nombres: `AAHU.CRT` y `AAHU.KEY`. **No se incluye en este
   proyecto**; sin él el móvil rechaza la conexión.
4. Con el coche encendido, mete el pendrive y **reinicia el CIC** (el CIC solo
   ejecuta `copie_scr.sh` al arrancar; si metes el pendrive más tarde, no hace nada).
5. El instalador copia solo lo que haya cambiado y **reinicia el CIC solo** si ha
   instalado algo. Al terminar deja `AAINSTALL.TXT` en el pendrive.
6. Quita el pendrive (el coche tiene un solo USB: hace falta para el móvil).

Qué instala:

| Dónde | Ficheros |
|---|---|
| Flash `/mnt/EFS_RO/Hmi/data/ro/Plugins/AAP/` | `AAPlugin.so`, `AAPlugin.xml`, `AAPlugin.manifest` |
| Disco `/mnt/hbdebug/aap/` | `libcicaa.so`, `cicaalive`, `dspload`, `hu.crt`, `hu.key`, `dvd_dm642_aa.bin` |

* `dvd_dm642_aa.bin` **se genera en el coche** a partir de su propia imagen DSP
  (no se distribuye firmware de BMW). La original de la flash no se modifica:
  el plugin la sustituye solo en RAM, y cada reinicio del CIC vuelve a la original.
* Si encuentra el plugin de desarrollo antiguo (`Plugins/CMD` con
  `CicMenuDemo.so`), lo copia al pendrive (`AALEGACY/`) y lo elimina.

El `AAPlugin-<versión>-usb.zip` de la página de Releases de GitHub contiene lo
mismo que `usb/`, ya compilado.

Para actualizar, repite el proceso con la versión nueva. Para desinstalar, borra
`Plugins/AAP` (remontando la flash en escritura) y `/mnt/hbdebug/aap`.

## Compilar

### 1. Descargar el código (con mbed TLS)

mbed TLS 3.6.7 (Apache-2.0) va incluido como submódulo git en
`third_party/mbedtls`, así que lo único que tienes que conseguir por tu cuenta es la
toolchain de QNX. Clona con los submódulos:

```bash
git clone --recursive https://github.com/josemalm32/bmw-cic-androidauto.git
cd bmw-cic-androidauto
```

Si ya lo clonaste sin `--recursive`, o descargaste un zip del código:

```bash
git submodule update --init third_party/mbedtls
```

(El «Download ZIP» de GitHub no incluye los submódulos. En ese caso descarga
mbed TLS 3.6.7 de <https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7>
y descomprímelo de forma que exista `third_party/mbedtls/library/ssl_tls.c`.)

`build.py` aplica nuestro pequeño parche de `patches/` (UTCTime con zona horaria en
los certificados de head unit) a una copia en `out/build/`; el submódulo no se toca.

### 2. Toolchain QNX

* Hace falta **QNX Momentics Development Suite 6.3.x para Windows**, con licencia
  propia de QNX (BlackBerry QNX). Este proyecto no la incluye ni la distribuye.
* En la instalación elige los componentes **SDK** y **GPL**, con el destino SH
  incluido. Por defecto se instala en `C:\QNX630`; si no, indica las rutas con
  `QNX_HOST` y `QNX_TARGET`.
* Comprobación: `ntosh-gcc-3.3.5 --version` debe responder `3.3.5 (qnx-nto)` y
  debe existir `target/qnx6/shle/lib/libc.so.2`.

También necesitas Python 3.8 o superior.

### 3. Compilar

```bash
python tools/build.py
```

Variables de entorno (opcionales):

| Variable | Por defecto |
|---|---|
| `QNX_HOST` | `C:/QNX630/host/win32/x86` |
| `QNX_TARGET` | `C:/QNX630/target/qnx6` |
| `MBEDTLS_DIR` | `third_party/mbedtls` |

El resultado queda en `out/usb/` (listo para el pendrive), en
`out/AAPlugin-<versión>-usb.zip` (lo mismo más las licencias) y los binarios en
`out/build/`. Las librerías del coche (`libCoreFramework`, `libAppFramework`,
`libdspipc`...) no hacen falta: `build.py` genera stubs solo para enlazar, a partir
de las listas de símbolos de `tools/`.

#### Incluir tu certificado en la compilación

Si la carpeta `certs/` (créala junto a `tools/`) contiene un `*.crt` y un `*.key`
(PEM), `build.py` los copia a la raíz del pendrive como `AAHU.CRT` / `AAHU.KEY`, así
que tanto `out/usb/` como el zip quedan listos sin pasos manuales. Si falta alguno,
la compilación funciona igual y solo te recuerda añadirlos a mano. `certs/` está en
`.gitignore` y nunca se sube, pero **un zip compilado así contiene tu clave
privada: no lo publiques** (por ejemplo como release de GitHub).

## Estructura

```
src/common/aap_paths.h      rutas, interruptores y valores por defecto (márgenes, geometría)
src/plugin/                 plugin HMI (C++): UI, iDrive, ruta de vídeo nativa, arranque
src/libcicaa/               protocolo Android Auto (USB/AOA, TLS, canales) -> anillos shm
src/cicaalive/              proceso que alimenta el DSP DM642 (H.264 + AAC) por dspipc
src/dspload/                parche/carga de la imagen DSP, verificación y reinicio
installer/copie_scr.plain.sh  instalador (build.py lo codifica a copie_scr.sh)
tools/build.py              compila todo y prepara el pendrive
patches/                    nuestro parche a mbed TLS (se aplica a una copia al compilar)
third_party/mbedtls         mbed TLS 3.6.7 (submódulo git)
tools/encdec_sh.py          codificación de copie_scr.sh que espera el CIC
docs/ARQUITECTURA.md        cómo funciona por dentro
```

## Ajustes opcionales

Pon cualquiera de estos ficheros en la raíz del pendrive de instalación: el
instalador los copia a `/mnt/hbdebug/aap/` (sin reiniciar; se usan desde el
siguiente arranque de AA). Un fichero cuya primera palabra sea `default` borra ese
ajuste. El plugin nunca los crea.

* `geom.txt`: `sx sy sw sh dx dy dw dh`, recorte del vídeo y rectángulo en
  pantalla. Por defecto `80 80 640 320 160 0 960 480`. Por ejemplo,
  `80 80 640 320 0 0 1280 480` lo estira a todo el ancho.
* `keymap.txt`: líneas `tecla_cic tecla_android` (p. ej. `12 22`).
* `vlead_ms.txt`: margen de vídeo en ms (por defecto 300; más alto, menos cortes
  y más retraso). **Experimental:** `nopts` o `pts-1` manda el vídeo sin marca de
  tiempo cuando el reloj del DSP ya corre: cada frame se muestra en cuanto se
  decodifica (mínimo retraso, nunca se descarta).
  Ajuste según pruebas en el coche: 150 ms se entalla más (el DSP descarta frames,
  `avSync: skip frame`, sobre todo en frames grandes cuando su carga llega al 100 %);
  500 ms es más fluido pero con más retraso; 300 ms es el compromiso por defecto.
  Prueba entre 250 y 400 con tu móvil. Una segunda palabra opcional `dbg100`
  (p. ej. `300 dbg100`) sube el nivel de depuración del DSP a 100 durante los
  primeros 8 s del reloj (solo diagnóstico, con los logs activados).

## Licencia

[PolyForm Noncommercial 1.0.0](LICENSE.md): puedes usarlo, estudiarlo, modificarlo y
compartirlo gratis para cualquier fin **no comercial**. Venderlo, venderlo instalado
o incluirlo en un producto o servicio de pago requiere permiso escrito de
[josemalm32](https://github.com/josemalm32). Ver [NOTICE.md](NOTICE.md) (incluye mbed TLS, Apache-2.0).

## Aviso

Proyecto experimental, sin relación con BMW ni con Google. Modifica la flash del
CIC (carpeta de plugins) y usa funciones internas no documentadas. Úsalo bajo tu
responsabilidad y nunca manipules la pantalla mientras conduces.
