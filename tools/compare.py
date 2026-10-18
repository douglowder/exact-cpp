#!/usr/bin/env python3
"""Path A vs Plan B on the same app: parity of app state and performance.

Usage:
  compare.py PORT run NAME OUT.json     # on whichever firmware is running now
  compare.py diff A.json B.json REPORT.md

`run` drives a fixed script through the agent ops both firmwares share
(`tilt` injects an attitude and stops the live IMU, so both runs see the same
values), records the app's state after every step, then times bubble moves,
screen switches, and key-to-panel latency, and reads memory and boot time.
`diff` requires identical states and writes a comparison table.
"""

from __future__ import annotations

import json
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import agent

SCRIPT = [
    ("tilt", 5.0, 0.0), ("tilt", 0.0, 0.0), ("tilt", 2.0, 0.0), ("tilt", 4.0, 0.0),
    ("key", "ArrowRight"), ("key", "Enter"), ("tilt", 0.4, -0.3), ("tilt", 0.2, 0.1),
    ("key", "Enter"), ("key", "ArrowLeft"), ("tilt", 3.0, -1.5), ("key", "Enter"),
    ("tilt", 3.5, -1.0), ("key", "Escape"), ("tap", "mode"), ("tap", "zero"),
    ("tilt", -45.0, 45.0), ("tilt", 1.25, -0.75),
]
SETTLE_S = 0.3


def step(port: agent.Port, item: tuple) -> None:
    kind = item[0]
    if kind == "tilt":
        port.request({"op": "tilt", "pitch": item[1], "roll": item[2]})
    elif kind == "key":
        port.request({"op": "key", "key": item[1]})
    else:
        port.request({"op": "tap", "id": item[1]})
    time.sleep(SETTLE_S)


def app_state(port: agent.Port) -> dict:
    s = port.request({"op": "state"})
    return {k: s[k] for k in ("slots", "derives", "resources")}


def sample(port: agent.Port, items: list[tuple]) -> list[dict]:
    out = []
    for item in items:
        step(port, item)
        out.append(port.request({"op": "stats"}))
    return out


def run(port_path: str, name: str, out: Path) -> None:
    port = agent.Port(port_path)
    list(port.lines(1.0))
    boot = port.request({"op": "stats"})
    # Start from a known state: before the first `tilt`, the live IMU ran, and
    # whatever it read may already have cued. Count cues from here on.
    step(port, ("tilt", 10.0, 10.0))
    base = app_state(port)["slots"]["cues"]
    states = []
    for item in SCRIPT:
        step(port, item)
        state = app_state(port)
        state["slots"]["cues"] -= base
        states.append({"step": list(item), "state": state})
    bubble = sample(port, [("tilt", 0.1 * (i % 7), -0.1 * (i % 5)) for i in range(30)])
    port.request({"op": "tilt", "pitch": 0.0, "roll": 0.0})
    time.sleep(SETTLE_S)
    if app_state(port)["slots"]["focus"] != "mode":
        step(port, ("key", "ArrowRight"))
    switches = sample(port, [("key", "Enter")] * 10)
    final = port.request({"op": "stats"})
    result = {"name": name, "boot": boot, "states": states, "bubble": bubble,
              "switches": switches, "final": final}
    out.write_text(json.dumps(result, indent=1))
    print(f"{name}: {len(states)} states, {len(bubble)} bubble samples, {len(switches)} switches")


def median(rows: list[dict], key: str) -> float:
    return statistics.median(r[key] for r in rows)


STATS = {"frames", "commits", "paint_us", "push_us", "damage_px", "input_to_panel_us", "ready_ms",
         "missing"}


def counters(run: dict) -> list[str]:
    return [k for k in run["final"] if k not in STATS]


def cell(value: float | None) -> str:
    return "–" if value is None else f"{value:,.0f}"


def diff(a_path: Path, b_path: Path, report: Path) -> int:
    a, b = json.loads(a_path.read_text()), json.loads(b_path.read_text())
    mismatches = [
        (i, sa["step"], sa["state"], sb["state"])
        for i, (sa, sb) in enumerate(zip(a["states"], b["states"]))
        if sa["state"] != sb["state"]
    ]
    lines = [f"# {a['name']} vs {b['name']}", ""]
    lines.append(f"State parity: {len(a['states']) - len(mismatches)} of {len(a['states'])} steps identical.")
    for i, st, sa, sb in mismatches:
        lines.append(f"- step {i} {st}: {a['name']} {json.dumps(sa)} / {b['name']} {json.dumps(sb)}")
    rows = [
        ("Bubble move: paint (µs, median)", median(a["bubble"], "paint_us"), median(b["bubble"], "paint_us")),
        ("Bubble move: push (µs, median)", median(a["bubble"], "push_us"), median(b["bubble"], "push_us")),
        ("Bubble move: damage (px, median)", median(a["bubble"], "damage_px"), median(b["bubble"], "damage_px")),
        ("Screen switch: paint (µs, median)", median(a["switches"], "paint_us"), median(b["switches"], "paint_us")),
        ("Screen switch: push (µs, median)", median(a["switches"], "push_us"), median(b["switches"], "push_us")),
        ("Key to panel (µs, median)", median(a["switches"], "input_to_panel_us"), median(b["switches"], "input_to_panel_us")),
        ("Reset to first frame (ms)", a["boot"]["ready_ms"], b["boot"]["ready_ms"]),
    ]
    # Board counters (free memory, …): each board reports its own names.
    for key in dict.fromkeys([*counters(a), *counters(b)]):
        rows.append((f"Counter `{key}` at end", a["final"].get(key), b["final"].get(key)))
    lines += ["", f"| Measure | {a['name']} | {b['name']} |", "| --- | --- | --- |"]
    lines += [f"| {label} | {cell(va)} | {cell(vb)} |" for label, va, vb in rows]
    report.write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 1 if mismatches else 0


def main() -> int:
    if len(sys.argv) == 5 and sys.argv[2] == "run":
        run(sys.argv[1], sys.argv[3], Path(sys.argv[4]))
        return 0
    if len(sys.argv) == 5 and sys.argv[1] == "diff":
        return diff(Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]))
    print(__doc__)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
