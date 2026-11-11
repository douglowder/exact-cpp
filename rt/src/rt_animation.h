// CSS animations sampled in closed form, as Exact's motion crate samples
// them (motion/src/animation.rs, easing.rs): local time in, one property's
// value out, so a seek and sixty frames give the same values.
#pragma once

#include <cstdint>

#include "exact_rt.h"

namespace rt {

// A property value: four components, as `KeyValue::v`.
struct AValue {
  double v[4] = {0, 0, 0, 0};
};

// Timing function `e` at input progress `x` (clamped to 0–1).
double ease(const App& app, const Ease& e, double x);

// The directed progress of `a` at local time `t_ms` (since it started, its
// delay included); false when it has no effect then (Web Animations §4.8).
bool directed_progress(const Animation& a, double t_ms, double& p);

// Whether `a`'s keyframes set `prop`.
bool animates(const App& app, const Animation& a, AP prop);

// `prop`'s value at local time `t_ms` over `underlying` (its value without the
// animation); false when the animation sets no `prop` or has no effect then.
bool sample(const App& app, const Animation& a, double t_ms, AP prop, const AValue& underlying, AValue& out);

// A 0xAARRGGBB color (-1: transparent) as premultiplied sRGB, and back, as
// motion's `Value::rgba8` and `Value::to_rgba8` convert.
AValue color_value(int64_t argb);
int64_t color_argb(const AValue& v);

}  // namespace rt
