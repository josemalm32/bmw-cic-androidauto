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

* **Solo CIC HIGH con software del CIC versión C1A o C16A.** El plugin llama a funciones
  internas de HmiMain por dirección fija; antes de usarlas compara su código con
  firmas. Versiones de HmiMain conocidas (`src/plugin/iba_profiles.h`):
  * 9283417A, probada en un E9x (las direcciones del código son de esta versión);
  * `CIC_L6_1107_Release B1107-501-1121A Build 11`, probada en un E60 LCI;
  * `CIC_L6_1103_Release B1103-500-1048A Build 43` (software del CIC C16A, E9x LCI):
    probada en el coche con el binario de la versión 1.0 parcheado con estas
    direcciones; las direcciones de las teclas skip y el perfil compilado desde el
    código fuente aún no están probados.

  Si tu HmiMain es otra versión, el plugin no toca nada y muestra
  "HmiMain distinto: no compatible"; ver [Otras versiones de HmiMain](#otras-versiones-de-hmimain).
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

Los textos de estado siguen el idioma configurado en el CIC (se lee del HMI al abrir
la página y en cada pulsación): inglés, español, alemán, francés, italiano,
neerlandés y portugués; cualquier otro idioma usa inglés. Las etiquetas de los
botones (fijas en el XML) quedan en inglés. Detectar el idioma requiere las
direcciones del proxy MMI de esa versión de HmiMain: conocidas para E9x (9283417A);
en otros perfiles los textos quedan en inglés hasta añadirlas.

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
src/plugin/iba_profiles.h   tablas de direcciones para versiones de HmiMain distintas de 9283417A
src/libcicaa/               protocolo Android Auto (USB/AOA, TLS, canales) -> anillos shm
src/cicaalive/              proceso que alimenta el DSP DM642 (H.264 + AAC) por dspipc
src/dspload/                parche/carga de la imagen DSP, verificación y reinicio
installer/copie_scr.plain.sh  instalador (build.py lo codifica a copie_scr.sh)
tools/build.py              compila todo y prepara el pendrive
patches/                    nuestro parche a mbed TLS (se aplica a una copia al compilar)
third_party/mbedtls         mbed TLS 3.6.7 (submódulo git)
tools/encdec_sh.py          codificación de copie_scr.sh que espera el CIC
tools/hmi_profile.py        busca las direcciones del plugin en otra versión de HmiMain
docs/ARQUITECTURA.md        cómo funciona por dentro
```

## Ajustes opcionales

Pon cualquiera de estos ficheros en la raíz del pendrive de instalación: el
instalador los copia a `/mnt/hbdebug/aap/` (sin reiniciar; se usan desde el
siguiente arranque de AA). Un fichero cuya primera palabra sea `default` borra ese
ajuste. El plugin nunca los crea.

* `geom.txt`: `sx sy sw sh dx dy dw dh`, recorte del vídeo y rectángulo en
  pantalla. Si existe, sustituye la geometría integrada del botón Zoom Mode (Normal
  `0 0 720 360 256 0 960 480`, Fullscreen `0 0 720 360 0 0 960 480`, Stretched
  `0 0 720 360 0 0 1280 480`).
* `keymap.txt`: líneas `tecla_cic tecla_android` (p. ej. `12 22`).
* `video.txt`: `margen_ancho margen_alto [dpi]` que se piden al móvil (por defecto
  `80 120 140`). Android Auto dibuja su interfaz en un área de
  (800 - margen_ancho) x (480 - margen_alto) del vídeo de 800x480. Sin `video.txt`
  esa área va en la esquina superior izquierda; con él el móvil la centra, así que
  ajusta `geom.txt` a juego. Menos dpi = interfaz más pequeña, cabe más.
  Márgenes mayores que `600 320` o dpi fuera de 60-400 se ignoran (quedan los valores
  por defecto).
* `driver.txt`: `right` para volante a la derecha (la barra de Android Auto pasa a la
  derecha); por defecto izquierda.
* `gps.txt`: `off` deja de mandar la posición del CIC al móvil (usa su propio GPS).
  Por defecto activado: la navegación sigue la posición del coche (navegación a estima).
* `vlead_ms.txt`: margen de vídeo en ms (por defecto 50; más alto, menos cortes
  y más retraso). **Experimental:** `nopts` o `pts-1` manda el vídeo sin marca de
  tiempo cuando el reloj del DSP ya corre: cada frame se muestra en cuanto se
  decodifica (mínimo retraso, nunca se descarta).
  Ajuste según pruebas en el coche: 150 ms se entalla más (el DSP descarta frames,
  `avSync: skip frame`, sobre todo en frames grandes cuando su carga llega al 100 %);
  500 ms es más fluido pero con más retraso; 300 ms es el compromiso por defecto.
  Prueba entre 250 y 400 con tu móvil. Una segunda palabra opcional `dbg100`
  (p. ej. `300 dbg100`) sube el nivel de depuración del DSP a 100 durante los
  primeros 8 s del reloj (solo diagnóstico, con los logs activados).

### Pantalla completa con las proporciones correctas

La pantalla del CIC es de 1280x480 (8:3) pero el área de AA por defecto es de 720x360
(2:1), mostrada a 960x480. Para llenar la pantalla sin estirar, se pide al
móvil un área 8:3 y se amplía. En un E60 LCI el escalador de vídeo del CIC solo acepta
un origen de hasta unos 600 px de ancho (640 se corta por la derecha; 600 o menos
llenan la pantalla), así que:

* `video.txt`: `200 256 96` (área de 600x224; dpi 96 para que quepan la barra y las
  tarjetas de AA);
  Con `video.txt` el móvil centra su área, así que `geom.txt` debe recortarla;
* `geom.txt`: `100 128 600 224 0 0 1280 480`.

Se ve menos nítido (ampliación de unas 2,1 veces) y las listas muestran menos filas,
pero ocupa toda la pantalla y los iconos redondos siguen redondos. Por telnet se
pueden probar valores de `geom.txt` en vivo:
`echo "100 128 600 224 0 0 1280 480" > /mnt/hbdebug/aap/geom.txt` y reconectar.

## Otras versiones de HmiMain

Otras versiones del software C1A contienen el mismo código de HmiMain desplazado
unos KB. Para soportar una:

1. Consigue su HmiMain. No es un fichero en el CIC en marcha: está en la segunda
   imagen de arranque (`memifs2 -o 0x00740000` en `/etc/starter.cfg`). Por telnet,
   copia la flash en bruto (solo lectura): `cp /dev/fs0 /fs/usb0/fs0.img`. Nunca
   copies nada *a* `/dev/fs0*`.
2. `python tools/hmi_profile.py --extract fs0.img HmiMain` (también sirve una copia de
   la partición `/dev/fs0p2`)
3. `python tools/hmi_profile.py HmiMain --name "E.. ..."`: busca las 47 huellas de
   código (ignorando las direcciones de los literal pools), resuelve las repetidas con
   el desplazamiento de sus vecinas, mueve las direcciones de datos según el
   desplazamiento medido y comprueba cada vtable por su nombre de tipo C++. No
   imprime una tabla incompleta.
4. Añade la salida a `src/plugin/iba_profiles.h`, renombra sus tablas `kXlat_X` /
   `kSigFix_X` (p. ej. `kXlatE61`, `kSigFixE61`) y añade una línea a `kIbaProfiles`.

Si el paso 3 da una huella como no encontrada o ambigua, o una dirección de datos que
no puede comprobar, esa versión es más que una copia desplazada (otra versión del
software del CIC). Busca esas direcciones a mano (desensamblado, volcado de memoria del
HmiMain en marcha) y pásalas con `--pin 0xANTIGUA=0xNUEVA`: la herramienta completa
entonces la tabla, toma las vtables de sus símbolos donde los datos no se movieron con
un único desplazamiento y marca las líneas fijadas. También avisa de las huellas que
tomó por su posición; revisa esas líneas.

En marcha, el plugin usa el primer perfil cuyas huellas coinciden todas; con los logs
activados, `plugin.log` muestra `HmiMain profile ...`.

Otra versión del software del CIC es más que un desplazamiento. En C16A el código se
movió por bloques en cantidades distintas (de 27 KB a 165 KB) y una de las 47
funciones tiene código distinto, así que su huella no está en ese HmiMain. El perfil
C16A se mapeó primero a mano; ahora `hmi_profile.py` lo reproduce con tres `--pin`
(`0x08d6ebbc`, `0x08d6ed6c`, `0x091fda28`). Su tabla `SigFix` contiene los bytes de esa
función en C16A.

## Licencia

[PolyForm Noncommercial 1.0.0](LICENSE.md): puedes usarlo, estudiarlo, modificarlo y
compartirlo gratis para cualquier fin **no comercial**. Venderlo, venderlo instalado
o incluirlo en un producto o servicio de pago requiere permiso escrito de
[josemalm32](https://github.com/josemalm32). Ver [NOTICE.md](NOTICE.md) (incluye mbed TLS, Apache-2.0).

## Aviso

Proyecto experimental, sin relación con BMW ni con Google. Modifica la flash del
CIC (carpeta de plugins) y usa funciones internas no documentadas. Úsalo bajo tu
responsabilidad y nunca manipules la pantalla mientras conduces.
