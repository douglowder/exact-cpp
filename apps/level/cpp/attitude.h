// The IMU attitude and filter, the same math and constants as the Rust
// level-data crate (path A), so both builds feed the app the same values.
#pragma once

#include <cmath>

namespace attitude {

struct Attitude {
  float pitch;
  float roll;
};

// Axes with the lanyard at the bottom, screen up: +x toward the lanyard,
// +y screen-left, +z out of the screen. Pitch: top edge up; roll: right edge up.
inline bool from_accel(float ax, float ay, float az, Attitude* out) {
  if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az)) return false;
  float magnitude = std::sqrt(ax * ax + ay * ay + az * az);
  if (magnitude < 0.5f || magnitude > 2.0f) return false;
  out->pitch = std::atan2(-ax, std::sqrt(ay * ay + az * az)) * 57.29578f;
  out->roll = std::atan2(-ay, std::sqrt(ax * ax + az * az)) * 57.29578f;
  return true;
}

class Filter {
 public:
  Attitude update(Attitude raw, float dt_ms) {
    if (!primed_) {
      primed_ = true;
      value_ = raw;
      return value_;
    }
    float change = std::fmax(std::fabs(raw.pitch - value_.pitch), std::fabs(raw.roll - value_.roll));
    float tau = change > 0.6f ? 25.0f : 100.0f;
    float alpha = 1.0f - std::exp(-std::fmax(dt_ms, 0.0f) / tau);
    value_.pitch += (raw.pitch - value_.pitch) * alpha;
    value_.roll += (raw.roll - value_.roll) * alpha;
    return value_;
  }

 private:
  bool primed_ = false;
  Attitude value_{0, 0};
};

// Degrees to whole tenths, limited to ±90 degrees; not-finite becomes 0.
inline int tenths(float degrees) {
  if (!std::isfinite(degrees)) return 0;
  return static_cast<int>(std::lround(std::fmax(-900.0f, std::fmin(900.0f, degrees * 10.0f))));
}

}  // namespace attitude
