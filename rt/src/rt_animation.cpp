// CSS animations: a port of Exact's motion crate (motion/src/animation.rs and
// easing.rs) over the tables plan2cpp writes.
#include "rt_animation.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rt {

namespace {

double bezier_axis(double c1, double c2, double t) {
  const double inverse = 1.0 - t;
  return 3.0 * inverse * inverse * t * c1 + 3.0 * inverse * t * t * c2 + t * t * t;
}

double bezier_axis_derivative(double c1, double c2, double t) {
  const double inverse = 1.0 - t;
  return 3.0 * inverse * inverse * c1 + 6.0 * inverse * t * (c2 - c1) + 3.0 * t * t * (1.0 - c2);
}

// Newton's method with a bisection fallback, as browsers solve it.
double solve_bezier_t(double x1, double x2, double x) {
  const double epsilon = 1e-7;
  double t = x;
  for (int k = 0; k < 8; ++k) {
    const double error = bezier_axis(x1, x2, t) - x;
    if (std::fabs(error) < epsilon) return t;
    const double slope = bezier_axis_derivative(x1, x2, t);
    if (std::fabs(slope) < 1e-6) break;
    t -= error / slope;
  }
  double low = 0.0, high = 1.0;
  t = x;
  for (int k = 0; k < 64; ++k) {
    if (high - low <= epsilon) break;
    if (bezier_axis(x1, x2, t) < x) low = t;
    else high = t;
    t = (low + high) * 0.5;
  }
  return t;
}

double bezier(const Ease& e, double x) {
  if (x <= 0.0) return 0.0;
  if (x >= 1.0) return 1.0;
  return bezier_axis(e.b, e.d, solve_bezier_t(e.a, e.c, x));
}

// CSS Easing §2.3.2, the step-easing output (before flag unset).
double steps(const Ease& e, double x) {
  const double count = e.a;
  const int position = static_cast<int>(e.b);
  double current = std::floor(x * count);
  if (position == 0 || position == 3) current += 1.0;
  if (current < 0.0) current = 0.0;
  const double jumps = position == 2 ? count - 1.0 : position == 3 ? count + 1.0 : count;
  if (current > jumps) current = jumps;
  return current / jumps;
}

// CSS Easing 2's linear(), with Chrome's hold before two first stops that
// share an input.
double piecewise(const App& app, const Ease& e, double x) {
  const int n = e.stop_count;
  if (n == 0) return x;
  auto in = [&](int k) { return app.ease_stops[2 * (e.stop_start + k)]; };
  auto out = [&](int k) { return app.ease_stops[2 * (e.stop_start + k) + 1]; };
  if (n == 1) return out(0);
  for (int k = n - 1; k >= 0; --k) {
    if (in(k) == x) return out(k);
  }
  int a = 0, b = 1;
  if (x < in(0)) {
    a = 0;
    b = 1;
  } else if (x > in(n - 1)) {
    a = n - 2;
    b = n - 1;
  } else {
    b = 1;
    while (b < n && !(in(b) > x)) ++b;
    a = b - 1;
  }
  if (in(a) == in(b)) return out(b);
  return out(a) + (out(b) - out(a)) * (x - in(a)) / (in(b) - in(a));
}

double active_duration(const Animation& a) {
  return a.duration_ms == 0.0 || a.iterations == 0.0 ? 0.0 : a.duration_ms * a.iterations;
}

enum class Phase { Before, Active, After };

Phase phase(const Animation& a, double t) {
  const double end = std::max(a.delay_ms + active_duration(a), 0.0);
  const double before = std::max(std::min(a.delay_ms, end), 0.0);
  const double after = std::max(std::min(a.delay_ms + active_duration(a), end), 0.0);
  if (t < before) return Phase::Before;
  if (t >= after) return Phase::After;
  return Phase::Active;
}

bool fills_backwards(const Animation& a) { return a.fill == 2 || a.fill == 3; }
bool fills_forwards(const Animation& a) { return a.fill == 1 || a.fill == 3; }

struct Stop {
  double offset;
  int ease;  // -1: the animation's
  AValue value;
};

// The keyframes `prop` takes part in, with the implicit 0% and 100% from
// `underlying` where no keyframe declares them (CSS Animations 1 §3).
std::vector<Stop> track(const App& app, const Animation& a, AP prop, const AValue& underlying) {
  std::vector<Stop> out;
  for (uint16_t k = 0; k < a.value_count; ++k) {
    const KeyValue& kv = app.key_values[a.value_start + k];
    if (kv.prop != prop) continue;
    Stop s{kv.offset, kv.ease, AValue{}};
    for (int c = 0; c < 4; ++c) s.value.v[c] = kv.v[c];
    out.push_back(s);
  }
  if (out.empty() || out.front().offset > 0.0) out.insert(out.begin(), Stop{0.0, -1, underlying});
  if (out.back().offset < 1.0) out.push_back(Stop{1.0, -1, underlying});
  return out;
}

}  // namespace

double ease(const App& app, const Ease& e, double x) {
  x = std::min(1.0, std::max(0.0, x));
  switch (e.kind) {
    case 1: return bezier(e, x);
    case 2: return steps(e, x);
    case 3: return piecewise(app, e, x);
    default: return x;
  }
}

bool directed_progress(const Animation& a, double t, double& p) {
  if (std::isnan(t)) return false;
  const double ad = active_duration(a);
  const Phase ph = phase(a, t);
  double active = 0;
  switch (ph) {
    case Phase::Before:
      if (!fills_backwards(a)) return false;
      active = std::max(t - a.delay_ms, 0.0);
      break;
    case Phase::Active: active = t - a.delay_ms; break;
    case Phase::After:
      if (!fills_forwards(a)) return false;
      active = std::max(std::min(t - a.delay_ms, ad), 0.0);
      break;
  }
  const double overall = a.duration_ms == 0.0 ? (ph == Phase::Before ? 0.0 : a.iterations) : active / a.duration_ms;
  double simple = std::isinf(overall) ? 0.0 : std::fmod(overall, 1.0);
  if (simple == 0.0 && ph != Phase::Before && active == ad && a.iterations != 0.0) simple = 1.0;
  double iteration;
  if (ph == Phase::After && std::isinf(a.iterations)) iteration = INFINITY;
  else if (simple == 1.0) iteration = std::floor(overall) - 1.0;
  else iteration = std::floor(overall);
  const bool odd = std::isfinite(iteration) && ((static_cast<int64_t>(iteration) % 2) + 2) % 2 == 1;
  bool forwards = true;
  switch (a.direction) {
    case 1: forwards = false; break;
    case 2: forwards = !odd; break;
    case 3: forwards = odd; break;
    default: break;
  }
  p = forwards ? simple : 1.0 - simple;
  return true;
}

bool animates(const App& app, const Animation& a, AP prop) {
  for (uint16_t k = 0; k < a.value_count; ++k) {
    if (app.key_values[a.value_start + k].prop == prop) return true;
  }
  return false;
}

bool sample(const App& app, const Animation& a, double t_ms, AP prop, const AValue& underlying, AValue& out) {
  if (!animates(app, a, prop)) return false;
  double p = 0;
  if (!directed_progress(a, t_ms, p)) return false;
  const std::vector<Stop> tr = track(app, a, prop, underlying);
  const size_t n = tr.size();
  // The interval: the last keyframe at or before p (the last of equal
  // offsets) and the next; at p = 1 the final interval.
  size_t start = 0;
  for (size_t i = 0; i < n; ++i) {
    if (tr[i].offset <= p && i + 1 < n) start = i;
  }
  if (p >= 1.0) {
    start = n >= 2 ? n - 2 : 0;
    while (start > 0 && tr[start].offset >= 1.0) --start;
  }
  const Stop& from = tr[start];
  const Stop& to = tr[std::min(start + 1, n - 1)];
  const double span = to.offset - from.offset;
  if (span <= 0.0) {
    out = p >= to.offset ? to.value : from.value;
    return true;
  }
  const double local = (p - from.offset) / span;
  const Ease& e = app.eases[from.ease >= 0 ? from.ease : a.ease];
  const double eased = ease(app, e, local);
  for (int c = 0; c < 4; ++c) out.v[c] = from.value.v[c] + (to.value.v[c] - from.value.v[c]) * eased;
  return true;
}

AValue color_value(int64_t argb) {
  AValue out;
  if (argb < 0) return out;
  const double a = static_cast<double>((argb >> 24) & 0xff) / 255.0;
  const double r = static_cast<double>((argb >> 16) & 0xff) / 255.0;
  const double g = static_cast<double>((argb >> 8) & 0xff) / 255.0;
  const double b = static_cast<double>(argb & 0xff) / 255.0;
  out.v[0] = r * a;
  out.v[1] = g * a;
  out.v[2] = b * a;
  out.v[3] = a;
  return out;
}

int64_t color_argb(const AValue& v) {
  const double a = std::min(1.0, std::max(0.0, v.v[3]));
  if (a <= 0.0) return 0;
  auto q = [](double x) { return static_cast<int64_t>(std::min(255.0, std::max(0.0, std::round(x * 255.0)))); };
  return (q(a) << 24) | (q(v.v[0] / a) << 16) | (q(v.v[1] / a) << 8) | q(v.v[2] / a);
}

}  // namespace rt
