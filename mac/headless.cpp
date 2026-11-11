// An app on the headless Mac board: the same generated app_gen.cpp,
// exact_rt core, and app glue, with no display. Built once per app
// (level-headless, badge-headless). The app's state, keys, taps by testId,
// routes, timers, and the agent work; touch and screenshots do not. It
// prints the pseudo-terminal it serves, a MISSING line for each part the app
// needs and the board lacks, a STALE line for each extension built from
// another copy of its file, and BOOTED; then it runs until interrupted. Drive it
// with tools/agent.py (`key`, `tap`, `navigate`, `state`, `nodes`, `stats`).
//
//   APP-headless [--watch] [--no-extensions] [--manual-clock] [--link PATH] [--clock EPOCH_MS,OFFSET]
//     --watch      print the app's state on stdout each time it changes
//     --no-extensions  give the app none of its extensions (a board that
//                  lacks them: the boot report names each one)
//     --manual-clock  the board's clock starts at 0 and moves only by the
//                  agent's `clock` op, each timer and transport wake on the way
//                  at its own time (tools/sound_check.py)
//     --link PATH  also make PATH a symbolic link to the pseudo-terminal
//     --clock EPOCH_MS,OFFSET  a wall clock: UTC milliseconds now, and the
//                  local offset in minutes east of UTC (default: no clock)
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include "app_glue.h"
#include "exact_rt.h"
#include "exact_rt_mac.h"

namespace {

const std::string kProgram = std::string(app::kName) + "-headless";

volatile std::sig_atomic_t stop = 0;

void on_signal(int) { stop = 1; }

struct Options {
  bool watch = false;
  bool extensions = true;
  bool manual_clock = false;
  std::string link;
  bool clock = false;
  long long epoch_ms = 0;
  int utc_offset = 0;
};

bool parse(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--watch") == 0) {
      o.watch = true;
    } else if (std::strcmp(argv[i], "--no-extensions") == 0) {
      o.extensions = false;
    } else if (std::strcmp(argv[i], "--manual-clock") == 0) {
      o.manual_clock = true;
    } else if (std::strcmp(argv[i], "--link") == 0 && i + 1 < argc) {
      o.link = argv[++i];
    } else if (std::strcmp(argv[i], "--clock") == 0 && i + 1 < argc &&
               std::sscanf(argv[i + 1], "%lld,%d", &o.epoch_ms, &o.utc_offset) == 2) {
      o.clock = true;
      ++i;
    } else {
      std::fprintf(stderr,
                   "usage: %s [--watch] [--no-extensions] [--manual-clock] [--link PATH] [--clock EPOCH_MS,OFFSET]\n",
                   kProgram.c_str());
      return false;
    }
  }
  return true;
}

// Prints the app's state if it differs from `last`, the state printed last.
void print_state(rt::Runtime& runtime, std::string& last) {
  std::string state = runtime.agent("{\"op\":\"state\"}");
  if (state == last) return;
  last = state;
  std::printf("STATE %s\n", state.c_str());
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse(argc, argv, options)) return 2;
  rt_mac::HeadlessBoard board;
  if (options.manual_clock) board.set_manual_clock();
  if (options.clock) board.set_clock(options.epoch_ms, options.utc_offset);
  std::string error;
  if (!board.begin(error)) {
    std::fprintf(stderr, "%s: %s\n", kProgram.c_str(), error.c_str());
    return 1;
  }
  if (!options.link.empty()) {
    unlink(options.link.c_str());
    if (symlink(board.link_path().c_str(), options.link.c_str()) != 0) {
      std::fprintf(stderr, "%s: cannot link %s to %s: %s\n", kProgram.c_str(), options.link.c_str(),
                   board.link_path().c_str(), std::strerror(errno));
      return 1;
    }
  }
  rt::Host host(app::kApp, board, options.extensions ? app::extensions() : std::vector<rt::Extension*>{});
  if (!host.boot()) {
    std::fprintf(stderr, "%s: the app did not boot\n", kProgram.c_str());
    return 1;
  }
  std::printf("PTY %s headless\n", board.link_path().c_str());
  for (const std::string& part : host.runtime().missing()) std::printf("MISSING %s\n", part.c_str());
  for (const std::string& name : host.runtime().stale()) std::printf("STALE %s\n", name.c_str());
  std::printf("BOOTED\n");
  std::fflush(stdout);
  std::string last;
  if (options.watch) print_state(host.runtime(), last);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  uint32_t commits = host.runtime().stats.commits;
  while (!stop) {
    host.turn();
    if (options.watch && host.runtime().stats.commits != commits) {
      commits = host.runtime().stats.commits;
      print_state(host.runtime(), last);
    }
  }
  if (!options.link.empty()) unlink(options.link.c_str());
  return 0;
}
