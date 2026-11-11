// The motion demo's glue on the Mac: it lists no extensions.
#include "app_glue.h"

namespace app {

const char* const kName = "motion";

std::vector<rt::Extension*> extensions() { return {}; }

}  // namespace app
