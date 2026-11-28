#!/usr/bin/env python3
"""Exercise the AirPlay host fixture with a simulated MFi signer."""
import os
import plistlib
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def request(sock, method, path, payload=b"", seq=1):
    sock.sendall(
        (f"{method} {path} RTSP/1.0\r\nCSeq: {seq}\r\n"
         f"Content-Length: {len(payload)}\r\n\r\n").encode() + payload
    )
    with sock.makefile("rb") as response:
        status = int(response.readline().split()[1])
        headers = {}
        for line in response:
            if line == b"\r\n":
                break
            key, value = line.split(b":", 1)
            headers[key.lower()] = value.strip()
        return status, response.read(int(headers.get(b"content-length", 0)))


def connect(proc):
    for _ in range(50):
        if proc.poll() is not None:
            raise RuntimeError("AirPlay test server exited")
        try:
            return socket.create_connection(("127.0.0.1", 7000), timeout=2)
        except ConnectionRefusedError:
            time.sleep(0.1)
    raise RuntimeError("AirPlay test server did not listen")


def run(binary, fail_sign):
    with tempfile.TemporaryDirectory() as work:
        root = Path(work)
        fifo = root / "knob"
        os.mkfifo(fifo)
        env = os.environ.copy()
        env.pop("CARPLAY_TEST_SIGN_FAIL", None)
        if fail_sign:
            env["CARPLAY_TEST_SIGN_FAIL"] = "1"
        with subprocess.Popen(
            [binary, work, str(root / "video"), str(fifo), work], env=env
        ) as proc:
            try:
                with connect(proc) as sock:
                    status, info = request(sock, "GET", "/info")
                    assert status == 200 and plistlib.loads(info)
                    # The standard X25519 basepoint suffices for the simulated peer.
                    challenge = b"\x01\x09" + bytes(31)
                    status, body = request(sock, "POST", "/auth-setup", challenge, 2)
                    if fail_sign:
                        assert status == 500, status
                        return
                    assert status == 200, status
                    cert_size, = struct.unpack_from(">I", body, 32)
                    assert body[36:36 + cert_size] == b"test certificate"
                    signature_size, = struct.unpack_from(">I", body, 36 + cert_size)
                    assert signature_size == 128
                    assert len(body) == 40 + cert_size + signature_size
                    status, _ = request(sock, "POST", "/auth-setup", b"\x01", 3)
                    assert status == 400, status
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == "__main__":
    run(sys.argv[1], fail_sign=False)
    run(sys.argv[1], fail_sign=True)
    print("AirPlay /info, simulated MFi authentication, malformed request and signer failure passed")
