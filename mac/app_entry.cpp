// An app bundle's entry point for ExactMac: the app, its name and its extensions, from the
// same app glue the Mac programs link (level_app.cpp, badge_app.cpp, metronome_app.cpp).
#include "app_glue.h"
#include "exact_mac_app.hpp"

extern "C" const ExactMacApp* exact_mac_app() {
  static const ExactMacApp app{kExactMacAbi, &app::kApp, app::kName, &app::extensions};
  return &app;
}
