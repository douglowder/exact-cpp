# POC: streaming Exact to a SenseCAP Watcher

A proof of concept for running Exact apps on a board that has no app code
compiled in, built on exact-cpp's unchanged `exact_rt` core. It was tried on a
[SenseCAP Watcher](https://www.seeedstudio.com/watcher) (ESP32-S3, 412 × 412
round SPD2010 QSPI panel, SPD2010 touch, a knob, 8 MB PSRAM) connected to a Mac
over its USB serial line (CH342, 3 Mbaud).

One firmware does two things:

1. **Thin client (stream the pixels).** The app runs on the Mac on a
   `rt::Board` whose `Display` is a software RGB565 surface. Each frame's
   damage is resampled to the Watcher's screen, diffed against what the
   Watcher already shows, and sent as an AREA packet of SKIP / FILL / LIT ops.
   Touch and knob come back as `rt::Input`s.
2. **Plan mode (stream the plan).** The Mac sends the app itself. `plan2cpp`,
   with `PLAN2CPP_VM` set, also writes every code body as `rt_vm` bytecode (the
   plan's own instructions with jump targets, record sizes and stdlib calls
   resolved). The host serializes the app's `rt::App` tables plus that bytecode
   into a plan blob; the Watcher reads it into PSRAM, points each `CodeFn` at
   one of 1024 trampolines into a small interpreter, and runs it on `exact_rt`
   with its own touch and knob. A new app needs no flash.

```
host/   exact-stream (the Mac host), vm_check, app bundles   (Mac, clang)
vm/     rt_vm (the interpreter), plan_blob (write and read)  (Mac and board)
proto/  the serial protocol, both directions
firmware/watcher_stream/  the Watcher firmware (Arduino, esp32 core 3.3.10;
        vendored esp_lcd_spd2010, Apache-2.0)
apps/knob/  an app written for the Watcher's 412 × 412 screen and knob
tools/  gen_vm.sh, build_firmware.sh, flash.py, fake_watcher.py, stream_test.py
```

The only change outside this folder is `plan2cpp/src/{code,emit}.rs`: with
`PLAN2CPP_VM` unset its output is byte-for-byte what it was (the committed
`app_gen.cpp` files and `cargo test` are unchanged).

## Run

```
poc/watcher-stream/tools/gen_vm.sh          # plan2cpp with PLAN2CPP_VM -> build/vmgen
make -C poc/watcher-stream/host             # exact-stream, vm_check, apps/*.bundle
poc/watcher-stream/build/host/vm_check poc/watcher-stream/build/host/apps/knob.bundle
poc/watcher-stream/tools/build_firmware.sh
python3 poc/watcher-stream/tools/flash.py /dev/cu.usbmodemXXXX3 \
    poc/watcher-stream/build/firmware/out/watcher_stream.ino.bin
poc/watcher-stream/build/host/exact-stream --port /dev/cu.usbmodemXXXX3 knob          # pixels
poc/watcher-stream/build/host/exact-stream --plan --port /dev/cu.usbmodemXXXX3 knob   # plan
```

Stdin: `next`, `app NAME`, `key ArrowRight`, `stats`, `plan`, `stream`,
`agent {json}`. The knob: turn = ArrowLeft/ArrowRight, click = Enter, hold
0.6 s = Escape, hold 2 s = the next app. In plan mode the host's agent PTY is
forwarded to the board's own `rt::Host`, so `tools/agent.py PTY state | key |
screenshot` drives the runtime on the Watcher. Without hardware,
`tools/stream_test.py` runs the host against `tools/fake_watcher.py`.

`flash.py` writes only the spare OTA slot and one otadata sector, keeping the
installed firmware (`flash.py PORT --back` boots it again). The Watcher it was
tried on has its bootloader's partition table at 0x10000; the Arduino
libraries read 0x8000, so `flash.py` gives the app its own table there with its
`nvs` in the unused tail of its slot (Arduino erases an `nvs` it cannot read).
It refuses a board without an OTA table at 0x10000.

## Checks

* `vm_check APP.bundle` runs an app compiled and interpreted, side by side on
  the same inputs and manual clock, and compares every frame: knob, metronome,
  motion and level are identical on all 128 frames. Badge differs only where
  its extension publishes the profile photo once, to the first runtime.
* Thin client: 0 CRC failures at 3 Mbaud over long runs of `motion`.

## Measured on the Watcher

| Thin client | |
|---|---|
| Firmware | 393 KB (thin client only) |
| Full-screen keyframe | 5–25 KB encoded; 44–120 ms incl. a 21 ms panel push |
| Key → panel, knob app (412 × 172 area) | median 14.0 ms |
| Stopwatch apps (468 × 466) | resampled to 412 × 412 on the Mac |

| Plan mode | |
|---|---|
| Firmware | 845 KB, any app (thin client + exact_rt + interpreter + fonts) |
| Blob | knob 8.5 KB, metronome 13.6 KB, level 21 KB, motion 22 KB, badge 710 KB |
| Sent → running | knob 94 ms, motion 154 ms, badge 2.85 s (the serial line) |
| Load / boot on the board | 1.6–51 ms / 58–70 ms |
| Agent key → frame pushed (knob) | 17.8 ms (paint 6.1 ms, push 6.2 ms) |

## Not done

* Plan mode lays out at the board's size: Stopwatch apps (468 × 466) are
  cropped, not scaled. Host extensions (`attitude`, `init-badge`) are not on the
  Watcher, so the level app has no tilt and the badge has no profile.
* Sound: the Mac plays an app's sequences in thin-client mode; the Watcher's
  speaker is unused.
* The serial line limits big plans (badge: 710 KB, mostly fonts and images).
  Wi-Fi would be a transport change behind the same packets.
