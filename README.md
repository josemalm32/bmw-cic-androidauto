# AAPlugin: Android Auto for BMW CIC HIGH

Native HMI plugin that runs **wired Android Auto** on the screen of a BMW with a
**CIC HIGH** head unit (SuperH SH4, QNX 6.3.2). No extra hardware: it uses the CIC's
own video DSP (TI DM642) for decoding, the car's audio path and the iDrive controller.

> [Versión en español](README.es.md)

## Status

| Feature | Status |
|---|---|
| AA connection over USB (AOA + TLS) | ✅ |
| Video on screen (800x480 → 960x480 centred) | ✅ |
| AA audio (music, navigation, system) through the car speakers | ✅ (AAC decoded on the DSP) |
| iDrive: rotary, push, 4 directions, back | ✅ |
| Leave / return to AA (MENU, Back, AA exit icon) | ✅ |
| Phone calls | via the car's Bluetooth (HFP), as usual |
| Microphone / Assistant / voice search | ❌ not implemented |
| Full screen 1280x480 | ❌ DSP limit (see [docs/ARQUITECTURA.md](docs/ARQUITECTURA.md)) |

Known issues: occasional short audio cuts or video freezes when the DSP is at 100%
(maps with a lot of movement), and about 0.5 s of latency.

## Compatibility

* **Only CIC HIGH with HmiMain 9283417A, i.e. CIC software version C1A** (tested on an E9x). The plugin calls
  internal HmiMain functions at fixed addresses; before using them it compares their
  code against signatures. If your HmiMain differs, the plugin touches nothing and
  shows "HmiMain distinto: no compatible".
* DSP image `dvd_dm642.bin` of 2363004 bytes (the one shipped with that firmware).
  The installer checks the exact bytes before patching.
* NBT / EVO / CIC MID are not supported.

## Using it in the car

**Multimedia → Android Auto** shows a page with:

* **Conectar Android Auto** (connect): starts AA. Plug the phone into the USB port;
  AA appears after a few seconds. The iDrive rotary, push and directions go to the
  phone. **MENU** or **Volver** (back) return to the car UI without closing AA;
  pressing *Conectar* again brings AA back.
* **Logs ON/OFF**: enables or disables logging. **By default nothing is written
  anywhere.** With logs ON, files go to `/mnt/hbdebug/aap/logs/`; the next time the
  CIC boots with the install stick plugged in, they are copied to the stick
  (`AALOGS/`) and deleted from the CIC.
* **Estado** (status): one line describing what is happening.

## Installation

1. Build it (see below) or download a release; you get a `usb/` folder.
2. Copy **the contents** of `usb/` to the root of a FAT32 USB stick:
   `copie_scr.sh` and the `AAP/` folder.
3. Add **your own Android Auto head-unit certificate** (PEM) to the stick root as
   `AAHU.CRT` and `AAHU.KEY`. **It is not part of this project**; without it the
   phone rejects the connection.
4. With the car on, plug in the stick and **restart the CIC**. The CIC only runs
   `copie_scr.sh` while booting; a stick inserted later is ignored.
5. The installer copies only what changed and **reboots the CIC by itself** only if
   it installed something. It leaves `AAINSTALL.TXT` on the stick.
6. Remove the stick (the car has a single USB port, needed for the phone).

What gets installed:

| Where | Files |
|---|---|
| Flash `/mnt/EFS_RO/Hmi/data/ro/Plugins/AAP/` | `AAPlugin.so`, `AAPlugin.xml`, `AAPlugin.manifest` |
| Disk `/mnt/hbdebug/aap/` | `libcicaa.so`, `cicaalive`, `dspload`, `hu.crt`, `hu.key`, `dvd_dm642_aa.bin` |

* `dvd_dm642_aa.bin` is **generated in the car** from its own DSP image (no BMW
  firmware is distributed, and it is not part of the build output). The original
  image in flash is never modified: the plugin swaps it in RAM only, and every CIC
  reboot restores the original.
* If the old development plugin is found (`Plugins/CMD` with `CicMenuDemo.so`), it
  is copied to the stick (`AALEGACY/`) and removed.

The ready-made `AAPlugin-<version>-usb.zip` from the GitHub Releases page contains
the same `usb/` content.

To update, repeat the steps with the new version. To uninstall, delete
`Plugins/AAP` (remounting the flash read-write) and `/mnt/hbdebug/aap`.

## Building

### 1. Get the source (with mbed TLS)

mbed TLS 3.6.7 (Apache-2.0) is included as a git submodule in
`third_party/mbedtls`, so the only thing you need to get on your own is the QNX
toolchain. Clone with submodules:

```bash
git clone --recursive https://github.com/josemalm32/bmw-cic-androidauto.git
cd bmw-cic-androidauto
```

If you already cloned without `--recursive`, or downloaded a zip of the code:

```bash
git submodule update --init third_party/mbedtls
```

(A GitHub "Download ZIP" does not include submodules. In that case download
mbed TLS 3.6.7 from <https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7>
and extract it so that `third_party/mbedtls/library/ssl_tls.c` exists.)

`build.py` applies our small patch in `patches/` (UTCTime with timezone in
head-unit certificates) to a copy in `out/build/`; the submodule is never modified.

### 2. QNX toolchain

* You need **QNX Momentics Development Suite 6.3.x for Windows**, under your own
  license from QNX (BlackBerry QNX). This project does not include or distribute it.
* During installation select the **SDK** and **GPL** components, including the SH
  target. The default location is `C:\QNX630`; otherwise set `QNX_HOST` and
  `QNX_TARGET`.
* Check: `ntosh-gcc-3.3.5 --version` must print `3.3.5 (qnx-nto)` and
  `target/qnx6/shle/lib/libc.so.2` must exist.

You also need Python 3.8 or newer.

### 3. Build

```bash
python tools/build.py
```

Environment variables (optional):

| Variable | Default |
|---|---|
| `QNX_HOST` | `C:/QNX630/host/win32/x86` |
| `QNX_TARGET` | `C:/QNX630/target/qnx6` |
| `MBEDTLS_DIR` | `third_party/mbedtls` |

The result is `out/usb/` (ready for the stick), `out/AAPlugin-<version>-usb.zip`
(the same plus license files) and the binaries in `out/build/`.
The car's libraries (`libCoreFramework`, `libAppFramework`, `libdspipc`...) are not
needed: `build.py` generates link-only stubs from the symbol lists in `tools/`.

#### Including your certificate in the build

If the `certs/` folder (create it next to `tools/`) contains one `*.crt` and one
`*.key` (PEM), `build.py` copies them to the stick root as `AAHU.CRT` / `AAHU.KEY`,
so both `out/usb/` and the zip are ready to use with no manual step. If either is
missing the build still works and just reminds you to add them by hand.
`certs/` is in `.gitignore` and is never pushed, but **a zip built this way contains
your private key: do not publish it** (for example as a GitHub release).

## Layout

```
src/common/aap_paths.h        paths, switches and defaults (margins, geometry)
src/plugin/                   HMI plugin (C++): UI, iDrive, native video route, start-up
src/libcicaa/                 Android Auto protocol (USB/AOA, TLS, channels) -> shm rings
src/cicaalive/                process feeding the DM642 DSP (H.264 + AAC) over dspipc
src/dspload/                  DSP image patch/load, file compare, reboot
installer/copie_scr.plain.sh  installer (build.py encodes it to copie_scr.sh)
tools/build.py                builds everything and stages the USB stick
patches/                      our patch to mbed TLS (applied to a copy at build time)
third_party/mbedtls           mbed TLS 3.6.7 (git submodule)
tools/encdec_sh.py            copie_scr.sh encoding expected by the CIC
docs/ARQUITECTURA.md          how it works inside (Spanish)
```

## Optional settings

Put any of these files in the root of the install stick: the installer copies them
to `/mnt/hbdebug/aap/` (no reboot; used from the next AA start). A file whose first
word is `default` removes that setting. The plugin itself never creates them.

* `geom.txt`: `sx sy sw sh dx dy dw dh`, video crop and on-screen rectangle.
  Default `80 80 640 320 160 0 960 480`; `80 80 640 320 0 0 1280 480` stretches it
  to the full width.
* `keymap.txt`: lines `cic_key android_key` (e.g. `12 22`).
* `vlead_ms.txt`: video lead in ms (default 300; higher = fewer freezes, more latency).
  **Experimental:** `nopts` or `pts-1` sends video without timestamps once the DSP clock
  runs, so each frame is shown as soon as it is decoded (lowest latency, never skipped).
  Tuning from car tests: 150 ms stalls more (the DSP drops frames, `avSync: skip frame`,
  mostly on large frames when its load hits 100 %); 500 ms is smoother but laggier;
  300 ms is the default compromise. Try 250-400 for your phone.
  An optional second word `dbg100` (e.g. `300 dbg100`) raises the DSP debug level to
  100 for the first 8 s of the clock (diagnostics only, needs logs ON).

## License

[PolyForm Noncommercial 1.0.0](LICENSE.md): free to use, study, modify and share for
any **noncommercial** purpose. Selling it, selling it pre-installed, or including it
in a paid product or service requires written permission from
[josemalm32](https://github.com/josemalm32). See [NOTICE.md](NOTICE.md) (includes
mbed TLS, Apache-2.0).

## Disclaimer

Experimental project, not affiliated with BMW or Google. It modifies the CIC flash
(plugins folder) and uses undocumented internal functions. Use at your own risk and
never operate the screen while driving.
