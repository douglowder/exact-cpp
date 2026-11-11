// The core's drawing on a board's surface: clipped to a rectangle and blended
// at a coverage. Opaque drawing inside the board's own area goes through the
// board's shapes (fast on the Stopwatch); the rest blends into the surface
// rows the board gives (`Display::row`), pixel by pixel, in RGB565.
#pragma once

#include "exact_rt.h"

namespace rt {

class Canvas {
 public:
  // `clip` lies inside the area the board began; `whole` says it is that area.
  Canvas(Display& d, Rect clip, bool whole) : d_(d), clip_(clip), whole_(whole) {}
  Rect clip() const { return clip_; }

  // `alpha` is 0–255 coverage of `rgb` (0xRRGGBB) over what is drawn.
  void fill_rect(Rect r, uint32_t rgb, uint8_t alpha);
  void fill_round_rect(Rect r, int radius, uint32_t rgb, uint8_t alpha);
  void draw_round_rect(Rect r, int radius, uint32_t rgb, uint8_t alpha);
  void fill_circle(int cx, int cy, int radius, uint32_t rgb, uint8_t alpha);
  void draw_circle(int cx, int cy, int radius, uint32_t rgb, uint8_t alpha);
  // A `w` x `h` coverage mask of 4-bit nibbles (high first) at (x, y).
  void mask4(int x, int y, int w, int h, const uint8_t* bits, uint32_t rgb, uint8_t alpha);
  // `img` scaled (nearest pixel) into `dst`, with its corners rounded to
  // `radius`, and only where `dst` meets `box`.
  void image(Rect dst, const Image& img, Rect box, int radius, uint8_t alpha);

 private:
  void span(int x, int y, int w, int h, uint32_t rgb, uint8_t alpha);
  void put(int x, uint16_t* row, uint32_t rgb, unsigned alpha);
  void put565(int x, uint16_t* row, uint16_t c, unsigned alpha);
  bool fast(uint8_t alpha) const { return whole_ && alpha == 255; }

  Display& d_;
  Rect clip_;
  bool whole_;
};

}  // namespace rt
