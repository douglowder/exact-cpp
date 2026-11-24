# Streaming an Exact app to a board

A board can run an Exact app without the app being compiled into its firmware.
`exact-stream` runs the app here, on the unchanged `exact_rt` core, and talks to
the board over a serial line in one of two modes:

1. **Thin client (stream the pixels).** The app runs on the Mac against a
   software RGB565 surface the size the app was written for. Each frame's damage
   is resampled to the board's screen, diffed against what the board already
   shows, and sent as an AREA packet of SKIP / FILL / LIT ops. The board's touch
   and buttons come back as `rt::Input`s. The board needs only
   `stream-proto/` — no runtime, no app.
2. **Plan mode (stream the plan).** The Mac sends the app itself. `plan2cpp`,
   with `PLAN2CPP_VM` set, writes every code body as `rt_vm` bytecode beside its
   C++. The host serializes the app's `rt::App` tables plus that bytecode into a
   plan blob; the board reads it into memory, points each `CodeFn` at a
   trampoline into the interpreter, and runs it on `exact_rt` with its own input.
   A new app needs no flash.

The board half lives in that board's own repo. This directory and
`rt-vm/`, `stream-proto/` know no board.

## Layout

| Path | What it is |
| --- | --- |
| `stream_main.cpp`, `stream_board.{h,cpp}` | `exact-stream`: the serial line, the damage resampler and differ, the stdin commands |
| `vm_check.cpp` | Runs an app compiled and interpreted side by side and compares every frame |
| `vm_bench.cpp` | Times the same two and reports a plan's memory footprint |
| `vm_entry.cpp` | A bundle's second entry point: its tables and bytecode, for writing a blob |
| `knob_app.cpp` | The `knob` app's (empty) Mac glue |
| `tools/gen_vm.sh` | `plan2cpp` with `PLAN2CPP_VM` set, into `build/vmgen` |
| `tools/fake_watcher.py`, `tools/stream_test.py` | The host against a fake board, with no hardware |

## Build

```sh
tools/gen_vm.sh     # plan2cpp with PLAN2CPP_VM -> build/vmgen
make                # exact-stream, vm_check, vm_bench, build/host/apps/*.bundle
```

## Check it

With no hardware at all:

```sh
build/host/vm_check build/host/apps/badge.bundle
python3 tools/stream_test.py build/host/exact-stream /tmp/streamtest motion
```

`vm_check` runs an app compiled and interpreted on the same inputs and the same
manual clock and requires every frame to be the same, pixel for pixel. It exits
non-zero if any frame or the final agent `state` differs. `stream_test.py` runs
the host against `fake_watcher.py` over a pseudo-terminal and saves the fake
board's screen.

`vm_bench APP.bundle [REPEATS] [dense]` times both and reports the plan's blob
size and the memory `plan_blob::Loaded` allocates. `dense` drives an input on
every turn rather than every fourth. Both runtimes paint through the same core,
so a turn's time includes paint: the ratio is what an app's responsiveness sees,
not the interpreter's cost alone.

## Run against a board

```sh
build/host/exact-stream --port /dev/cu.usbmodemNNNN knob          # pixels
build/host/exact-stream --plan --port /dev/cu.usbmodemNNNN knob   # the plan
```

Stdin takes `next`, `app NAME`, `key ArrowRight`, `stats`, `plan`, `stream` and
`agent {json}`. In plan mode the host's agent pseudo-terminal is forwarded to
the board's own `rt::Host`, so `tools/agent.py PTY state | key | screenshot`
drives the runtime on the board.
