#!/usr/bin/env python3
"""An app's sounds on the headless Mac board against Exact's runner.

Usage:
  sound_check.py APP_HEADLESS RUNNER_RUN.json

RUNNER_RUN.json is sound-ref's run of an app's steps on Exact's runner (`cargo run -p
sound-ref -- apps/metronome/app.contract apps/metronome/steps.json metronome`). The board
runs on its manual clock (`--manual-clock`), which starts at 0 as the runner's does and moves
only by the agent's `clock` op. The check:
  1. replays the steps (a tap, a key, or the clock moving on) and requires the app's slots to
     equal the runner's after boot and after every step;
  2. requires the voices that sound to equal the runner's: each one's sound, gain, and start
     within 0.001 ms, in order. A voice that ended as `edit` or `cancelled` never sounded and
     is left out on both sides;
  3. requires the boot report to name the missing `speaker` (the Mac board has none).
A sanitizer report on stderr fails the check. Exit 0 when all pass.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

TOOLS = Path(__file__).parent
sys.path.insert(0, str(TOOLS))
import agent
import mac_check

SILENT = {"edit", "cancelled", "cut", "clock"}
TOLERANCE_MS = 0.001


def sounding(voices: list[dict]) -> list[tuple[str, float, float]]:
    """The voices that sound, as (src, start ms, gain), in start order."""
    out = [(v["src"], v["at"], v["gain"]) for v in voices if v["by"] not in SILENT]
    return sorted(out, key=lambda v: v[1])


def request(port: agent.Port, op: str, arg: str) -> dict:
    if op == "key":
        return port.request({"op": "key", "key": arg})
    if op == "tap":
        return port.request({"op": "tap", "id": arg})
    return port.request({"op": "clock", "ms": float(arg)})


def check_steps(port: agent.Port, steps: list[dict]) -> list[str]:
    failures = []
    for i, want in enumerate(steps):
        if i > 0:
            op, arg = want["step"]
            answer = request(port, op, arg)
            if "error" in answer:
                failures.append(f"step {i} {want['step']}: {answer['error']}")
        got = port.request({"op": "state"})["slots"]
        if got != want["slots"]:
            failures.append(
                f"step {i} {want['step']} slots:\n  runner {want['slots']}\n  board  {got}"
            )
    return failures


def check_voices(want: list[dict], got: list[dict]) -> list[str]:
    runner, board = sounding(want), sounding(got)
    if len(runner) != len(board):
        counts = f"{len(board)} voices sound on the board, {len(runner)} on the runner"
        return [f"{counts}:\n  runner {runner}\n  board  {board}"]
    failures = []
    for i, (r, b) in enumerate(zip(runner, board, strict=True)):
        if r[0] != b[0] or abs(r[1] - b[1]) > TOLERANCE_MS or abs(r[2] - b[2]) > 1e-9:
            failures.append(f"voice {i + 1}: runner {r}, board {b}")
    return failures


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    binary, run = sys.argv[1], json.loads(Path(sys.argv[2]).read_text())
    host = mac_check.Host(binary, "--manual-clock")
    failures = []
    try:
        report = mac_check.boot_report(host)
        if not any("speaker" in line for line in report):
            failures.append(f"the boot report does not name the speaker: {report}")
        port = agent.Port(host.tty)
        failures += check_steps(port, run["steps"])
        failures += check_voices(
            run["voices"], port.request({"op": "sounds"})["voices"]
        )
    finally:
        stderr = host.stop()
    if not mac_check.sanitizer_clean(stderr):
        failures.append(f"sanitizer report:\n{stderr}")
    for f in failures:
        print(f"FAIL {f}")
    count = len(sounding(run["voices"]))
    print(
        f"sound_check: {count} voices, {len(run['steps'])} states: "
        + ("all checks pass" if not failures else f"{len(failures)} failed")
    )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
