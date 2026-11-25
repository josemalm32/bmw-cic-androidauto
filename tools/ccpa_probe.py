#!/usr/bin/env python3
"""Talk to a Carlinkit CPC200-CCPA dongle from a PC, before trying it in the car.

Sends the same start sequence as libcicaa (src/libcicaa/ccpa_core.c), logs every
message and saves the H.264 stream to a file you can play with ffplay.

    pip install pyusb            (Windows: also install the WinUSB driver for
                                  1314:1520 with Zadig)
    python tools/ccpa_probe.py --seconds 60 --out ccpa.h264
    ffplay -f h264 ccpa.h264
"""
import argparse
import json
import struct
import sys
import time

import usb.core
import usb.util

MAGIC = 0x55AA55AA
IDS = [(0x1314, 0x1520), (0x1314, 0x1521)]
NAMES = {0x01: 'Open', 0x02: 'Plugged', 0x03: 'Phase', 0x04: 'Unplugged', 0x06: 'Video',
         0x07: 'Audio', 0x08: 'Command', 0x0a: 'BtAddress', 0x0c: 'BtPIN', 0x0d: 'BtName',
         0x0e: 'WifiName', 0x12: 'BtPaired', 0x14: 'Manufacturer', 0x19: 'BoxSettings',
         0x2a: 'Media', 0xcc: 'SoftwareVersion'}


def frame(msg_type, payload=b''):
    return struct.pack('<IIII', MAGIC, len(payload), msg_type, msg_type ^ 0xFFFFFFFF) + payload


def send_file(name, content):
    n = name.encode('ascii') + b'\0'
    return frame(0x99, struct.pack('<I', len(n)) + n + struct.pack('<I', len(content)) + content)


def u32(v):
    return struct.pack('<I', v)


def cmd(v):
    return frame(0x08, u32(v))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--width', type=int, default=800)
    ap.add_argument('--height', type=int, default=480)
    ap.add_argument('--fps', type=int, default=30)
    ap.add_argument('--dpi', type=int, default=140)
    ap.add_argument('--seconds', type=int, default=60)
    ap.add_argument('--out', default='ccpa.h264')
    ap.add_argument('--usb-audio', action='store_true', help='audio over USB instead of phone Bluetooth')
    a = ap.parse_args()

    dev = None
    for vid, pid in IDS:
        dev = usb.core.find(idVendor=vid, idProduct=pid)
        if dev:
            break
    if not dev:
        sys.exit('No CCPA dongle (1314:1520/1521) found. Wrong model, or no WinUSB driver?')
    print(f'found {dev.idVendor:04x}:{dev.idProduct:04x} {usb.util.get_string(dev, dev.iProduct)!r}')
    dev.set_configuration(1)
    intf = dev.get_active_configuration()[(0, 0)]
    ep_in = usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
    ep_out = usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)

    def tx(b):
        ep_out.write(b, timeout=2000)

    tx(send_file('/tmp/screen_dpi', u32(a.dpi)))
    tx(frame(0x01, struct.pack('<7I', a.width, a.height, a.fps, 5, 49152, 2, 2)))
    tx(send_file('/tmp/night_mode', u32(0)))
    tx(send_file('/tmp/hand_drive_mode', u32(0)))
    tx(send_file('/tmp/charge_mode', u32(1)))
    tx(send_file('/etc/box_name', b'BMW CIC'))
    tx(frame(0x19, json.dumps({'mediaDelay': 300, 'syncTime': int(time.time()),
                               'androidAutoSizeW': a.width, 'androidAutoSizeH': a.height}).encode()))
    for c in (1000, 25, 7, 23 if a.usb_audio else 22):
        tx(cmd(c))
    tx(send_file('/etc/android_work_mode', u32(1)))
    t0 = time.time()
    last_hb = t0
    wifi_sent = False
    frames = 0
    buf = b''
    with open(a.out, 'wb') as out:
        while time.time() - t0 < a.seconds:
            now = time.time()
            if now - last_hb >= 2:
                tx(frame(0xAA))
                last_hb = now
            if not wifi_sent and now - t0 >= 1:
                tx(cmd(1002))
                wifi_sent = True
            try:
                buf += bytes(ep_in.read(65536, timeout=100))
            except usb.core.USBTimeoutError:
                continue
            while len(buf) >= 16:
                magic, ln, typ, chk = struct.unpack('<IIII', buf[:16])
                if magic != MAGIC or chk != typ ^ 0xFFFFFFFF:
                    print('bad header', buf[:16].hex())
                    buf = buf[1:]
                    continue
                if len(buf) < 16 + ln:
                    break
                p = buf[16:16 + ln]
                buf = buf[16 + ln:]
                name = NAMES.get(typ, f'0x{typ:02x}')
                if typ == 0x06:
                    w, h, flags = struct.unpack('<III', p[:12])
                    out.write(p[20:])
                    frames += 1
                    if frames == 1 or frames % 100 == 0:
                        print(f'[{now - t0:6.1f}] Video {w}x{h} flags={flags:#x} frames={frames}')
                    if frames % 150 == 0:
                        tx(cmd(12))
                elif typ == 0x07:
                    dt, vol, at = struct.unpack('<IfI', p[:12])
                    extra = f'cmd={p[12]}' if len(p) == 13 else f'pcm={len(p) - 12}B'
                    if len(p) <= 16:
                        print(f'[{now - t0:6.1f}] Audio decode={dt} type={at} {extra}')
                elif typ in (0x0d, 0x0e, 0x19, 0xcc, 0x0a, 0x12):
                    print(f'[{now - t0:6.1f}] {name}: {p.decode("ascii", "replace")}')
                else:
                    vals = [struct.unpack_from('<I', p, i)[0] for i in range(0, min(len(p), 32) - 3, 4)]
                    print(f'[{now - t0:6.1f}] {name} len={ln} {vals}')
    print(f'done: {frames} video frames -> {a.out}')


if __name__ == '__main__':
    main()
