// What a Mac program needs from an app beyond its generated app_gen.cpp: the
// extensions it lists (extensions.json), as this board gives them. Each app
// links one glue file (level_app.cpp, badge_app.cpp).
#pragma once

#include <vector>

#include "exact_rt.h"

namespace app {

extern const rt::App kApp;
// The program name for messages and the PTY line, e.g. "level".
extern const char* const kName;
// The app's extensions on the Mac.
std::vector<rt::Extension*> extensions();

}  // namespace app
