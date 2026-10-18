// What an app bundle gives ContractMac (container.mm): one C entry point, `contract_mac_app`,
// built into each bundle beside the app's app_gen.cpp and glue (app_entry.cpp). The bundle
// links against the container's own contract_rt core, so both are built from one checkout;
// `abi` refuses a bundle built against another layout.
#pragma once

#include <cstdint>
#include <vector>

#include "contract_rt.h"

constexpr uint32_t kContractMacAbi = 1;

struct ContractMacApp {
  uint32_t abi;
  const rt::App* app;
  const char* name;
  std::vector<rt::Extension*> (*extensions)();
};

extern "C" const ContractMacApp* contract_mac_app();
