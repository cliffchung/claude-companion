#!/usr/bin/env python3
"""Grab a JPEG screenshot from the Tab5 over the USB debug console."""
import argparse
import base64
import binascii
import os
import sys
import time
from typing import Iterable, Tuple

DEFAULT_PORT = "/dev/cu.usbmodem211201"


def decode_frame(lines: Iterable[str]) -> Tuple[bytes, int, int]:
    """Decode a '@@SHOT jpeg <len> <w> <h>' ... '@@END' frame; ignore other lines."""
    header = None
    chunks = []
    ended = False
    for raw in lines:
        line = raw.strip()
        if header is None:
            if line.startswith("@@SHOT "):
                parts = line.split()
                if len(parts) != 5 or parts[1] != "jpeg":
                    raise ValueError("bad header: %r" % line)
                header = (int(parts[2]), int(parts[3]), int(parts[4]))
            continue
        if line == "@@END":
            ended = True
            break
        chunks.append(line)
    if header is None:
        raise ValueError("no @@SHOT header")
    if not ended:
        raise ValueError("missing @@END")
    try:
        data = base64.b64decode("".join(chunks), validate=True)
    except binascii.Error as e:
        raise ValueError("bad base64: %s" % e)
    if len(data) != header[0]:
        raise ValueError("length mismatch: header %d, decoded %d" % (header[0], len(data)))
    return data, header[1], header[2]


def grab(port: str, timeout: float = 20.0):
    import serial  # pyserial
    lines = []
    with serial.Serial(port, 115200, timeout=0.2) as s:
        s.reset_input_buffer()
        s.write(b"\nscreenshot\n")
        deadline = time.time() + timeout
        buf = b""
        while time.time() < deadline:
            buf += s.read(65536)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("ascii", "replace").strip()
                lines.append(text)
                if text == "@@END":
                    return decode_frame(lines)
    raise ValueError("timeout waiting for @@END")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--out")
    a = ap.parse_args()
    out = a.out
    if not out:
        d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "screenshots")
        out = os.path.normpath(os.path.join(d, time.strftime("shot-%Y%m%d-%H%M%S.jpg")))
    try:
        data, w, h = grab(a.port)
    except ValueError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(data)
    print("saved %s %dx%d" % (out, w, h))
    return 0


if __name__ == "__main__":
    sys.exit(main())
