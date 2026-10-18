// The level app's `attitude` extension (extensions/attitude.json), the same
// on every board: the `attitude` source (filtered accelerometer, in tenths of
// a degree), and the `tilt` agent op, which injects an attitude and stops the
// live sensor so scripted runs see the same values on every board.
#pragma once

#include <string>

#include "attitude.h"
#include "contract_rt.h"
#include "ext_attitude.h"

namespace level {

class Glue : public ext::AttitudeExtension {
 public:
  bool attitude(Attitude& out) override;
  // Reads the board's accelerometer every 10 ms and tells the runtime when
  // the rounded attitude changes. Without an accelerometer the attitude stays
  // level until a `tilt` op sets it.
  void sense(rt::Runtime& runtime) override;
  std::string tilt(rt::Runtime& runtime, double pitch, double roll) override;

 private:
  attitude::Filter filter_;
  int pitch_tenths_ = 0;
  int roll_tenths_ = 0;
  bool injected_ = false;
  bool sensed_ = false;
  uint32_t last_ms_ = 0;
};

}  // namespace level
