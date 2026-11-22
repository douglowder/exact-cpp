#!/usr/bin/env python3
"""Installs the stream firmware on a SenseCAP Watcher and leaves its current
firmware where it is.

The Watcher this was tried on reads its partition table at 0x10000 (ota_0 holds the
firmware it boots). The Arduino-built stream app reads a table at 0x8000, the
offset its prebuilt libraries were compiled with, so the app gets a table of
its own there: the same otadata and app slots, and an `nvs` carved from the
unused tail of the slot it runs in. Arduino's startup erases an `nvs` it cannot
read, so the app must never be shown the real NVS.

Writes, and nothing else:
  0x8000             the app's table (the sector the bootloader no longer reads)
  the other OTA slot the app
  one otadata sector boot that slot
`flash.py PORT --back` writes one otadata sector to boot the first slot again.

  flash.py PORT APP.bin [--table 0x10000]
  flash.py PORT --back [--table 0x10000]
"""
import glob, os, struct, subprocess, sys, tempfile, zlib

HW = os.path.expanduser("~/Library/Arduino15/packages/esp32")
ESPTOOL = sorted(glob.glob(HW + "/tools/esptool_py/*/esptool"))[-1]
GEN = sorted(glob.glob(HW + "/hardware/esp32/*/tools/gen_esp32part.py"))[-1]
APP_TABLE = 0x8000
SECTOR = 0x1000
APP_NVS = 0x10000


def esptool(port, *args):
    cmd = [ESPTOOL, "-p", port, "-b", "460800", *args]
    print("+ esptool", " ".join(args), flush=True)
    subprocess.run(cmd, check=True)


def read(port, off, size, tmp):
    path = os.path.join(tmp, f"r{off:x}.bin")
    for attempt in range(4):
        try:
            esptool(port, "--after", "no-reset", "read-flash", hex(off), hex(size), path)
            return open(path, "rb").read()
        except subprocess.CalledProcessError:
            print("read failed, retrying", flush=True)
    sys.exit("cannot read the board")


def table(data):
    parts = {}
    for i in range(0, 0xC00, 32):
        e = data[i:i + 32]
        if e[:2] != b"\xaa\x50":
            break
        t, st, off, size = struct.unpack("<BBII", e[2:12])
        parts[e[12:28].rstrip(b"\0").decode()] = (t, st, off, size)
    return parts


def ota_entries(data):
    out = []
    for i in (0, 1):
        seq, _, state, crc = struct.unpack("<I20sII", data[i * SECTOR:i * SECTOR + 32])
        ok = seq != 0xFFFFFFFF and crc == zlib.crc32(struct.pack("<I", seq), 0xFFFFFFFF)
        out.append((seq, state, ok))
    return out


def main():
    port, what = sys.argv[1], sys.argv[2]
    boot_table = int(sys.argv[sys.argv.index("--table") + 1], 0) if "--table" in sys.argv else 0x10000
    with tempfile.TemporaryDirectory() as tmp:
        parts = table(read(port, boot_table, 0xC00, tmp))
        need = ["otadata", "ota_0", "ota_1"]
        if any(n not in parts for n in need):
            sys.exit(f"no OTA layout in the table at {boot_table:#x}: {parts}")
        ota_off = parts["otadata"][2]
        slots = [parts["ota_0"], parts["ota_1"]]
        ent = ota_entries(read(port, ota_off, 2 * SECTOR, tmp))
        valid = [(seq, i) for i, (seq, _, ok) in enumerate(ent) if ok]
        if not valid:
            sys.exit(f"no valid otadata entry at {ota_off:#x}; refusing to guess")
        top_seq, top_sector = max(valid)
        booting = (top_seq - 1) % 2
        print(f"table at {boot_table:#x}; otadata {ent}; booting ota_{booting}", flush=True)
        # Installing: the stream app goes to ota_1 (ota_0 keeps the original
        # firmware). If ota_1 is already booting, it is the stream app: update it.
        target = 0 if what == "--back" else 1
        if what != "--back":
            image = what
            _, _, toff, tsize = slots[target]
            if os.path.getsize(image) > tsize - APP_NVS:
                sys.exit(f"{image} does not fit ota_{target}")
            # The app's table.
            csv = os.path.join(tmp, "app.csv")
            o0, s0 = slots[0][2], slots[0][3]
            with open(csv, "w") as f:
                f.write("# The stream app's view (tools/flash.py)\n")
                f.write(f"otadata, data, ota, {ota_off:#x}, {2 * SECTOR:#x}\n")
                f.write(f"ota_0, app, ota_0, {o0:#x}, {s0:#x}\n")
                f.write(f"ota_1, app, ota_1, {toff:#x}, {tsize - APP_NVS:#x}\n")
                f.write(f"nvs, data, nvs, {toff + tsize - APP_NVS:#x}, {APP_NVS:#x}\n")
            binp = os.path.join(tmp, "app_table.bin")
            subprocess.run([sys.executable, GEN, "-q", csv, binp], check=True)
            print(open(csv).read(), flush=True)
            esptool(port, "--after", "no-reset", "write-flash", hex(APP_TABLE), binp)
            esptool(port, "--after", "no-reset", "write-flash", hex(toff), image)
        if booting == target:
            esptool(port, "run")
            print(f"updated ota_{target}, which was booting already", flush=True)
            return
        seq = top_seq + 1
        while (seq - 1) % 2 != target:
            seq += 1
        entry = struct.pack("<I20sII", seq, b"\xff" * 20, 0xFFFFFFFF, zlib.crc32(struct.pack("<I", seq), 0xFFFFFFFF))
        spath = os.path.join(tmp, "otadata_sector.bin")
        open(spath, "wb").write(entry + b"\xff" * (SECTOR - len(entry)))
        esptool(port, "write-flash", hex(ota_off + (1 - top_sector) * SECTOR), spath)
        print(f"booting ota_{target} (otadata seq {seq})", flush=True)


main()
