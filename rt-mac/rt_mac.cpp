// The Mac boards. Text uses the same integer algorithms as M5GFX 0.2.26
// (LGFXBase.cpp and lgfx_fonts.cpp, MIT license, M5Stack and lovyan03), and
// the other shapes are the core's defaults, made of fill_rect, so a Mac
// screenshot can match the Stopwatch's pixel for pixel.
#include "exact_rt_mac.h"

#include <fcntl.h>
#include <malloc/malloc.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gfx_fonts.h"

namespace rt_mac {

namespace {

constexpr int kBulkTimeoutMs = 2000;

uint16_t rgb565(uint32_t rgb) {
  return static_cast<uint16_t>(((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f));
}

const gfx::Font& font(int face) {
  const gfx::Face& f = gfx::kFaces[face / 2];
  return face % 2 ? *f.bold : *f.regular;
}

const gfx::Glyph* glyph(const gfx::Font& f, uint32_t code) {
  if (code < f.first || code > f.last) return nullptr;
  return &f.glyph[code - f.first];
}

// The glyph M5GFX measures for a code point: its own, or the space's.
const gfx::Glyph& metric(const gfx::Font& f, uint32_t code) {
  const gfx::Glyph* g = glyph(f, code);
  return g ? *g : *glyph(f, 0x20);
}

// Code points of UTF-8 text, without control characters.
std::vector<uint32_t> code_points(const char* text) {
  std::vector<uint32_t> out;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p;) {
    uint32_t c = *p++;
    int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (more) c &= 0x3fu >> more;
    for (; more > 0 && (*p & 0xc0) == 0x80; --more) c = (c << 6) | (*p++ & 0x3f);
    if (c >= 0x20) out.push_back(c);
  }
  return out;
}

// GFXfont::getDefaultMetric: the largest extents above and below the
// baseline over the font's glyphs (M5GFX stops one short of `last`).
void default_metric(const gfx::Font& f, int& baseline, int& height) {
  int above = 0, below = 0;
  const size_t count = static_cast<size_t>(f.last - f.first);
  size_t c = 0;
  do {
    int ab = -f.glyph[c].yOffset;
    above = std::max(above, ab);
    below = std::max(below, f.glyph[c].height - ab);
  } while (++c < count);
  baseline = above;
  height = above + below;
}

}  // namespace

Display::Display(int width, int height, int band_rows)
    : width_(width), height_(height), band_rows_(std::clamp(band_rows, 1, height)) {}

bool Display::begin() {
  band_.assign(static_cast<size_t>(width_) * band_rows_, 0);
  panel_.assign(static_cast<size_t>(width_) * height_ * 2, 0);
  return true;
}

void Display::begin_area(rt::Rect area) {
  area_ = area;
  area_.h = std::min(area_.h, band_rows_);
}

void Display::push_area() {
  for (int y = area_.y; y < area_.y + area_.h; ++y) {
    const uint16_t* row = &band_[static_cast<size_t>(y - area_.y) * width_];
    uint8_t* out = &panel_[(static_cast<size_t>(y) * width_) * 2];
    for (int x = area_.x; x < area_.x + area_.w; ++x) {
      out[x * 2] = static_cast<uint8_t>(row[x] >> 8);
      out[x * 2 + 1] = static_cast<uint8_t>(row[x] & 0xff);
    }
  }
}

uint16_t* Display::row(int y) {
  if (y < area_.y || y >= area_.y + area_.h) return nullptr;
  return &band_[static_cast<size_t>(y - area_.y) * width_];
}

void Display::fill(int x, int y, int w, int h) {
  int x0 = std::max(x, area_.x), y0 = std::max(y, area_.y);
  int x1 = std::min(x + w, area_.x + area_.w), y1 = std::min(y + h, area_.y + area_.h);
  for (int yy = y0; yy < y1; ++yy) {
    uint16_t* row = &band_[static_cast<size_t>(yy - area_.y) * width_];
    std::fill(row + x0, row + std::max(x0, x1), color_);
  }
}

void Display::fill_rect(rt::Rect r, uint32_t rgb) {
  color_ = rgb565(rgb);
  if (r.w < 0) r.x += r.w, r.w = -r.w;
  if (r.h < 0) r.y += r.h, r.h = -r.h;
  fill(r.x, r.y, r.w, r.h);
}

int Display::face(float px, int weight) const {
  int best = 0;
  for (int i = 1; i < 4; ++i) {
    if (std::fabs(gfx::kFaces[i].px - px) < std::fabs(gfx::kFaces[best].px - px)) best = i;
  }
  return best * 2 + (weight >= 600 ? 1 : 0);
}

// LGFXBase::text_width at text size 1.
int Display::text_width(int face, const char* text) const {
  const gfx::Font& f = font(face);
  int left = 0, right = 0;
  for (uint32_t c : code_points(text)) {
    const gfx::Glyph& g = metric(f, c);
    if (left == 0 && right == 0 && g.xOffset < 0) left = right = -g.xOffset;
    right = left + std::max<int>(g.xAdvance, g.width + g.xOffset);
    left += g.xAdvance;
  }
  return right;
}

int Display::line_height(int face) const {
  int baseline = 0, height = 0;
  default_metric(font(face), baseline, height);
  return height;
}

// LGFXBase::draw_string at text size 1, datum top-left, no background.
void Display::draw_text(int face, const char* text, int x, int y, uint32_t rgb) {
  color_ = rgb565(rgb);
  const gfx::Font& f = font(face);
  int baseline = 0, height = 0;
  default_metric(f, baseline, height);
  std::vector<uint32_t> codes = code_points(text);
  if (codes.empty()) return;
  int cursor = x;
  const gfx::Glyph& first = metric(f, codes[0]);
  if (first.xOffset < 0) cursor -= first.xOffset;
  for (uint32_t c : codes) {
    const gfx::Glyph* g = glyph(f, c);
    if (!g) {
      // IFont::drawCharDummy: a glyph the face lacks is a box the space's advance wide and
      // the line's height tall, outlined one pixel in from its edges.
      const int w = metric(f, 0x20).xAdvance;
      if (w > 2 && height > 2) {
        const int bx = cursor + 1, by = y + 1, bw = w - 2, bh = height - 2;
        fill(bx, by, bw, 1);
        fill(bx, by + bh - 1, bw, 1);
        fill(bx, by, 1, bh);
        fill(bx + bw - 1, by, 1, bh);
      }
      cursor += w;
      continue;
    }
    const uint8_t* bits = &f.bitmap[g->bitmapOffset];
    int left = cursor + g->xOffset, top = y + baseline + g->yOffset;
    for (int k = 0; k < g->width * g->height; ++k) {
      if ((bits[k >> 3] >> (7 - (k & 7))) & 1) fill(left + k % g->width, top + k / g->width, 1, 1);
    }
    cursor += g->xAdvance;
  }
}

const uint8_t* Display::frame(uint32_t* bytes) const {
  if (bytes) *bytes = static_cast<uint32_t>(panel_.size());
  return panel_.data();
}

Link::~Link() {
  if (slave_ >= 0) close(slave_);
  if (master_ >= 0) close(master_);
}

bool Link::begin(std::string& error) {
  master_ = posix_openpt(O_RDWR | O_NOCTTY);
  if (master_ < 0 || grantpt(master_) != 0 || unlockpt(master_) != 0) {
    error = std::string("cannot open a pseudo-terminal: ") + std::strerror(errno);
    return false;
  }
  const char* name = ptsname(master_);
  if (!name) {
    error = std::string("cannot name the pseudo-terminal: ") + std::strerror(errno);
    return false;
  }
  path_ = name;
  slave_ = open(name, O_RDWR | O_NOCTTY);
  if (slave_ < 0) {
    error = "cannot open " + path_ + ": " + std::strerror(errno);
    return false;
  }
  termios raw;
  tcgetattr(slave_, &raw);
  cfmakeraw(&raw);
  tcsetattr(slave_, TCSANOW, &raw);
  fcntl(master_, F_SETFL, fcntl(master_, F_GETFL) | O_NONBLOCK);
  return true;
}

int Link::read() {
  unsigned char c = 0;
  return ::read(master_, &c, 1) == 1 ? c : -1;
}

void Link::wait(int ms) const {
  pollfd p{master_, POLLIN, 0};
  poll(&p, 1, ms);
}

void Link::write_line(const std::string& line, bool bulk) {
  std::string out = line + "\n";
  size_t sent = 0;
  while (sent < out.size()) {
    ssize_t n = ::write(master_, out.data() + sent, out.size() - sent);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR) return;
    pollfd p{master_, POLLOUT, 0};
    if (!bulk || poll(&p, 1, kBulkTimeoutMs) <= 0) return;
  }
}

namespace {

int64_t micros_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
      .count();
}

int64_t heap_in_use() {
  malloc_statistics_t st{};
  malloc_zone_statistics(nullptr, &st);
  return static_cast<int64_t>(st.size_in_use);
}

}  // namespace

Board::Board(int width, int height, int band_rows) : display_(width, height, band_rows) {}

int64_t Clock::micros() const { return manual_ ? manual_us_ : micros_since(start_); }

std::vector<rt::Counter> Board::counters() const {
  return {{"heap_in_use", heap_in_use()}, {"haptics", haptics_}};
}

void Board::command(const std::string& name, const std::vector<rt::V>& args) {
  (void)args;
  if (name == "haptic") ++haptics_;
}

void Board::idle() { link_.wait(2); }

std::vector<rt::Counter> HeadlessBoard::counters() const { return {{"heap_in_use", heap_in_use()}}; }

void HeadlessBoard::idle() { link_.wait(2); }

}  // namespace rt_mac
