#!/usr/bin/env python3
"""The badge app on the Mac boards against Exact's runner.

Usage:
  badge_check.py BADGE_MAC BADGE_HEADLESS RUNNER_RUN.json OUT_DIR

RUNNER_RUN.json is badge-ref's run of apps/badge/steps.json on Exact's
runner (`cargo run -p badge-ref -- apps/badge/app.contract
apps/badge/steps.json apps/badge/fixture/data.json`). Every host starts on the
fixture's clock (`--clock`), and its `init-badge` extension answers from the fixture.
The check:
  1. replays the steps on each Mac board and requires the app state (slots,
     derives, resources) and every testId node (text, inactive) to equal the
     runner's after boot and after every step;
  2. checks the host rules the runner leaves to hosts, on both boards: Escape
     on a modal route presses its Back control, a newly shown autofocus
     control (the Reset modal's Cancel) takes the focus and Enter presses it,
     a `setBrightness` send reaches the extension (`panel`), and a location
     no route matches opens the not-found modal with nothing refused in the
     journal (`logs`), where Escape presses its Back;
  3. on the board with a display, recognizes gestures from raw contact phases
     (`pointer`): a horizontal drag turns the page, a vertical drag on the
     Schedule snaps to a card (its screen equals the reference of the same
     pan sent as events), a drag on the Settings menu scrolls it, and a
     contact that stays within the slop presses;
  4. on the board with a display, draws the Connect modal with 466-, 32-, 7-,
     and 1-row bands and requires the same pixels;
  5. on the board with a display, draws each page state (PAGES) with 466- and
     7-row bands, requires the same pixels, and requires each to equal its
     reference in comparison/badge-pages/ (written to OUT_DIR on a
     difference, for review);
  6. requires the `extensions` op to report the app's extension
     (`init-badge`) bound and built from extensions/init-badge.json as it is
     now, and a headless run with --no-extensions to name it in the boot
     report, answer none for its sources, and not know its `panel` op.
A sanitizer report on any host's stderr fails the check. Exit 0 when all pass.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

TOOLS = Path(__file__).parent
sys.path.insert(0, str(TOOLS))
import agent
import mac_check

FIXTURE = json.loads(
    (TOOLS.parent / "apps" / "badge" / "fixture" / "data.json").read_text()
)
CLOCK = f"{FIXTURE['epoch_ms']},{FIXTURE['utc_offset']}"
BANDS = (466, 32, 7, 1)
MODAL = (("key", "ArrowLeft"), ("tap", "menu-phone"), ("tap", "connect-phone"))
REFERENCES = TOOLS.parent / "comparison" / "badge-pages"
# Each page state: its name and the steps from the one before. The fixture's
# clock is 9:45 on 2026-10-08, so the keynote is on and the invitation open.
PAGES = (
    ("init", ()),
    ("schedule", (("key", "ArrowRight"),)),
    ("schedule-snapped", (("pan", "sessions 0 -130"), ("panrelease", "sessions 0 0"))),
    ("party", (("key", "ArrowRight"),)),
    ("badge", (("key", "ArrowRight"),)),
    ("settings", (("key", "ArrowRight"),)),
    (
        "settings-scrolled",
        (("pan", "settings-menu 0 -200"), ("panrelease", "settings-menu 0 0")),
    ),
    ("settings-brightness", (("tap", "menu-brightness"),)),
    (
        "settings-orientation",
        (("tap", "menu-done"), ("tap", "menu-orientation"), ("tap", "orientation-180")),
    ),
    ("settings-hack", (("tap", "menu-done"), ("tap", "menu-hack"))),
    ("setup", (("tap", "menu-done"), ("tap", "menu-phone"), ("tap", "connect-phone"))),
    ("reset", (("tap", "setup-done"), ("navigate", "/settings/reset"))),
    ("reset-complete", (("tap", "reset-confirm"),)),
    ("party-locked", (("tap", "reset-finish"), ("navigate", "/party"))),
    ("not-found", (("navigate", "/nowhere"),)),
)


def host(binary: str, *args: str) -> mac_check.Host:
    return mac_check.Host(binary, "--clock", CLOCK, *args)


def request(port: agent.Port, op: str, arg: str) -> dict:
    if op == "key":
        return port.request({"op": "key", "key": arg})
    if op == "tap":
        return port.request({"op": "tap", "id": arg})
    if op in ("pan", "panrelease"):
        target, a, b = arg.split(" ")
        names = ("vx", "vy") if op == "panrelease" else ("dx", "dy")
        return port.request(
            {"op": op, "id": target, names[0]: float(a), names[1]: float(b)}
        )
    return port.request({"op": "navigate", "url": arg})


def snapshot(port: agent.Port) -> dict:
    state = port.request({"op": "state"})
    return {
        "state": {k: state[k] for k in ("slots", "derives", "resources")},
        "nodes": port.request({"op": "nodes"})["nodes"],
    }


def top(state: dict) -> list[str]:
    nav = state["slots"]["nav"]
    stack = next(t["stack"] for t in nav["tabs"] if t["name"] == nav["tab"])
    return [e["name"] for e in stack]


def check_steps(port: agent.Port, name: str, reference: list[dict]) -> list[str]:
    failures = []
    for i, want in enumerate(reference):
        if i > 0:
            op, arg = want["step"]
            request(port, op, arg)
        got = snapshot(port)
        for part in ("state", "nodes"):
            if got[part] != want[part]:
                failures.append(
                    f"{name} step {i} {want['step']} {part}:\n  runner {want[part]}\n  "
                    f"board  {got[part]}"
                )
    return failures


def check_host_rules(binary: str, name: str, *args: str) -> list[str]:
    failures = []
    h = host(binary, *args)
    try:
        port = agent.Port(h.tty)
        for op, arg in MODAL:
            request(port, op, arg)
        port.request({"op": "key", "key": "Escape"})
        if top(snapshot(port)["state"]) != ["settings"]:
            failures.append(f"{name}: Escape on the Connect modal did not press Back")
        request(port, "navigate", "/settings/reset")
        port.request({"op": "key", "key": "Enter"})
        if top(snapshot(port)["state"]) != ["settings"]:
            failures.append(
                f"{name}: Enter did not press the autofocused Cancel control"
            )
        request(port, "tap", "menu-done")
        request(port, "tap", "menu-brightness")
        request(port, "tap", "brightness-down")
        level = port.request({"op": "panel"})["brightness"]
        if level != FIXTURE["settings"]["brightness"] - 10:
            failures.append(
                f"{name}: the extension's brightness is {level} after Decrease"
            )
        request(port, "navigate", "/nowhere")
        if top(snapshot(port)["state"]) != ["settings", "notfound"]:
            failures.append(f"{name}: /nowhere did not open the not-found modal")
        lines = port.request({"op": "logs"})["lines"]
        if any("refused" in line for line in lines):
            failures.append(f"{name}: logs {lines}, expected no refusal")
        port.request({"op": "key", "key": "Escape"})
        if top(snapshot(port)["state"]) != ["settings"]:
            failures.append(f"{name}: Escape on the not-found modal did not press Back")
    finally:
        stderr = h.stop()
    if not mac_check.sanitizer_clean(stderr):
        failures.append(f"{name} sanitizer report:\n{stderr}")
    return failures


def drag(
    port: agent.Port, start: tuple[int, int], end: tuple[int, int], lift: bool = True
) -> None:
    port.request({"op": "pointer", "phase": "down", "x": start[0], "y": start[1]})
    for k in range(1, 9):
        x = start[0] + (end[0] - start[0]) * k // 8
        y = start[1] + (end[1] - start[1]) * k // 8
        port.request({"op": "pointer", "phase": "move", "x": x, "y": y})
    if lift:
        port.request({"op": "pointer", "phase": "up", "x": end[0], "y": end[1]})


def check_gestures(binary: str, out: Path) -> list[str]:
    failures = []
    h = host(binary, "--band", "466")
    try:
        port = agent.Port(h.tty)

        def state() -> dict:
            return port.request({"op": "state"})

        drag(port, (300, 250), (150, 255))
        if state()["slots"]["nav"]["tab"] != "schedule":
            failures.append("a drag 150 px left on init did not turn to the Schedule")
        drag(port, (234, 330), (234, 200))
        mac_check.screenshot(h.tty, out / "gesture-snapped.png")
        diff = mac_check.differing(
            REFERENCES / "schedule-snapped.png", out / "gesture-snapped.png"
        )
        if diff != 0:
            failures.append(
                f"a 130 px drag up on the Schedule: {diff} pixels differ from schedule-snapped"
            )
        drag(port, (150, 250), (300, 250))
        if state()["slots"]["nav"]["tab"] != "init":
            failures.append(
                "a drag 150 px right on the Schedule list did not turn back to init"
            )
        port.request({"op": "key", "key": "ArrowLeft"})
        drag(port, (234, 350), (234, 150))
        port.request({"op": "pointer", "phase": "down", "x": 234, "y": 376})
        port.request({"op": "pointer", "phase": "move", "x": 237, "y": 378})
        port.request({"op": "pointer", "phase": "up", "x": 237, "y": 378})
        if top(state()) != ["settings", "reset"]:
            failures.append(
                "a tap on the scrolled Settings menu's Reset row did not open Reset"
            )
    finally:
        stderr = h.stop()
    if not mac_check.sanitizer_clean(stderr):
        failures.append(f"gestures sanitizer report:\n{stderr}")
    return failures


def check_bands(binary: str, out: Path) -> list[str]:
    failures = []
    for band in BANDS:
        h = host(binary, "--band", str(band))
        try:
            port = agent.Port(h.tty)
            for op, arg in MODAL:
                request(port, op, arg)
            mac_check.screenshot(h.tty, out / f"badge-modal-{band}.png")
        finally:
            stderr = h.stop()
        if not mac_check.sanitizer_clean(stderr):
            failures.append(f"band {band}: sanitizer report:\n{stderr}")
    for band in BANDS[1:]:
        diff = mac_check.differing(
            out / "badge-modal-466.png", out / f"badge-modal-{band}.png"
        )
        if diff != 0:
            failures.append(
                f"badge modal, {band}-row bands: {diff} pixels differ from 466-row bands"
            )
    return failures


def render_pages(binary: str, band: int, out: Path) -> list[str]:
    h = host(binary, "--band", str(band))
    try:
        port = agent.Port(h.tty)
        for name, steps in PAGES:
            for op, arg in steps:
                request(port, op, arg)
            mac_check.screenshot(h.tty, out / f"page-{name}-{band}.png")
    finally:
        stderr = h.stop()
    if mac_check.sanitizer_clean(stderr):
        return []
    return [f"pages, band {band}: sanitizer report:\n{stderr}"]


def check_pages(binary: str, out: Path) -> list[str]:
    failures = render_pages(binary, 466, out) + render_pages(binary, 7, out)
    for name, _ in PAGES:
        drawn = out / f"page-{name}-466.png"
        diff = mac_check.differing(drawn, out / f"page-{name}-7.png")
        if diff != 0:
            failures.append(
                f"page {name}: {diff} pixels differ between 466- and 7-row bands"
            )
        reference = REFERENCES / f"{name}.png"
        if not reference.exists():
            failures.append(f"page {name}: no reference {reference}; review {drawn}")
            continue
        diff = mac_check.differing(reference, drawn)
        if diff != 0:
            failures.append(
                f"page {name}: {diff} pixels differ from {reference}; review {drawn}"
            )
    return failures


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    mac, headless, run, out = (
        sys.argv[1],
        sys.argv[2],
        Path(sys.argv[3]),
        Path(sys.argv[4]),
    )
    out.mkdir(parents=True, exist_ok=True)
    reference = json.loads(run.read_text())["steps"]
    failures = []
    for binary, name, args in (
        (mac, "badge-mac", ("--band", "466")),
        (headless, "badge-headless", ()),
    ):
        h = host(binary, *args)
        try:
            failures += check_steps(agent.Port(h.tty), name, reference)
        finally:
            stderr = h.stop()
        if not mac_check.sanitizer_clean(stderr):
            failures.append(f"{name} sanitizer report:\n{stderr}")
        failures += check_host_rules(binary, name, *args)
    failures += check_gestures(mac, out)
    failures += check_bands(mac, out)
    failures += check_pages(mac, out)
    failures += mac_check.check_extensions(
        mac,
        headless,
        {
            "name": "init-badge",
            "sources": [
                "badgeProfile",
                "badgeSettings",
                "badgeBattery",
                "setBrightness",
            ],
            "ops": ["panel"],
            "props": [],
            "resource": "battery",
            "zero": {"percent": 0},
            "request": {"op": "panel"},
        },
        ["display", "keys", "touch"],
        "--clock",
        CLOCK,
    )
    for failure in failures:
        print(f"FAIL {failure}")
    print(
        f"badge_check: {len(reference)} runner states on 2 boards; "
        + ("all checks pass" if not failures else f"{len(failures)} failed")
    )
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
