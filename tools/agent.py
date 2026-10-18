#!/usr/bin/env python3
"""Drive the Contract app on the Stopwatch over USB serial (JSON agent lines).

Usage:
  agent.py PORT state | stats | tree
  agent.py PORT key Enter            # ArrowLeft, ArrowRight, Enter, Escape
  agent.py PORT tap zero             # a testId
  agent.py PORT screenshot OUT.png   # the canvas, CRC-checked
  agent.py PORT raw '{"op":"logs"}'

The port is opened without touching DTR/RTS: on the ESP32-S3's
USB-Serial/JTAG those lines reset the chip or select download mode.
"""

from __future__ import annotations

import base64
import json
import os
import re
import select
import struct
import sys
import termios
import time
import zlib
from pathlib import Path


class Port:
    def __init__(self, path: str) -> None:
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = attrs[1] = attrs[3] = 0
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.buffer = b""

    def send(self, line: str) -> None:
        os.write(self.fd, (line + "\n").encode())

    def lines(self, seconds: float):
        end = time.time() + seconds
        while time.time() < end:
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if ready:
                try:
                    self.buffer += os.read(self.fd, 65536)
                except BlockingIOError:
                    pass
            while b"\n" in self.buffer:
                raw, self.buffer = self.buffer.split(b"\n", 1)
                yield raw.decode(errors="replace").rstrip("\r")

    def request(self, payload: dict, seconds: float = 3.0) -> dict:
        list(self.lines(0.1))
        self.send(json.dumps(payload))
        for line in self.lines(seconds):
            if line.startswith("CE_AGENT "):
                return json.loads(line[len("CE_AGENT "):])
        raise TimeoutError(f"no answer to {payload}")


def write_png(path: Path, width: int, height: int, rgb565be: bytes) -> None:
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            i = (y * width + x) * 2
            v = (rgb565be[i] << 8) | rgb565be[i + 1]
            r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
            rows += bytes(((r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31))

    def chunk(kind: bytes, data: bytes) -> bytes:
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def screenshot(port: Port, out: Path) -> dict:
    list(port.lines(0.1))
    port.send(json.dumps({"op": "screenshot"}))
    begin = None
    data = bytearray()
    for line in port.lines(60):
        if line.startswith("CE_SHOT begin"):
            begin = dict(re.findall(r"(\w+)=(\S+)", line))
            data = bytearray(int(begin["bytes"]))
        elif line.startswith("CE_SHOT_DATA ") and begin:
            _, offset, payload = line.split(" ", 2)
            piece = base64.b64decode(payload)
            data[int(offset) : int(offset) + len(piece)] = piece
        elif line == "CE_SHOT end":
            break
        elif line.startswith("CE_AGENT ") and begin is None:
            answer = json.loads(line[len("CE_AGENT ") :])
            raise RuntimeError(f"no screenshot: {answer.get('error', answer)}")
    if begin is None:
        raise TimeoutError("no screenshot")
    crc = f"{zlib.crc32(bytes(data)):08x}"
    if crc != begin["crc32"]:
        raise RuntimeError(f"screenshot CRC {crc} != {begin['crc32']}")
    write_png(out, int(begin["width"]), int(begin["height"]), bytes(data))
    return {"path": str(out), "crc32": crc}


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    port = Port(sys.argv[1])
    verb, args = sys.argv[2], sys.argv[3:]
    if verb in ("state", "stats", "tree"):
        result = port.request({"op": verb})
    elif verb == "key":
        result = port.request({"op": "key", "key": args[0]})
    elif verb == "tap":
        result = port.request({"op": "tap", "id": args[0]})
    elif verb == "screenshot":
        result = screenshot(port, Path(args[0]))
    elif verb == "raw":
        result = port.request(json.loads(args[0]))
    else:
        print(__doc__)
        return 2
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
