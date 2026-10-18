// The Stopwatch board's faces, as Adafruit GFX font data: FreeSans 9/12/18/24
// pt, regular and bold. The data comes from M5GFX's own font headers (the
// build adds M5GFX's src/ to the include path), so the Mac measures and draws
// text with the same glyphs as the board.
#pragma once

#include <stdint.h>

namespace rt_mac::gfx {

struct Glyph {
  uint32_t bitmapOffset;
  uint8_t width, height;
  uint8_t xAdvance;
  int8_t xOffset, yOffset;
};

struct Font {
  uint8_t* bitmap;
  Glyph* glyph;
  uint16_t first, last;
  uint8_t yAdvance;
};

struct Face {
  const Font* regular;
  const Font* bold;
  float px;  // the CSS size it stands for (FreeSans N pt ~ 2N px)
};

extern const Face kFaces[4];

}  // namespace rt_mac::gfx
