#!/usr/bin/env python3
"""Writes the made-up badge profile in the badge firmware's record format.

Usage:
  badge_profile.py FIXTURE.json OUT.bin

FIXTURE.json is apps/badge/fixture/data.json; its "profile" gives the name,
company, network, and handle. The record is version 3 of
conference-manual-v1.bin: a 64-byte header ("INITCF01", version, 64, 160,
160, metadata length, image length, zeros, SHA-256 over the first 32 bytes,
the metadata, and the pixels), the metadata (name, network, handle, company,
each a little-endian u16 length and UTF-8), and a 160 x 160 RGB565 photo,
little-endian. The photo is a generated pattern, not a person.
"""

from __future__ import annotations

import hashlib
import json
import struct
import sys
from pathlib import Path

SIDE = 160


def photo() -> bytes:
    """Concentric bands of color: plainly not a person, easy to recognize."""
    out = bytearray()
    for y in range(SIDE):
        for x in range(SIDE):
            d = ((x - 80) ** 2 + (y - 80) ** 2) ** 0.5
            band = int(d) // 16
            r, g, b = [(40, 90, 200), (240, 180, 40), (30, 30, 30), (220, 70, 90)][band % 4]
            out += struct.pack("<H", (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3)
    return bytes(out)


def record(profile: dict) -> bytes:
    metadata = b""
    for field in (profile["name"], profile["network"], profile["handle"], profile["company"]):
        data = field.encode("utf-8")
        metadata += struct.pack("<H", len(data)) + data
    pixels = photo() if profile.get("photo") else b""
    head = b"INITCF01" + struct.pack("<HHHHII", 3, 64, SIDE, SIDE, len(metadata), len(pixels)) + bytes(8)
    digest = hashlib.sha256(head + metadata + pixels).digest()
    return head + digest + metadata + pixels


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    fixture = json.loads(Path(sys.argv[1]).read_text())
    Path(sys.argv[2]).write_bytes(record(fixture["profile"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
