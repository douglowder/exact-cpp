#include "level_glue.h"

namespace level {

namespace {

constexpr uint32_t kImuPeriodMs = 10;

}  // namespace

bool Glue::attitude(Attitude& out) {
  out.pitch = pitch_tenths_;
  out.roll = roll_tenths_;
  return true;
}

void Glue::sense(rt::Runtime& runtime) {
  const uint32_t now = runtime.board().millis();
  if (injected_ || (sensed_ && now - last_ms_ < kImuPeriodMs)) return;
  float ax = 0, ay = 0, az = 0;
  rt::Accelerometer* accel = runtime.board().accelerometer();
  if (!accel || !accel->read(ax, ay, az)) return;
  attitude::Attitude raw;
  if (attitude::from_accel(ax, ay, az, &raw)) {
    attitude::Attitude a = filter_.update(raw, static_cast<float>(now - last_ms_));
    int p = attitude::tenths(a.pitch), r = attitude::tenths(a.roll);
    if (p != pitch_tenths_ || r != roll_tenths_) {
      pitch_tenths_ = p;
      roll_tenths_ = r;
      attitude_changed(runtime);
    }
  }
  sensed_ = true;
  last_ms_ = now;
}

std::string Glue::tilt(rt::Runtime& runtime, double pitch, double roll) {
  injected_ = true;
  pitch_tenths_ = attitude::tenths(static_cast<float>(pitch));
  roll_tenths_ = attitude::tenths(static_cast<float>(roll));
  attitude_changed(runtime);
  return "{\"ok\":true}";
}

}  // namespace level
