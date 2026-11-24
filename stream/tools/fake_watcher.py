#!/usr/bin/env python3
"""A Watcher on a pseudo-terminal, for testing exact-stream without the board.

It speaks the stream protocol (proto/exact_stream_proto.h) exactly as the
firmware does: answers HELLO with INFO, decodes each AREA into its own copy of
the screen and ACKs it, and on request writes that copy as a PNG. Inputs typed
on stdin go to the host: `key left|right|enter|escape|next`, `tap X Y`.

  fake_watcher.py OUT.png [SECONDS]
prints `PTY /dev/ttysNNN`; point exact-stream --port at it.
"""
import os, pty, select, struct, sys, time, tty, zlib

W = H = 412
fb = bytearray(W * H * 2)


def packet(t, payload=b""):
    n = len(payload)
    body = bytes([t, n & 255, (n >> 8) & 255, n >> 16]) + payload
    return b"\xa5\x5a" + body + struct.pack("<I", zlib.crc32(body))


def png(path):
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        for x in range(W):
            c = (fb[(y * W + x) * 2] << 8) | fb[(y * W + x) * 2 + 1]
            r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
            raw += bytes([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)])
    def chunk(k, d):
        return struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def area(p):
    x, y, w, h, seq, enc = struct.unpack("<HHHHHB", p[:11])
    d = p[11:]
    total = w * h
    px = 0
    def put(i, hi, lo):
        r, c = divmod(i, w)
        o = ((y + r) * W + x + c) * 2
        fb[o], fb[o + 1] = hi, lo
    ok = x + w <= W and y + h <= H
    if enc == 0:
        for i in range(total):
            put(i, d[2 * i], d[2 * i + 1])
        px = total
    else:
        i = 0
        while i < len(d) and ok:
            op, n = d[i] >> 6, d[i] & 63
            i += 1
            if n == 63:
                n = d[i] | (d[i + 1] << 8)
                i += 2
            else:
                n += 1
            if px + n > total:
                ok = False
                break
            if op == 0:
                px += n
            elif op == 1:
                hi, lo = d[i], d[i + 1]
                i += 2
                for k in range(n):
                    put(px + k, hi, lo)
                px += n
            elif op == 2:
                for k in range(n):
                    put(px + k, d[i + 2 * k], d[i + 2 * k + 1])
                i += 2 * n
                px += n
            else:
                ok = False
    ok = ok and px == total
    return seq, ok, w, h, len(p)


def main():
    out = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 1e9
    master, slave = pty.openpty()
    tty.setraw(slave)
    os.set_blocking(master, False)
    print("PTY", os.ttyname(slave), flush=True)
    buf = bytearray()
    areas = 0
    start = time.time()
    os.write(master, b"boot chatter\r\n" + packet(0x81, struct.pack("<HHHH", W, H, 1, 0)))
    keys = {"left": 1, "right": 2, "enter": 3, "escape": 4, "next": 5}
    while time.time() - start < secs:
        r, _, _ = select.select([master, sys.stdin], [], [], 0.05)
        if sys.stdin in r:
            line = sys.stdin.readline().split()
            if not line:
                return
            if line[0] == "key":
                os.write(master, packet(0x83, struct.pack("<BhhB", 0, 0, 0, keys[line[1]])))
            elif line[0] == "tap":
                x, y = int(line[1]), int(line[2])
                os.write(master, packet(0x83, struct.pack("<BhhB", 1, x, y, 0)))
                time.sleep(0.05)
                os.write(master, packet(0x83, struct.pack("<BhhB", 3, x, y, 0)))
            elif line[0] == "png":
                png(line[1] if len(line) > 1 else out)
                print("PNG", flush=True)
        if master in r:
            try:
                buf += os.read(master, 1 << 20)
            except BlockingIOError:
                pass
        while len(buf) >= 10:
            j = buf.find(b"\xa5\x5a")
            if j < 0:
                buf.clear()
                break
            del buf[:j]
            if len(buf) < 6:
                break
            n = buf[3] | (buf[4] << 8) | (buf[5] << 16)
            if len(buf) < n + 10:
                break
            body, crc = bytes(buf[2:6 + n]), struct.unpack("<I", buf[6 + n:10 + n])[0]
            del buf[:n + 10]
            if zlib.crc32(body) != crc:
                print("bad crc", flush=True)
                continue
            t, p = body[0], body[4:]
            if t == 0x01:
                os.write(master, packet(0x81, struct.pack("<HHHH", W, H, 1, 0)))
            elif t == 0x10:
                seq, ok, w, h, size = area(p)
                areas += 1
                print(f"AREA seq={seq} ok={ok} {w}x{h} bytes={size}", flush=True)
                os.write(master, packet(0x82, struct.pack("<HBII", seq, 1 if ok else 0, 1000, 1000)))
    png(out)
    print("DONE areas", areas, flush=True)


main()
