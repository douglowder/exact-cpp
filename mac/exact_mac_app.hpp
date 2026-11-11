// What an app bundle gives ExactMac (container.mm): one C entry point, `exact_mac_app`,
// built into each bundle beside the app's app_gen.cpp and glue (app_entry.cpp). The bundle
// links against the container's own exact_rt core, so both are built from one checkout;
// `abi` refuses a bundle built against another layout.
#pragma once

#include <cstdint>
#include <vector>

#include "exact_rt.h"

constexpr uint32_t kExactMacAbi = 1;

struct ExactMacApp {
  uint32_t abi;
  const rt::App* app;
  const char* name;
  std::vector<rt::Extension*> (*extensions)();
};

extern "C" const ExactMacApp* exact_mac_app();
