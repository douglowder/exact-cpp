// The knob demo's glue (apps/knob): it lists no extensions.
#include "app_glue.h"

namespace app {

const char* const kName = "knob";

std::vector<rt::Extension*> extensions() { return {}; }

}  // namespace app
