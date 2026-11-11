# exact-cpp

Exact apps compiled to C++. Exact's compiler makes the plan and `plan2cpp` writes the app
as C++ tables and functions. The `rt` core (`exact_rt`, standard C++17) runs the app through
a board interface, `rt::Board`. No Rust runs on a board.

This repo has the parts that know no board: `plan2cpp`, the core, the Mac boards, the Mac
container, the apps and their references on Exact's runner. Each hardware target is its own
repo, with its board and sketches, and has this repo as a submodule. The first one is
[exact-esp32-stopwatch](https://github.com/douglowder/exact-esp32-stopwatch) (the M5Stack
Stopwatch, an ESP32-S3).

Comments and docs cite Exact's rules by LLP number, such as LLP 1121. LLP numbers refer to
Exact's design records, which are not public.

## Requirements

Tested on macOS 26 on Apple silicon.

| Item | Version | Used for |
| --- | --- | --- |
| Xcode Command Line Tools (`xcode-select --install`) | Apple clang | The Mac boards and programs (`mac/`, `rt-test/`) |
| [rustup](https://rustup.rs) | Rust 1.97.0, from `rust-toolchain.toml` | `plan2cpp` and the references on Exact's runner |
| [arduino-cli](https://arduino.github.io/arduino-cli/) (`brew install arduino-cli`) and M5GFX | M5GFX 0.2.26 | The FreeSans faces the Mac boards draw with |
| Python 3 | standard library only | The checks and the agent tools in `tools/` |

Install M5GFX once:

```sh
arduino-cli lib install M5GFX@0.2.26
```

To build for the Stopwatch, use exact-esp32-stopwatch, which has this repo as a submodule.

## Get the code

```sh
git clone --recursive https://github.com/douglowder/exact-cpp.git
cd exact-cpp
```

In a clone made without `--recursive`, run `git submodule update --init --recursive`.

## Layout

| Path | What it is |
| --- | --- |
| `exact/` | Exact ([expo/exact](https://github.com/expo/exact)), as a submodule (the compiler, plan, runner and kernel) |
| `apps/<app>/app.contract` | The app's Exact source (`level`, `badge`, `metronome`, `motion`) |
| `apps/<app>/cpp/` | The app's C++: the generated `app_gen.cpp` and extension headers, and its glue |
| `apps/{level,badge}/extensions.json` | The host extensions the app uses, by name and version |
| `apps/<app>/app.tamber` | The app's colours as design tokens (Exact LLP 1121), `scheme=fixed` |
| `apps/level/app.test.contract` | Authored tests (`contract verify`) |
| `apps/badge/` | The init() badge's pages and modals as routes; `assets/` (fonts, SIL OFL 1.1, and marks; `README.md` has their sources); `fixture/`, a made-up badge the Mac boards and `badge-ref` answer from; `steps.json`, its parity script |
| `apps/metronome/` | A minimal metronome from Exact's `apps/metronome`: a tempo, Start and Stop, an accented beat 1; one `sequence` plays two clicks (`assets/`, made by `tools/make_clicks.py`) |
| `apps/motion/` | A demo of CSS animations in three scenes: easing (four timing functions), color (a disc and a ring), and pulse (opacity and staggered delays); A and B change scene, both pause |
| `extensions/` | Host extension files (prototype): `attitude.json` (level), `init-badge.json` (badge) |
| `plan2cpp/` | Compiles a plan to C++ with the app's fonts, PNG images, design tokens and sounds; refuses the rest by name; checks the app's sources against its extensions and writes each extension's C++ base class |
| `rt/` | The core: values, layout, paint, timers, the transport that plays a `sequence`, the agent; the `Board` interface and its defaults. An Arduino library (`library.properties`) |
| `rt-mac/` | The Mac boards: `Board` (a software RGB565 surface in bands, the M5GFX FreeSans data, a pseudo-terminal link) and `HeadlessBoard` (link and clock only); a manual clock; a CoreAudio speaker |
| `mac/` | The Mac programs for each app, `-mac` and `-headless`; and `ExactMac` (`container.mm`), one window that runs any app's bundle from its App menu |
| `rt-test/` | `route_check` (the C++ route port against Exact's route corpus), `ext_check` (extension binding), `data_check` (`data-*` props, on the test app in `marks/`), `lookup_check` (resources with arguments and placeholders, on the test app in `lookup/`) and `anim_check` (CSS animations, on the test app in `anim/`, against Exact's motion crate) |
| `level-data/`, `level-tests/` | The level app on Exact's runner (the IMU attitude filter; keys, touch, hysteresis, leak): the reference behavior |
| `badge-ref/` | Runs a script of the badge app on Exact's runner: the reference for routes |
| `sound-ref/` | Runs a script of an app with sounds on Exact's runner: the reference for the voices |
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

A board implements `rt::Board` (`rt/src/exact_rt.h`) and gives only the parts it has. A clock
is required. These are optional: a `Display` (the core draws rounded rectangles and circles if
the board does not), a `Link` for agent lines, an accelerometer, keys and touch, a speaker,
counters, and commands such as `haptic`. Without a display the app runs headless. At boot the
host writes a `CE_WARN missing=…` line for each part the app needs and the board lacks. A
display may draw the screen in bands (`band_rows()`).

## On the Mac

```sh
make -C mac                        # build/{level,badge,metronome,motion}-{mac,headless}
make -C mac test                   # sanitizer builds + mac_check.py, badge_check.py, sound_check.py
make -C mac container              # build/ExactMac and build/apps/*.bundle
mac/build/ExactMac metronome    # a window: arrows, Return, Escape, the mouse as touch
make -C rt-test test               # route corpus, extensions, data-* props, resources, animations
mac/build/level-mac --band 32      # prints "PTY /dev/ttysNNN"; drive it with tools/agent.py
mac/build/badge-headless --watch   # no display; prints each new state as "STATE {…}"
```

The Mac boards read the FreeSans faces from the installed M5GFX library (`M5GFX=<path>` to
override), so text is measured and drawn with the Stopwatch's glyphs.

Where an app sets no color, the screen is black and text is white: the dark scheme, the default
on every board. ExactMac's Appearance menu switches between Dark (⌘D) and Light (⌘L, black text
on white) and keeps the choice across apps; the agent's `{"op":"scheme","scheme":"light"}` does
the same on any board, and `{"op":"scheme"}` reports it.

`mac_check.py` requires the Mac's level app state after each step to equal the recorded board
run, and its screenshots to equal the board's pixel for pixel, with 466-, 32-, 7- and 1-row
bands. `badge_check.py` requires Exact's runner's state and nodes after every step, and each of
15 page states to equal its reference in `comparison/badge-pages/`. `sound_check.py` requires
the metronome's voices to equal the runner's.

## References on Exact's runner

```sh
cargo test                                      # plan2cpp, level-data, and the apps on the runner
cargo clippy --workspace --all-targets -- -D warnings
cargo run -p badge-ref -- apps/badge/app.contract apps/badge/steps.json apps/badge/fixture/data.json > comparison/badge-runner.json
cargo run -p sound-ref -- apps/metronome/app.contract apps/metronome/steps.json metronome > comparison/metronome-runner.json
```

`contract verify apps/level/app.contract` checks the runner against the Lean semantics. Build
the CLI with `cargo build --release -p contract --bin contract --manifest-path exact/Cargo.toml`;
it needs Lean (`elan default leanprover/lean4:v4.34.1`).

## Animations

CSS animations (`@keyframes` and `animation`) animate opacity, `color`, `background-color`, the
border colors and `translate`; a translate moves the node and its subtree as drawn, and where
it takes presses. plan2cpp parses each string literal of the plan that reads as an `animation`
value, with Exact's motion crate, into tables, so the board parses no CSS. A node's `animation`
value is looked up by its text: a value made at run time from other text is logged once and
plays nothing. plan2cpp refuses, by name, an animation of any other property. The board samples
each animation in closed form, as Exact's motion crate does: `anim_check` compares the two at
fixed times (`rt-test/anim/reference.h`, which `PLAN2CPP_BLESS=1 cargo test -p plan2cpp --test
anim` writes).

## Host extensions (prototype)

An app's sources other than Exact's own (`exactTime`) come from host extensions, a prototype
outside Exact (Exact LLP 1124, a draft). An extension file declares its sources, agent ops
and `data-*` props. `emit` refuses a source no listed extension declares, a field with another
type, and a send with another argument count, and records each extension's name, version and
SHA-256. `ext` writes a C++ base class with a typed method for each source, op and prop. A
resource asks its source with its arguments, and asks again after a commit that changes their
values. While its source gives no answer it shows its placeholder (`else source(args)` or
`else empty(…)`) or else its type's zero, as Exact's runner does. A resource asked for each
item of a list (`for`) is refused. At boot the host writes `CE_WARN missing=extension:<name>` for each listed extension
the board does not give.

## Not supported yet

exact-cpp runs a subset of what Exact's runner runs. plan2cpp refuses, by name, the first
construct of an app outside it, so `plan2cpp emit` on an app says what stops it first. A few
things compile and behave differently from Exact's runner; they are listed below too.

### Refused by plan2cpp

- Code: optional unwrapping and indexing, `map` and `filter` on lists, refreshing a resource,
  and the pending and failed states of resources and mutations (the opcodes `LoadBound`,
  `JumpIfNone`, `Unwrap`, `LoadIndex`, `Map`, `Filter`, `Refresh`, `PendingResource`,
  `PendingMutation`, `FailedResource`, `FailureResource`, `FailedMutation` and
  `FailureMutation`).
- Standard functions other than `toFixed`, `max`, `min`, `floor`, `ceil`, `round`, `toString`,
  `performanceNow` and the route functions (`open`, `push`, `replace`, `back`, `select`, `go`,
  `stack`, `top`, `depth`, `params`, `searchParam`, `encodeURIComponent`, `encodeRouteSegment`,
  `fragment`). Among those refused: `length`, `isEmpty`, `includes`, `join`, `split`, `slice`,
  `trim`, `formatTime`, `formatDate`, `formatNumber`, `parseNumber`, `sin`, `cos`, `sum`,
  `sortBy`, `range` and `t`.
- Regions: `match`, and a `for` region with more than one arm. A late child slot.
- Tasks that are gated (`when`), keyed or run per frame; plain intervals and `once` run.
- Mutations that refresh resources, run `then`, or queue. Resources asked once for each item of
  a list (`for … in`). A routes slot other than slot 0.
- Node types other than `View`, `Text`, `Pressable` and `Image`: `ScrollView`, `List`,
  `TextInput`, `Control`, `NativeView`, `Canvas`, `WebView`, `Video`, `Head`, and every SVG
  node.
- Events other than `press`, `key`, `navigate`, `pan` and `panrelease`: among them `change`,
  `input`, `submit`, `focus`, `blur`, `scroll`, `hover`, `pointerdown`, `pointerup`,
  `pointermove`, the media events and the drag events.
- Style rows other than `width`, `height`, `padding` and `margin` (each side), `display`,
  `flex-direction`, `justify-content`, `align-items`, `row-gap`, `column-gap`, `position`, `top`,
  `right`, `bottom`, `left`, `box-sizing`, `border-width`, `border-style`, `border-color` and
  `border-radius` (each side or corner), `background-color`, `color`, `font-size`,
  `font-weight`, `font-family`, `line-height`, `text-align`, `white-space`, `text-overflow`,
  `overflow`, `object-fit`, `opacity`, `appearance` and `animation`. Among those refused:
  `flex-grow`, `flex-shrink`, `flex-wrap`, `min-width`, `max-width`, the grid rows,
  `transform`, `scale`, `rotate`, `box-shadow`, `transition`, `z-index`, and the SVG rows.
- Props other than the text, image source, `testId`, `id`, `autofocus`, the navigation props
  (`navigationKey`, `navigationBack`, `navigationPresentation`), `closedby`, `data-*`, and the
  accessibility role and controls; the other accessibility props, such as `aria-label`, are kept
  and not used.
- Animations of a property other than opacity, `color`, `background-color`, the border colors
  and `translate` (so `scale` and `rotate` too), and keyframe colors in OKLab.
- Canvas surfaces, icon tokens (`symbol:`), localized texts (`t`), color profiles
  (`@color-profile`), and app axes (`app.tamber` `axes`, `setMode`).
- Design tokens that vary by color scheme, role overrides, and tokens that are not colors.
- Images and fonts outside the app's `assets/`.
- Sound: more than one `sequence`, a sequence with no `when` gate, a keyed one, one with
  `ended=`, `from=`, `countIn=`, `clock=` or `clockOut=`, and WAV files that are not 16-bit
  mono.

### Accepted, with different behavior

- Colors: the core reads `#rgb`, `#rrggbb`, `#rrggbbaa`, `transparent` and design tokens. A
  named color (`red`), `rgb()`, `hsl()` or `light-dark()` in a style row draws as no color.
- Exact's own sources other than `exactTime` (`exactViewport`, `exactTransport`, `exactMIDI`,
  `exactPage`, `exactSurface`, `exactDelivery`, `exactActivities` and the activity sends) have
  no answer on a board, so a resource that reads one shows its type's zero.
- An `animation` value made at run time from other text is logged once and plays nothing;
  plan2cpp reads only the plan's string literals.
- A board has one color scheme at a time (white text on black by default); `light-dark()` does
  not follow it.
- Hatches are left out: the node shows as it would on a platform without one.

### Needs a host extension

Every source that is not Exact's own, including those of Exact's `@exact/*` packages (camera,
geolocation, contacts and the rest) and an app's own data sources, needs a host extension
written in C++ for each board (see Host extensions). An extension's results are records of
numbers, strings and bools; a source that answers a list or a nested record cannot be declared
yet.

### Outside exact-cpp

exact-cpp compiles an app's `app.contract`. Exact's terminal apps (`terminal.contract`), its
game engine (`game/`), and the widget, Live Activity and watch entries of its apps run on
Exact's own hosts.

## License

MIT, the same as [Exact](https://github.com/expo/exact). See [LICENSE](LICENSE). The badge app's fonts are under the SIL Open Font License 1.1 (`apps/badge/assets/README.md`).
