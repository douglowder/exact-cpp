#!/bin/sh
# Builds the Watcher firmware with arduino-cli (esp32 core 3.3.10): the thin
# client and plan mode (exact-cpp's exact_rt core, the rt_vm interpreter and
# the plan-blob reader). The image is an app for the Watcher's existing
# bootloader (QIO flash, octal PSRAM, 32 MB); install it with tools/flash.py.
#   EXACT_CPP=… M5GFX=… tools/build_firmware.sh
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
ac=${ARDUINO_CLI:-arduino-cli}
cpp=${EXACT_CPP:-$(cd "$here/../.." && pwd)}
m5=${M5GFX:-$("$ac" config get directories.user)/libraries/M5GFX}
sk="$here/firmware/watcher_stream"
# The sketch's src/: copies of the shared sources (not committed).
rm -rf "$sk/src"
mkdir -p "$sk/src/vm" "$sk/src/fonts/lgfx/Fonts/GFXFF"
cp "$here"/vm/*.h "$here"/vm/*.cpp "$sk/src/vm/"
cp "$cpp/rt-mac/gfx_fonts.h" "$cpp/rt-mac/gfx_fonts.cpp" "$sk/src/fonts/"
for f in FreeSans9pt7b FreeSans12pt7b FreeSans18pt7b FreeSans24pt7b \
         FreeSansBold9pt7b FreeSansBold12pt7b FreeSansBold18pt7b FreeSansBold24pt7b; do
  cp "$m5/src/lgfx/Fonts/GFXFF/$f.h" "$sk/src/fonts/lgfx/Fonts/GFXFF/"
done
inc="-I$here/proto"
"$ac" compile \
  --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashMode=qio,FlashSize=32M,PartitionScheme=fatflash,CDCOnBoot=default,CPUFreq=240 \
  --library "$cpp/rt" \
  --build-property "compiler.cpp.extra_flags=$inc" \
  --build-property "compiler.c.extra_flags=$inc" \
  --build-path "$here/build/firmware" \
  --output-dir "$here/build/firmware/out" \
  "$sk" "$@"
ls -l "$here/build/firmware/out/"
