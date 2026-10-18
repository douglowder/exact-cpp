# Evidence

This repo starts from contract-embedded commit c96c6b0 (`contract-esp32/`). That repo's
`EVIDENCE.md` and `RFC.md` record what was shown before the split: the level, badge and
metronome apps on the Stopwatch and the Mac boards, and path A.

## Checks after the split (2026-10-18, exact2 91a43c569)

| Check | Result |
| --- | --- |
| `cargo test` | 63 passed, 1 ignored; the same counts as contract-embedded c96c6b0 |
| `cargo clippy --workspace --all-targets -- -D warnings` | no warnings |
| `make -C rt-test test` | 299 route answers match; `ext_check` and `data_check` pass |
| `make -C mac test` | level: 18 states on both Mac boards, screenshots equal; badge: 35 runner states on 2 boards; metronome: 11 voices, 10 states |
| `make -C mac all container` | builds with no warnings |
| Generated C++ | `apps/*/cpp/app_gen.cpp` byte for byte equal to c96c6b0's; the `rt-test/marks` files differ only in the source path in line 1 |
