# contract-cpp

Contract apps compiled to C++. exact2's compiler makes the plan and `plan2cpp` writes the app
as C++ tables and functions. The `rt` core (`contract_rt`, standard C++17) runs the app through
a board interface, `rt::Board`. No Rust runs on a board.

This repo has the parts that know no board: `plan2cpp`, the core, the Mac boards, the Mac
container, the apps and their references on exact2's runner. Each hardware target is its own
repo, with its board and sketches, and has this repo as a submodule. The first one is
contract-stopwatch (the M5Stack Stopwatch, an ESP32-S3).

## Layout

| Path | What it is |
| --- | --- |
| `exact2/` | exact2, as a submodule (the compiler, plan, runner and kernel) |
| `apps/<app>/app.contract` | The app, in Contract (`level`, `badge`, `metronome`) |
| `apps/<app>/cpp/` | The app's C++: the generated `app_gen.cpp` and extension headers, and its glue |
| `apps/{level,badge}/extensions.json` | The host extensions the app uses, by name and version |
| `apps/<app>/app.tamber` | The app's colours as design tokens (exact2 LLP 1121), `scheme=fixed` |
| `apps/level/app.test.contract` | Authored tests (`contract verify`) |
| `apps/badge/` | The init() badge's pages and modals as routes; `assets/` (fonts, SIL OFL 1.1, and marks; `README.md` has their sources); `fixture/`, a made-up badge the Mac boards and `badge-ref` answer from; `steps.json`, its parity script |
| `apps/metronome/` | A minimal metronome from exact2's `apps/metronome`: a tempo, Start and Stop, an accented beat 1; one `sequence` plays two clicks (`assets/`, made by `tools/make_clicks.py`) |
| `extensions/` | Host extension files (prototype): `attitude.json` (level), `init-badge.json` (badge) |
| `plan2cpp/` | Compiles a plan to C++ with the app's fonts, PNG images, design tokens and sounds; refuses the rest by name; checks the app's sources against its extensions and writes each extension's C++ base class |
| `rt/` | The core: values, layout, paint, timers, the transport that plays a `sequence`, the agent; the `Board` interface and its defaults. An Arduino library (`library.properties`) |
| `rt-mac/` | The Mac boards: `Board` (a software RGB565 surface in bands, the M5GFX FreeSans data, a pseudo-terminal link) and `HeadlessBoard` (link and clock only); a manual clock; a CoreAudio speaker |
| `mac/` | The Mac programs for each app, `-mac` and `-headless`; and `ContractMac` (`container.mm`), one window that runs any app's bundle from its App menu |
| `rt-test/` | `route_check` (the C++ route port against exact2's route corpus), `ext_check` (extension binding) and `data_check` (`data-*` props, on the test app in `marks/`) |
| `level-data/`, `level-tests/` | The level app on exact2's runner (the IMU attitude filter; keys, touch, hysteresis, leak): the reference behavior |
| `badge-ref/` | Runs a script of the badge app on exact2's runner: the reference for routes |
| `sound-ref/` | Runs a script of an app with sounds on exact2's runner: the reference for the voices |
| `comparison/` | The references the Mac checks use: `planb.json` and the PNGs (a recorded Stopwatch run of the level app), `badge-runner.json`, `badge-pages/`, `metronome-runner.json` |
| `tools/` | `agent.py` (drives an app over a serial port or the Mac's pseudo-terminal), `compare.py`, the checks, `badge_profile.py`, `make_clicks.py` |
| `VERSIONS.md` | Pinned tools and crates |

## Generate an app's C++

```sh
cargo run -p plan2cpp -- emit apps/level/app.contract apps/level/cpp/app_gen.cpp
cargo run -p plan2cpp -- ext extensions/attitude.json apps/level/cpp/ext_attitude.h
cargo run -p plan2cpp -- needs apps/badge/app.contract   # the app's needs, as JSON
cargo run -p plan2cpp -- dump apps/level/app.contract    # the plan's tables
```

Run plan2cpp from the repo root: the generated files name their source by its path from here,
and the tests compare the committed files with what plan2cpp makes now.

## Boards

A board implements `rt::Board` (`rt/src/contract_rt.h`) and gives only the parts it has. A clock
is required. These are optional: a `Display` (the core draws rounded rectangles and circles if
the board does not), a `Link` for agent lines, an accelerometer, keys and touch, a speaker,
counters, and commands such as `haptic`. Without a display the app runs headless. At boot the
host writes a `CE_WARN missing=…` line for each part the app needs and the board lacks. A
display may draw the screen in bands (`band_rows()`).

## On the Mac

```sh
make -C mac                        # build/{level,badge,metronome}-{mac,headless}
make -C mac test                   # sanitizer builds + mac_check.py, badge_check.py, sound_check.py
make -C mac container              # build/ContractMac and build/apps/*.bundle
mac/build/ContractMac metronome    # a window: arrows, Return, Escape, the mouse as touch
make -C rt-test test               # route corpus, extension binding, data-* props
mac/build/level-mac --band 32      # prints "PTY /dev/ttysNNN"; drive it with tools/agent.py
mac/build/badge-headless --watch   # no display; prints each new state as "STATE {…}"
```

The Mac boards read the FreeSans faces from the installed M5GFX library (`M5GFX=<path>` to
override), so text is measured and drawn with the Stopwatch's glyphs.

`mac_check.py` requires the Mac's level app state after each step to equal the recorded board
run, and its screenshots to equal the board's pixel for pixel, with 466-, 32-, 7- and 1-row
bands. `badge_check.py` requires exact2's runner's state and nodes after every step, and each of
15 page states to equal its reference in `comparison/badge-pages/`. `sound_check.py` requires
the metronome's voices to equal the runner's.

## References on exact2's runner

```sh
cargo test                                      # plan2cpp, level-data, and the apps on the runner
cargo clippy --workspace --all-targets -- -D warnings
cargo run -p badge-ref -- apps/badge/app.contract apps/badge/steps.json apps/badge/fixture/data.json > comparison/badge-runner.json
cargo run -p sound-ref -- apps/metronome/app.contract apps/metronome/steps.json metronome > comparison/metronome-runner.json
```

`contract verify apps/level/app.contract` checks the runner against the Lean semantics. Build
the CLI with `cargo build --release -p contract --bin contract --manifest-path exact2/Cargo.toml`;
it needs Lean (`elan default leanprover/lean4:v4.34.1`).

## Host extensions (prototype)

An app's sources other than exact2's own (`exactTime`) come from host extensions, a prototype
outside exact2 (exact2 LLP 1124, a draft). An extension file declares its sources, agent ops
and `data-*` props. `emit` refuses a source no listed extension declares, a field with another
type, and a send with another argument count, and records each extension's name, version and
SHA-256. `ext` writes a C++ base class with a typed method for each source, op and prop. At
boot the host writes `CE_WARN missing=extension:<name>` for each listed extension the board
does not give.
