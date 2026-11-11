#!/usr/bin/env python3
"""The level app on the Mac boards, checked against a board run, with no hardware.

Usage:
  mac_check.py LEVEL_MAC LEVEL_HEADLESS BOARD_RUN.json BOARD_FINAL.png BOARD_NUMERIC.png OUT_DIR

LEVEL_MAC and LEVEL_HEADLESS are mac/build/level-mac and
level-headless (or their -san builds). The check:
  1. runs tools/compare.py's script on a fresh Mac host and requires the app
     state after every step to equal the board run's;
  2. requires the Mac's final screenshot to equal the board's, pixel for pixel;
  3. draws the numeric screen (the NUMERIC steps from a fresh start) with 466-,
     32-, 7-, and 1-row bands and requires the board's pixels from each;
  4. runs the same script on the headless host and requires the same states,
     the parts it reports missing, and the no-display screenshot error;
  5. requires the `extensions` op to report the app's extension (`attitude`)
     bound, built from extensions/attitude.json as it is now (SHA-256), with
     its sources and ops; and, on the headless host started with
     --no-extensions, the boot report to name the extension, its resource to
     show its type's zero, and its op to be unknown.
A sanitizer report on any host's stderr fails the check. Exit 0 when all pass.
"""

from __future__ import annotations

import hashlib
import json
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

TOOLS = Path(__file__).parent
sys.path.insert(0, str(TOOLS))
import agent
import compare

EXTENSIONS = TOOLS.parent / "extensions"
BANDS = (466, 32, 7, 1)
NUMERIC = ({"op": "tilt", "pitch": 3.5, "roll": -1.0}, {"op": "key", "key": "Enter"},
           {"op": "tap", "id": "mode"})
# The Mac boards have no controls: the agent drives them.
MAC_MISSING = ["keys", "touch"]
HEADLESS_MISSING = ["display", "keys", "touch", "command:haptic"]


class Host:
    """A fresh Mac program process and its pseudo-terminal."""

    def __init__(self, binary: str, *args: str) -> None:
        self.process = subprocess.Popen([binary, *args], stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True)
        line = self.process.stdout.readline() if self.process.stdout else ""
        if not line.startswith("PTY "):
            raise RuntimeError(f"{Path(binary).name} did not start: {line!r} {self.stop()}")
        self.tty = line.split()[1]

    def stop(self) -> str:
        self.process.terminate()
        return self.process.communicate(timeout=10)[1]


def sanitizer_clean(stderr: str) -> bool:
    return "ERROR: AddressSanitizer" not in stderr and "runtime error:" not in stderr


def pixels(path: Path) -> list[bytes]:
    """RGB rows of a PNG that tools/agent.py wrote (8-bit RGB, no filters)."""
    data = path.read_bytes()
    pos, idat, width, height = 8, b"", 0, 0
    while pos < len(data):
        size, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + size]
        if kind == b"IHDR":
            width, height = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + size
    raw = zlib.decompress(idat)
    stride = width * 3 + 1
    return [raw[y * stride + 1:(y + 1) * stride] for y in range(height)]


def differing(a: Path, b: Path) -> int:
    rows_a, rows_b = pixels(a), pixels(b)
    if len(rows_a) != len(rows_b) or any(len(x) != len(y) for x, y in zip(rows_a, rows_b)):
        return -1
    return sum(1 for x, y in zip(rows_a, rows_b) for i in range(0, len(x), 3)
               if x[i:i + 3] != y[i:i + 3])


def screenshot(tty: str, out: Path) -> None:
    result = subprocess.run([sys.executable, "-I", str(TOOLS / "agent.py"), tty, "screenshot",
                             str(out)], capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"screenshot failed: {result.stderr.strip()}")


def state_failures(board_run: Path, run: Path) -> list[str]:
    failures = []
    board, mac = json.loads(board_run.read_text()), json.loads(run.read_text())
    for i, (b, m) in enumerate(zip(board["states"], mac["states"])):
        if b["state"] != m["state"]:
            failures.append(f"{run.name} step {i} {b['step']}: board {b['state']} / mac {m['state']}")
    if len(board["states"]) != len(mac["states"]):
        failures.append(f"{len(board['states'])} board steps, {len(mac['states'])} in {run.name}")
    missing = mac["final"].get("missing")
    want = HEADLESS_MISSING if "headless" in run.name else MAC_MISSING
    if missing != want:
        failures.append(f"{run.name}: missing parts {missing}, expected {want}")
    return failures


def check_parity(binary: str, board_run: Path, board_final: Path, out: Path) -> list[str]:
    host = Host(binary, "--band", "466")
    try:
        compare.run(host.tty, "Plan B on Mac", out / "mac.json")
        screenshot(host.tty, out / "mac-final.png")
    finally:
        stderr = host.stop()
    failures = state_failures(board_run, out / "mac.json")
    diff = differing(board_final, out / "mac-final.png")
    if diff != 0:
        failures.append(f"final screenshot: {diff} pixels differ from {board_final}")
    if not sanitizer_clean(stderr):
        failures.append(f"sanitizer report:\n{stderr}")
    return failures


def check_headless(binary: str, board_run: Path, out: Path) -> list[str]:
    host = Host(binary)
    try:
        compare.run(host.tty, "Plan B headless on Mac", out / "headless.json")
        answer = agent.Port(host.tty).request({"op": "screenshot"})
    finally:
        stderr = host.stop()
    failures = state_failures(board_run, out / "headless.json")
    if answer.get("error") != "this board has no display":
        failures.append(f"headless screenshot answered {answer}")
    if not sanitizer_clean(stderr):
        failures.append(f"headless sanitizer report:\n{stderr}")
    return failures


def boot_report(host: Host) -> list[str]:
    """The lines a headless host prints between its PTY line and BOOTED."""
    lines = []
    while host.process.stdout:
        line = host.process.stdout.readline().strip()
        if line in ("BOOTED", ""):
            break
        lines.append(line)
    return lines


def check_extensions(
    binary: str, headless: str, extension: dict, missing: list[str], *args: str
) -> list[str]:
    """Checks the `extensions` op on BINARY and a headless run without extensions.

    `extension` is the app's one extension: its name, the sources, ops and props
    it declares, a resource that reads its source and its type's zero, and one of
    its ops (a request).
    `missing` is what the headless host lacks besides the extension.
    """
    name = extension["name"]
    digest = hashlib.sha256((EXTENSIONS / f"{name}.json").read_bytes()).hexdigest()
    failures = []
    h = Host(binary, *args)
    try:
        listed = agent.Port(h.tty).request({"op": "extensions"})["extensions"]
    finally:
        stderr = h.stop()
    want = {"name": name, "version": 1, "sha256": digest, "bound": True, "same_file": True,
            "sources": extension["sources"], "ops": extension["ops"], "props": extension["props"]}
    if listed != [want]:
        failures.append(f"{Path(binary).name}: extensions {listed}, expected [{want}]")
    h = Host(headless, "--no-extensions", *args)
    try:
        port = agent.Port(h.tty)
        report = boot_report(h)
        resource = port.request({"op": "state"})["resources"][extension["resource"]]
        op = port.request(extension["request"])
    finally:
        stderr += h.stop()
    want_report = [f"MISSING {m}" for m in [*missing, f"extension:{name}"]]
    if report != want_report:
        failures.append(f"without extensions: boot report {report}, expected {want_report}")
    if resource != extension["zero"]:
        failures.append(
            f"without extensions: {extension['resource']} is {resource}, not {extension['zero']}"
        )
    if op != {"error": "unknown op"}:
        failures.append(f"without extensions: {extension['request']} answered {op}")
    if not sanitizer_clean(stderr):
        failures.append(f"extensions sanitizer report:\n{stderr}")
    return failures


def check_bands(binary: str, board_numeric: Path, out: Path) -> list[str]:
    failures = []
    for band in BANDS:
        host = Host(binary, "--band", str(band))
        try:
            port = agent.Port(host.tty)
            for request in NUMERIC:
                port.request(request)
                time.sleep(compare.SETTLE_S)
            screenshot(host.tty, out / f"numeric-{band}.png")
        finally:
            stderr = host.stop()
        if not sanitizer_clean(stderr):
            failures.append(f"band {band}: sanitizer report:\n{stderr}")
    for band in BANDS:
        diff = differing(board_numeric, out / f"numeric-{band}.png")
        if diff != 0:
            failures.append(f"numeric screen, {band}-row bands: {diff} pixels differ from "
                            f"{board_numeric}")
    return failures


def main() -> int:
    if len(sys.argv) != 7:
        print(__doc__)
        return 2
    binary, headless = sys.argv[1:3]
    board_run, board_final, board_numeric, out = (Path(a) for a in sys.argv[3:7])
    out.mkdir(parents=True, exist_ok=True)
    failures = check_parity(binary, board_run, board_final, out)
    failures += check_bands(binary, board_numeric, out)
    failures += check_headless(headless, board_run, out)
    failures += check_extensions(
        binary, headless,
        {"name": "attitude", "sources": ["attitude"], "ops": ["tilt"], "props": [],
         "resource": "tilt", "zero": {"pitch": 0, "roll": 0},
         "request": {"op": "tilt", "pitch": 1, "roll": 1}},
        HEADLESS_MISSING)
    for failure in failures:
        print(f"FAIL {failure}")
    print("mac_check: " + ("all checks pass" if not failures else f"{len(failures)} failed"))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
