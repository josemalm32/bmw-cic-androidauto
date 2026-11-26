# Changelog

## 1.1.1 (2026-11-25)

Changes since 1.1.0. Profile-only release: the plugin code is unchanged, so
cars that already worked on 1.1.0 behave the same.

### New features

- **E9x LCI (CIC software C16A, B1103-500-1048A) HmiMain profile**
  (@dertechnikfrea). Tested in the car.
- **F10 (CIC software B1107-501-1123B) HmiMain profile** (@patrus28). The
  steering-wheel and panel skip key hooks are not mapped for this build yet,
  so those keys do not control AA on the F10.
- **`tools/hmi_profile.py` handles another CIC software version**
  (@dertechnikfrea): `--pin` option, vtables found by symbol.

### Documentation

- Microphone, AA voice and voice-button findings for E9x LCI / C16A
  (@dertechnikfrea).
- README: the C16A profile can now be generated with `--pin`.

## 1.1.0 (2026-11-23)

Changes since 1.0.0. Most of the new features and fixes come from contributors
who tested on a BMW Z4 E89, an E60 LCI and an F30.

### New features

- **More cars supported (HmiMain address profiles).** The plugin now runs on
  the E60 LCI HmiMain (CI63F1 9273157A) and the F30 HmiMain (Rev 76617) as
  well as the E9x 9283417A (C1A). Addresses are translated per build.
  `tools/hmi_profile.py` generates a profile from an HmiMain image. When an
  alternative HmiMain is installed in `/mnt/hbuser`, the installer registers
  the plugin there too.
- **GPS from the car.** The CIC position (dead reckoning with wheel speed and
  gyro) is sent to the phone as the AA LOCATION sensor. Navigation keeps
  working in tunnels and car parks.
- **Steering-wheel and panel skip keys** go to the phone in AA Audio mode
  (next/previous track). The hooks are installed at Connect, after their
  addresses are checked, and they are removed on exit.
- **BT Audio is now the default.** Music plays over Bluetooth A2DP, so the CIC
  keeps its own source handling, volume range and skip keys. Navigation
  prompts still come over USB. The new **Switch Audio** button turns AA Audio
  (music over USB to the DSP) on or off.
- **Disconnect button.** Closes Android Auto on the phone properly
  (ByeByeRequest). You can press Connect again without unplugging the cable.
- **Screen modes.** The **Zoom Mode** button cycles through Normal, Fullscreen
  and Stretched. The mode is kept across restarts.
- **Settings files** `video.txt` (margins, with limits) and `driver.txt`
  (driver side; the default is left-hand drive).

### Picture and latency

- AA now uses a 720x360 area (the widest the DM642 DVD decoder outputs), with
  fixed built-in geometry. `geom.txt` is no longer used.
- Geometry hold: if the DVD service resets the picture to its PAL window, the
  plugin corrects it again. The picture fades in, so a wrong-sized frame no
  longer flashes on screen.
- Lower input delay: the default video lead is now 50 ms (was 300 ms). The
  start-up backlog is dropped once, and a watchdog resyncs when the display
  delay stays above 350 ms. The result is about 190 ms of delay, and long
  drives no longer freeze.
- Video frames are kept away from the 1 MiB wrap of the DSP ring buffer by
  adding filler NALs. This fixes the freezes, flicker and DSP errors every
  1 to 2 minutes.
- A new DSP firmware patch (late-frame skip counter) stops the DSP from
  dropping frames after a late frame. Before, the map froze after fast
  scrolling. The installer applies it automatically.
- After a DSP error, a new DSP session starts. Before, the picture stayed
  frozen.
- The AA picture comes back by itself after MENU, OPTION or reverse gear/PDC.
  Keyframe requests are answered within 100 ms.

### Other changes

- The night sensor is no longer sent. The phone uses its own day/night
  setting.
- The plugin waits 30 s for the phone (was 120 s), so a forgotten Connect no
  longer connects by surprise later.
- Installer: if `keymap.txt` or `vlead_ms.txt` is missing on the stick, the
  copy on the CIC is removed and the default applies. Old `geom*.txt` files
  are deleted.
- All status and log texts are now in English. The page was simplified, and
  the Logs button is now called Logging.
- Docs: new `docs/HALLAZGOS.md` with contributor findings (GPS, skip keys,
  DSP, HmiMain profiles).
