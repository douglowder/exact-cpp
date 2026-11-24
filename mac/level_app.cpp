// The level app's glue on the Mac: the same level::Glue as the Stopwatch
// firmware, the `attitude` extension (the `attitude` source and the `tilt` op).
#include <memory>
#include <vector>

#include "app_glue.h"
#include "level_glue.h"

namespace app {

const char* const kName = "level";

std::vector<rt::Extension*> extensions() {
  // One Glue per runtime, as in badge_app.cpp: an extension holds state for the
  // runtime it senses, so two runtimes in one process each need their own.
  static std::vector<std::unique_ptr<level::Glue>> owned;
  owned.push_back(std::make_unique<level::Glue>());
  return {owned.back().get()};
}

}  // namespace app
