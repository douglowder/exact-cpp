// The level app's glue on the Mac: the same level::Glue as the Stopwatch
// firmware, the `attitude` extension (the `attitude` source and the `tilt` op).
#include "app_glue.h"
#include "level_glue.h"

namespace app {

namespace {

level::Glue glue;

}  // namespace

const char* const kName = "level";

std::vector<rt::Extension*> extensions() { return {&glue}; }

}  // namespace app
