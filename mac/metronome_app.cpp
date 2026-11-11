// The metronome's glue on the Mac: it lists no extensions. The Mac boards have no speaker, so
// the boot report names it; the transport still keeps its voice record (the agent's
// `sounds`), which tools/sound_check.py compares with Exact's runner.
#include "app_glue.h"

namespace app {

const char* const kName = "metronome";

std::vector<rt::Extension*> extensions() { return {}; }

}  // namespace app
