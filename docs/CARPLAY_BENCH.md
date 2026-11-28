# CarPlay USB bench build

The receiver is a separate executable. The regular Android Auto build and USB installer do not start or install it.

- Initialize the pinned Mbed TLS submodule. Put lwIP 2.2.1 (`77dcd25a72509eb83f72b033d219b1d40cd8eb95`) and Monocypher 4.0.2 (`0d85f98c9d9b0227e42cf795cb527dff372b40a4`) under one directory as `lwip` and `monocypher`.
- Run `python tools/build.py --carplay-deps <directory>` to build `out/build/cic_carplay`. Run the regular build for the updated AAPlugin and `cicaalive`.
- On a bench CIC with Android Auto idle, install the updated AAPlugin and `cicaalive` using the regular procedure, then place `cic_carplay` under `/mnt/hbdebug/aap`. Run `sh carplay-bench.sh /path/to/accessory.p7b`, then select Connect on the AAPlugin page. Unplug the iPhone or terminate the launcher to stop the bench session. If the USB interface is busy, stop iPod playback and retry with the phone unplugged until the receiver starts waiting for it.
- The public PKCS#7 file must match the authenticator returned by the ACP backend. The upstream ACP node is the default. Set `CIC_CARPLAY_ACP_NODE` if the bench unit exposes its compatible authenticator at another path. Its presence and compatibility on CIC are unverified. The bench build targets 30 fps, video and iDrive.

On CIC units without `/dev/urandom`, set `CIC_CARPLAY_ENTROPY_FILE` to a private file of fresh cryptographically random bytes supplied from a trusted host (at least 64 bytes). Generate a fresh file for each launch.

The NBT upstream revision and licensing are recorded in [src/carplay/UPSTREAM.txt](../src/carplay/UPSTREAM.txt).

Run `bash tools/carplay-host-check.sh <directory>` on Linux with GCC and Python 3 for NCM, the CIC video ring and simulated AirPlay authentication checks.

## USB diagnosis before MFi

With Android Auto and iPod playback idle, run `/mnt/hbdebug/aap/cic_carplay --probe-usb` on the bench CIC and plug in the iPhone. Expect `usb probe: multiplexor and NCM endpoints opened`. The probe releases USB and exits before lockdown or CarPlay session negotiation, so a certificate is unnecessary for this check. The entropy setting above still applies. Unplug the phone before starting the full test.
