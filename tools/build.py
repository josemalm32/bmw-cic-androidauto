#!/usr/bin/env python3
"""Build AAPlugin for the BMW CIC HIGH (QNX 6.3.2, SH4 little-endian) and stage the USB stick.

Requirements (not included, get them yourself):
  * QNX Momentics 6.3 SH4 toolchain (ntosh-gcc-3.3.5, ntosh-g++-3.3.5, ntosh-as, ntosh-ld, ntosh-strip)
    QNX_HOST   default C:/QNX630/host/win32/x86
    QNX_TARGET default C:/QNX630/target/qnx6   (needs shle/lib/libc.so.2, usr/include)
  * mbed TLS 3.6.7 source tree (Apache-2.0), MBEDTLS_DIR default third_party/mbedtls-3.6.7
  * Python 3.8+

Usage:  python tools/build.py [--out out]
Result: out/usb/  -> copy its content to the root of a FAT32 USB stick:
            copie_scr.sh   (encoded installer)
            AAP/           (payload)
        Add your own Android Auto head-unit certificate as AAHU.CRT + AAHU.KEY (PEM) to the stick root.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'src'
sys.path.insert(0, str(ROOT / 'tools'))
from encdec_sh import transform  # noqa: E402


def version():
    for line in (SRC / 'common/aap_paths.h').read_text().splitlines():
        if line.startswith('#define AAP_VERSION'):
            return line.split('"')[1]
    raise SystemExit('AAP_VERSION not found')


class Tool:
    def __init__(self):
        self.host = Path(os.environ.get('QNX_HOST', 'C:/QNX630/host/win32/x86'))
        self.target = Path(os.environ.get('QNX_TARGET', 'C:/QNX630/target/qnx6'))
        self.bin = self.host / 'usr/bin'
        if not (self.bin / exe('ntosh-gcc-3.3.5')).exists():
            raise SystemExit(f'QNX SH toolchain not found in {self.bin} (set QNX_HOST)')
        self.env = os.environ.copy()
        self.env.update(QNX_HOST=str(self.host), QNX_TARGET=str(self.target))
        self.env['PATH'] = str(self.bin) + os.pathsep + self.env['PATH']

    def run(self, tool, args):
        cmd = [str(self.bin / exe(tool))] + [str(a) for a in args]
        for _ in range(10):  # the old cygwin toolchain fails transiently on fork
            r = subprocess.run(cmd, env=self.env, capture_output=True, text=True)
            if r.returncode == 0:
                return r.stdout + r.stderr
            if not any(k in r.stderr for k in ('fork', 'heap allocated', 'died')):
                break
            time.sleep(0.3)
        raise SystemExit(f'FAILED: {" ".join(cmd)}\n{r.stdout}{r.stderr}')


def exe(name):
    return name + '.exe' if os.name == 'nt' else name


def stub_lib(t, out, soname, names):
    """Link-only shared object exporting `names` (real library is on the car)."""
    asm = out / (soname + '.s')
    asm.write_text('.text\n.align 2\n' + ''.join(
        f'.global {n}\n.type {n},@function\n{n}:\n .long 0\n' for n in sorted(names)))
    t.run('ntosh-as', ['-little', '-o', out / (soname + '.o'), asm])
    t.run('ntosh-ld', ['-EL', '-shared', '-soname', soname, '-o', out / soname, out / (soname + '.o')])
    return out / soname


def build_libcicaa(t, b, mbedtls):
    obj = b / 'obj'
    obj.mkdir(parents=True, exist_ok=True)
    inc = ['-I' + str(SRC / 'libcicaa'), '-I' + str(SRC / 'common'),
           '-I' + str(mbedtls / 'include'), '-I' + str(mbedtls / 'library')]
    flags = ['-m4', '-ml', '-std=c99', '-D_QNX_SOURCE', '-fPIC', '-Os', '-Wall',
             '-DMBEDTLS_CONFIG_FILE=<cicaa_config.h>'] + inc
    srcs = [f for f in sorted((mbedtls / 'library').glob('*.c')) if f.name not in ('net_sockets.c', 'timing.c')]
    srcs += [SRC / 'libcicaa' / (n + '.c') for n in ('aa_core', 'aa_usb_qnx', 'cicaa', 'aa_video', 'aa_video_native')]
    objs = []
    for f in srcs:
        o = obj / (f.stem + '.o')
        if not o.exists() or o.stat().st_mtime < f.stat().st_mtime or f.parent.name == 'libcicaa':
            t.run('ntosh-gcc-3.3.5', flags + ['-c', f, '-o', o])
        objs.append(o)
    lib = b / 'libcicaa.so'
    t.run('ntosh-gcc-3.3.5', ['-m4', '-ml', '-shared', '-Wl,-z,defs', '-Wl,-soname,libcicaa.so', '-o', lib] + objs + ['-lusbdi', '-lc'])
    t.run('ntosh-strip', ['--strip-unneeded', lib])
    return lib


def build_cicaalive(t, b):
    stubs = b / 'stubs'
    stubs.mkdir(exist_ok=True)
    names = json.loads((ROOT / 'tools/libdspipc_symbols.json').read_text())
    dspipc = stub_lib(t, stubs, 'libdspipc.so', names)
    o = b / 'cicaalive.o'
    t.run('ntosh-gcc-3.3.5', ['-m4', '-ml', '-std=gnu99', '-D_QNX_SOURCE', '-Os', '-Wall',
                              '-I' + str(SRC / 'libcicaa'), '-I' + str(SRC / 'common'),
                              '-c', SRC / 'cicaalive/cicaalive.c', '-o', o])
    out = b / 'cicaalive'
    t.run('ntosh-gcc-3.3.5', ['-m4', '-ml', o, dspipc, '-o', out])
    t.run('ntosh-strip', [out])
    return out


def build_dspload(t, b):
    o = b / 'dspload.o'
    t.run('ntosh-gcc-3.3.5', ['-m4', '-ml', '-std=c99', '-D_QNX_SOURCE', '-Os', '-Wall',
                              '-I' + str(SRC / 'common'), '-c', SRC / 'dspload/dspload.c', '-o', o])
    out = b / 'dspload'
    t.run('ntosh-gcc-3.3.5', ['-m4', '-ml', o, '-o', out])
    t.run('ntosh-strip', [out])
    return out


def build_plugin(t, b):
    stubs = b / 'stubs'
    stubs.mkdir(exist_ok=True)
    o = b / 'AAPlugin.o'
    t.run('ntosh-g++-3.3.5', ['-m4', '-ml', '-fPIC', '-Os', '-fno-exceptions', '-fno-rtti', '-fcheck-new',
                              '-I' + str(SRC / 'plugin'), '-I' + str(SRC / 'common'),
                              '-c', SRC / 'plugin/AAPlugin.cpp', '-o', o])
    libs = []
    for p in json.loads((ROOT / 'tools/link_providers.json').read_text()):
        if p['soname'] == 'libc.so.2':
            continue
        libs.append(stub_lib(t, stubs, p['soname'], set(p['symbols'])))
    libs += [t.target / 'shle/lib/libc.so.2',
             t.host / 'usr/lib/gcc-lib/sh-unknown-nto-qnx6.3.0/3.3.5/pic/libgcc.a']
    out = b / 'AAPlugin.so'
    t.run('ntosh-ld', ['-EL', '-shared', '-soname', 'AAPlugin.so', '-z', 'defs', '-o', out, o] + libs)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=str(ROOT / 'out'))
    args = ap.parse_args()
    out = Path(args.out)
    b = out / 'build'
    b.mkdir(parents=True, exist_ok=True)
    mbedtls = Path(os.environ.get('MBEDTLS_DIR', ROOT / 'third_party/mbedtls-3.6.7'))
    if not (mbedtls / 'library/ssl_tls.c').exists():
        raise SystemExit(f'mbed TLS 3.6.7 not found at {mbedtls} (set MBEDTLS_DIR)')
    t = Tool()
    ver = version()
    print('AAPlugin', ver)
    files = {
        'libcicaa.so': build_libcicaa(t, b, mbedtls),
        'cicaalive': build_cicaalive(t, b),
        'dspload': build_dspload(t, b),
        'AAPlugin.so': build_plugin(t, b),
    }
    usb = out / 'usb'
    if usb.exists():
        shutil.rmtree(usb)
    pay = usb / 'AAP'
    pay.mkdir(parents=True)
    for name, path in files.items():
        shutil.copyfile(path, pay / name)
        print(f'  {name:14s} {path.stat().st_size:8d} bytes')
    for name in ('AAPlugin.xml', 'AAPlugin.manifest'):
        shutil.copyfile(SRC / 'plugin' / name, pay / name)
    script = (ROOT / 'installer/copie_scr.plain.sh').read_text().replace('@VERSION@', ver)
    script = script.replace('\r\n', '\n').encode()
    (usb / 'copie_scr.sh').write_bytes(transform(script))
    print(f'USB stick content ready in {usb}')
    # Release zip: extract to the root of the stick. Includes the license texts
    # (libcicaa.so contains mbed TLS, Apache-2.0).
    rel = out / f'AAPlugin-{ver}-usb.zip'
    with zipfile.ZipFile(rel, 'w', zipfile.ZIP_DEFLATED) as z:
        for f in sorted(usb.rglob('*')):
            if f.is_file():
                z.write(f, f.relative_to(usb).as_posix())
        z.write(ROOT / 'LICENSE.md', 'AAPLUGIN-LICENSE.md')
        z.write(ROOT / 'NOTICE.md', 'AAPLUGIN-NOTICE.md')
        z.write(mbedtls / 'LICENSE', 'MBEDTLS-LICENSE.txt')
        z.writestr('AAPLUGIN-INSTALL.txt',
                   f'AAPlugin {ver} - Android Auto for BMW CIC HIGH (CIC C1A, HmiMain 9283417A)\r\n'
                   'Extract everything to the root of a FAT32 USB stick, add your AA head-unit\r\n'
                   'certificate as AAHU.CRT + AAHU.KEY (PEM), plug it in and restart the CIC.\r\n'
                   'Details: https://github.com/josemalm32/bmw-cic-androidauto\r\n')
    print(f'Release zip: {rel}')
    print('Add AAHU.CRT and AAHU.KEY (your AA head-unit certificate, PEM) to the stick root.')


if __name__ == '__main__':
    main()
