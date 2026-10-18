#include "rt_canvas.h"

#include <algorithm>
#include <cmath>

#include "rt_shapes.h"

namespace rt {

namespace {

uint16_t to565(uint32_t rgb) {
  return static_cast<uint16_t>(((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f));
}

uint16_t swap(uint16_t v) { return static_cast<uint16_t>((v << 8) | (v >> 8)); }

// `src` over `dst` at `alpha` (0–255): each channel widened to 8 bits,
// mixed, and truncated back to RGB565, as LVGL blends, so a gray stays gray.
uint16_t mix(uint16_t dst, uint16_t src, unsigned alpha) {
  const unsigned keep = 255 - alpha;
  auto r8 = [](uint16_t c) { unsigned v = (c >> 11) & 31; return (v << 3) | (v >> 2); };
  auto g8 = [](uint16_t c) { unsigned v = (c >> 5) & 63; return (v << 2) | (v >> 4); };
  auto b8 = [](uint16_t c) { unsigned v = c & 31; return (v << 3) | (v >> 2); };
  unsigned r = (r8(dst) * keep + r8(src) * alpha + 127) / 255;
  unsigned g = (g8(dst) * keep + g8(src) * alpha + 127) / 255;
  unsigned b = (b8(dst) * keep + b8(src) * alpha + 127) / 255;
  return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

Rect meet(Rect a, Rect b) {
  int x = std::max(a.x, b.x), y = std::max(a.y, b.y);
  int right = std::min(a.x + a.w, b.x + b.w), bottom = std::min(a.y + a.h, b.y + b.h);
  return {x, y, std::max(0, right - x), std::max(0, bottom - y)};
}

// How far row `dy` of an `h`-row box with corner `radius` is cut in at each side.
int corner_inset(int dy, int h, int radius) {
  if (radius <= 0) return 0;
  int depth = dy < radius ? radius - dy : (dy >= h - radius ? dy - (h - radius) + 1 : 0);
  if (depth <= 0) return 0;
  const float y = static_cast<float>(depth) - 0.5f;
  const float r = static_cast<float>(radius);
  return radius - static_cast<int>(std::lround(std::sqrt(std::max(0.0f, r * r - y * y))));
}

}  // namespace

void Canvas::put565(int x, uint16_t* row, uint16_t c, unsigned alpha) {
  if (alpha == 0) return;
  const bool be = d_.big_endian();
  if (alpha >= 255) {
    row[x] = be ? swap(c) : c;
    return;
  }
  const uint16_t dst = be ? swap(row[x]) : row[x];
  const uint16_t out = mix(dst, c, alpha);
  row[x] = be ? swap(out) : out;
}

void Canvas::put(int x, uint16_t* row, uint32_t rgb, unsigned alpha) { put565(x, row, to565(rgb), alpha); }

void Canvas::span(int x, int y, int w, int h, uint32_t rgb, uint8_t alpha) {
  const Rect r = meet(Rect{x, y, w, h}, clip_);
  if (r.w <= 0 || r.h <= 0 || alpha == 0) return;
  if (alpha == 255) {
    d_.fill_rect(r, rgb);
    return;
  }
  const uint16_t c = to565(rgb);
  for (int yy = r.y; yy < r.y + r.h; ++yy) {
    uint16_t* row = d_.row(yy);
    if (!row) {
      if (alpha >= 128) d_.fill_rect(Rect{r.x, yy, r.w, 1}, rgb);
      continue;
    }
    for (int xx = r.x; xx < r.x + r.w; ++xx) put565(xx, row, c, alpha);
  }
}

void Canvas::fill_rect(Rect r, uint32_t rgb, uint8_t alpha) { span(r.x, r.y, r.w, r.h, rgb, alpha); }

void Canvas::fill_round_rect(Rect r, int radius, uint32_t rgb, uint8_t alpha) {
  if (fast(alpha)) return d_.fill_round_rect(r, radius, rgb);
  shapes::fill_round_rect(r, radius, [&](int x, int y, int w, int h) { span(x, y, w, h, rgb, alpha); });
}

void Canvas::draw_round_rect(Rect r, int radius, uint32_t rgb, uint8_t alpha) {
  if (fast(alpha)) return d_.draw_round_rect(r, radius, rgb);
  shapes::draw_round_rect(r, radius, [&](int x, int y, int w, int h) { span(x, y, w, h, rgb, alpha); });
}

void Canvas::fill_circle(int cx, int cy, int radius, uint32_t rgb, uint8_t alpha) {
  if (fast(alpha)) return d_.fill_circle(cx, cy, radius, rgb);
  shapes::fill_circle(cx, cy, radius, [&](int x, int y, int w, int h) { span(x, y, w, h, rgb, alpha); });
}

void Canvas::draw_circle(int cx, int cy, int radius, uint32_t rgb, uint8_t alpha) {
  if (fast(alpha)) return d_.draw_circle(cx, cy, radius, rgb);
  shapes::draw_circle(cx, cy, radius, [&](int x, int y, int w, int h) { span(x, y, w, h, rgb, alpha); });
}

void Canvas::mask4(int x, int y, int w, int h, const uint8_t* bits, uint32_t rgb, uint8_t alpha) {
  const Rect r = meet(Rect{x, y, w, h}, clip_);
  if (r.w <= 0 || r.h <= 0 || alpha == 0) return;
  for (int yy = r.y; yy < r.y + r.h; ++yy) {
    uint16_t* row = d_.row(yy);
    for (int xx = r.x; xx < r.x + r.w; ++xx) {
      const int k = (yy - y) * w + (xx - x);
      const unsigned nibble = (bits[k >> 1] >> ((k & 1) ? 0 : 4)) & 15;
      const unsigned a = nibble * 17 * alpha / 255;
      if (row) put(xx, row, rgb, a);
      else if (a >= 128) d_.fill_rect(Rect{xx, yy, 1, 1}, rgb);
    }
  }
}

void Canvas::image(Rect dst, const Image& img, Rect box, int radius, uint8_t alpha) {
  const Rect r = meet(meet(dst, box), clip_);
  if (r.w <= 0 || r.h <= 0 || dst.w <= 0 || dst.h <= 0 || alpha == 0) return;
  for (int yy = r.y; yy < r.y + r.h; ++yy) {
    const int inset = corner_inset(yy - box.y, box.h, radius);
    const int sy = static_cast<int>(static_cast<int64_t>(yy - dst.y) * img.h / dst.h);
    uint16_t* row = d_.row(yy);
    for (int xx = std::max(r.x, box.x + inset); xx < std::min(r.x + r.w, box.x + box.w - inset); ++xx) {
      const int sx = static_cast<int>(static_cast<int64_t>(xx - dst.x) * img.w / dst.w);
      const size_t k = static_cast<size_t>(sy) * img.w + sx;
      const unsigned a = (img.alpha ? img.alpha[k] : 255u) * alpha / 255;
      if (a == 0) continue;
      uint16_t c = 0;
      if (img.format == 2) {
        c = to565(img.color);
      } else {
        c = static_cast<uint16_t>(img.rgb[k * 2] | (img.rgb[k * 2 + 1] << 8));
      }
      if (row) {
        put565(xx, row, c, a);
      } else if (a >= 128) {
        const uint32_t rgb = img.format == 2 ? img.color
                             : ((c >> 11) & 31) << 19 | ((c >> 5) & 63) << 10 | (c & 31) << 3;
        d_.fill_rect(Rect{xx, yy, 1, 1}, rgb);
      }
    }
  }
}

}  // namespace rt
