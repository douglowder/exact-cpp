// A VM-capable bundle's second entry point: the app's tables and its bodies as
// rt_vm bytecode (plan2cpp with PLAN2CPP_VM set), for writing it as a plan blob.
#include "rt_vm.h"

namespace app {
extern const rt::App kApp;
extern const rt_vm::Body kVmBodies[];
extern const uint16_t kVmBodyCount;
extern const rt_vm::Sizes kVmSizes;
}  // namespace app

extern "C" const rt_vm::Exports* exact_vm_exports() {
  static const rt_vm::Exports e{&app::kApp, app::kVmBodies, app::kVmBodyCount, &app::kVmSizes};
  return &e;
}
