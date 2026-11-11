// The shape algorithms of M5GFX 0.2.26 (LGFXBase.cpp, MIT license, M5Stack
// and lovyan03), as templates over a `span(x, y, w, h)` that fills a
// rectangle: the board defaults fill with `fill_rect`, and the core's canvas
// with clipping and opacity, so every board draws the Stopwatch's pixels.
#pragma once

#include "exact_rt.h"

namespace rt {
namespace shapes {

// LGFXBase::fillRoundRect.
template <class Span>
void fill_round_rect(Rect rect, int r, Span span) {
  int x = rect.x, y = rect.y, w = rect.w, h = rect.h;
  if (w < 0) x += w, w = -w;
  if (h < 0) y += h, h = -h;
  if (!w || !h) return;
  int y2 = y + r;
  int y1 = y + h - r - 1;
  int ddF_y = -(r << 1);
  int delta = w + ddF_y;
  span(x, y2, w, h + ddF_y);
  int x0 = x + r;
  int f = 1 - r;
  int ddF_x = 1;
  int len = 0;
  for (int i = 0; i <= r; i++) {
    len++;
    if (f >= 0) {
      span(x0 - r, y2 - i, (r << 1) + delta, len);
      span(x0 - r, y1 + i - len + 1, (r << 1) + delta, len);
      if (i == r) break;
      len = 0;
      span(x0 - i, y1 + r, (i << 1) + delta, 1);
      ddF_y += 2;
      f += ddF_y;
      span(x0 - i, y2 - r, (i << 1) + delta, 1);
      r--;
    }
    ddF_x += 2;
    f += ddF_x;
  }
}

// LGFXBase::drawRoundRect.
template <class Span>
void draw_round_rect(Rect rect, int r, Span span) {
  int x = rect.x, y = rect.y, w = rect.w, h = rect.h;
  if (w < 0) x += w, w = -w;
  if (h < 0) y += h, h = -h;
  if (!w || !h) return;
  w--;
  h--;
  int len = (r << 1) + 1;
  int y1 = y + h - r;
  int y0 = y + r;
  span(x, y0 + 1, 1, h - len);
  span(x + w, y0 + 1, 1, h - len);
  int x1 = x + w - r;
  int x0 = x + r;
  span(x0 + 1, y, w - len, 1);
  span(x0 + 1, y + h, w - len, 1);
  int f = 1 - r;
  int ddF_y = -(r << 1);
  int ddF_x = 1;
  len = 0;
  for (int i = 0; i <= r; i++) {
    len++;
    if (f >= 0) {
      span(x0 - i, y0 - r, len, 1);
      span(x0 - i, y1 + r, len, 1);
      span(x1 + i - len + 1, y1 + r, len, 1);
      span(x1 + i - len + 1, y0 - r, len, 1);
      span(x1 + r, y1 + i - len + 1, 1, len);
      span(x0 - r, y1 + i - len + 1, 1, len);
      span(x1 + r, y0 - i, 1, len);
      span(x0 - r, y0 - i, 1, len);
      len = 0;
      r--;
      ddF_y += 2;
      f += ddF_y;
    }
    ddF_x += 2;
    f += ddF_x;
  }
}

// LGFXBase::fillCircleHelper.
template <class Span>
void fill_circle_helper(int x, int y, int r, int corners, int delta, Span span) {
  if (r <= 0) return;
  ++delta;
  int f = 1 - r;
  int ddF_y = -(r << 1);
  int ddF_x = 1;
  int i = 0;
  do {
    int len = 0;
    while (f < 0) {
      f += (ddF_x += 2);
      ++len;
    }
    i += len;
    f += (ddF_y += 2);
    if (corners & 0x1) {
      if (len) span(x - r, y + i - len + 1, (r << 1) + delta, len);
      span(x - i, y + r, (i << 1) + delta, 1);
    }
    if (corners & 0x2) {
      span(x - i, y - r, (i << 1) + delta, 1);
      if (len) span(x - r, y - i, (r << 1) + delta, len);
    }
  } while (i < --r);
}

// LGFXBase::fillCircle.
template <class Span>
void fill_circle(int x, int y, int r, Span span) {
  span(x - r, y, (r << 1) + 1, 1);
  fill_circle_helper(x, y, r, 3, 0, span);
}

// LGFXBase::drawCircle.
template <class Span>
void draw_circle(int x, int y, int r, Span span) {
  if (r <= 0) {
    span(x, y, 1, 1);
    return;
  }
  int f = 1 - r;
  int ddF_y = -(r << 1);
  int ddF_x = 1;
  int i = 0;
  int j = -1;
  do {
    while (f < 0) {
      ++i;
      f += (ddF_x += 2);
    }
    f += (ddF_y += 2);
    span(x - i, y + r, i - j, 1);
    span(x - i, y - r, i - j, 1);
    span(x + j + 1, y - r, i - j, 1);
    span(x + j + 1, y + r, i - j, 1);
    span(x + r, y + j + 1, 1, i - j);
    span(x + r, y - i, 1, i - j);
    span(x - r, y - i, 1, i - j);
    span(x - r, y + j + 1, 1, i - j);
    j = i;
  } while (i < --r);
}

}  // namespace shapes
}  // namespace rt
