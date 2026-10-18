// An app bundle's entry point for ContractMac: the app, its name and its extensions, from the
// same app glue the Mac programs link (level_app.cpp, badge_app.cpp, metronome_app.cpp).
#include "app_glue.h"
#include "contract_mac_app.hpp"

extern "C" const ContractMacApp* contract_mac_app() {
  static const ContractMacApp app{kContractMacAbi, &app::kApp, app::kName, &app::extensions};
  return &app;
}
