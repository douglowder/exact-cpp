# Badge app assets

The fonts and marks of the init() badge firmware (`stopwatch/`, the
`douglowder/stopwatch` submodule), for `apps/badge/app.contract`. plan2cpp
rasterizes the fonts and converts the images when it compiles the app; the
board stores only the sizes the app uses.

## Fonts (`fonts/`)

The badge firmware has only converted LVGL bitmap subsets of these fonts
(`firmware/factory_badge/main/ui/font_*.c`). Exact declares a font from a TTF
or OTF file, so these are the upstream files those subsets were made from,
downloaded from the pinned sources in the firmware's
`firmware/factory_badge/main/ui/fonts/README.md`, with the same SHA-256 checksums.

| File | Upstream | SHA-256 |
| --- | --- | --- |
| `Inter-Medium.ttf` | [Inter 4.1](https://github.com/rsms/inter/releases/tag/v4.1), `Inter-4.1.zip` → `extras/ttf/Inter-Medium.ttf` | `97ad806f526e41546d46365bb3a393145f75b7b1568913db74549ad8b8dba872` |
| `IBMPlexMono-Regular.ttf` | [IBM Plex](https://github.com/IBM/plex) `78cd4223d8de9fcb78cba84eadecb269c56093c5`, `packages/plex-mono/fonts/complete/ttf/` | `7c6fbddca4b700be918f5f6183d9bd4464fa427fe435f0b480d77fe2bb8c5a43` |
| `IBMPlexMono-Medium.ttf` | same | `98fbd727aae340b236955879dabed4d991aac9e8e90b3b2a67ce4a59221cc97c` |
| `IBMPlexMono-SemiBold.ttf` | same | `f04d7c488ddf7d1fa99f2574efc3406ea4cbe17bb1af3a1ab960f84d0c96a172` |

Both families are under the SIL Open Font License 1.1. `Inter-OFL.txt` and
`IBMPlexMono-OFL.txt` are the license texts, copied from the firmware's
`ui/fonts/`.

## Images (`images/`)

Copied unchanged from the firmware's `firmware/factory_badge/main/ui/assets/`:
`logo_small.png`, `logo_large.png`, `left.png`, `right.png`,
`empty_portrait.png` (white marks with alpha), and
`developers-after-dark.png` (the 2400 x 2400 invitation artwork; plan2cpp
resamples it to the 300 x 300 the app draws). The firmware's
`ui/assets/README.md` records their origin. The `init()` wordmark and the
event artwork are brand assets of their owners (the firmware's
`THIRD_PARTY_NOTICES.md`); they are here only as the badge firmware uses them.
