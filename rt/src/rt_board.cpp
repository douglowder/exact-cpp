// The defaults of the board interface: what a board leaves out. The shapes
// are rt_shapes.h's, made of `fill_rect` spans, so a board that draws only
// rectangles matches the Stopwatch's pixels.
#include "contract_rt.h"
#include "rt_shapes.h"

namespace rt {

namespace {

// A span of `rgb` through the board's own `fill_rect`.
struct Fill {
  Display* d;
  uint32_t rgb;
  void operator()(int x, int y, int w, int h) const {
    if (w > 0 && h > 0) d->fill_rect(Rect{x, y, w, h}, rgb);
  }
};

}  // namespace

void Display::fill_round_rect(Rect r, int radius, uint32_t rgb) { shapes::fill_round_rect(r, radius, Fill{this, rgb}); }

void Display::draw_round_rect(Rect r, int radius, uint32_t rgb) { shapes::draw_round_rect(r, radius, Fill{this, rgb}); }

void Display::fill_circle(int x, int y, int r, uint32_t rgb) { shapes::fill_circle(x, y, r, Fill{this, rgb}); }

void Display::draw_circle(int x, int y, int r, uint32_t rgb) { shapes::draw_circle(x, y, r, Fill{this, rgb}); }

const uint8_t* Display::frame(uint32_t* bytes) const {
  if (bytes) *bytes = 0;
  return nullptr;
}

uint16_t* Display::row(int) { return nullptr; }

bool Board::input(Input&) { return false; }

bool Board::wall_clock(int64_t&, int&) const { return false; }

bool Board::can(const std::string&) const { return false; }

void Board::command(const std::string&, const std::vector<V>&) {}

void Board::step(uint32_t) {}

}  // namespace rt
