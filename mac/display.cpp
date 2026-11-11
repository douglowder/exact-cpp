// An app on the Mac board (with a display): the same generated app_gen.cpp,
// the same exact_rt core, and the same app glue as the Stopwatch firmware.
// Built once per app (level-mac, badge-mac). It prints the pseudo-terminal it
// serves, then runs until interrupted; drive it with tools/agent.py or
// tools/compare.py as if it were the board.
//
//   APP-mac [--band ROWS] [--link PATH] [--clock EPOCH_MS,OFFSET]
//     --band ROWS  draw the screen in bands of ROWS rows (default: all 466)
//     --link PATH  also make PATH a symbolic link to the pseudo-terminal
//     --clock EPOCH_MS,OFFSET  a wall clock: UTC milliseconds now, and the
//                  local offset in minutes east of UTC (default: no clock)
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app_glue.h"
#include "exact_rt.h"
#include "exact_rt_mac.h"
#include "speaker_mac.hpp"

namespace {

const std::string kProgram = std::string(app::kName) + "-mac";

constexpr int kWidth = 468;
constexpr int kHeight = 466;

volatile std::sig_atomic_t stop = 0;

void on_signal(int) { stop = 1; }

struct Options {
  int band = kHeight;
  std::string link;
  bool clock = false;
  long long epoch_ms = 0;
  int utc_offset = 0;
};

bool parse(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--band") == 0 && i + 1 < argc) {
      o.band = std::atoi(argv[++i]);
      if (o.band < 1 || o.band > kHeight) {
        std::fprintf(stderr, "%s: --band takes 1 to %d rows, not %s\n", kProgram.c_str(), kHeight, argv[i]);
        return false;
      }
    } else if (std::strcmp(argv[i], "--link") == 0 && i + 1 < argc) {
      o.link = argv[++i];
    } else if (std::strcmp(argv[i], "--clock") == 0 && i + 1 < argc &&
               std::sscanf(argv[i + 1], "%lld,%d", &o.epoch_ms, &o.utc_offset) == 2) {
      o.clock = true;
      ++i;
    } else {
      std::fprintf(stderr, "usage: %s [--band ROWS] [--link PATH] [--clock EPOCH_MS,OFFSET]\n", kProgram.c_str());
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse(argc, argv, options)) return 2;
  rt_mac::Board board(kWidth, kHeight, options.band);
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
  // An app with sounds plays them on the Mac's output (this program's clock is real).
  rt_mac::Speaker speaker(app::kApp, board);
  if (app::kApp.sound_count > 0) {
    if (speaker.start(error)) {
      board.set_speaker(&speaker);
    } else {
      std::fprintf(stderr, "%s: no speaker: %s\n", kProgram.c_str(), error.c_str());
    }
  }
  rt::Host host(app::kApp, board, app::extensions());
  if (!host.boot()) {
    std::fprintf(stderr, "%s: the display did not start\n", kProgram.c_str());
    return 1;
  }
  std::printf("PTY %s band=%d\n", board.link_path().c_str(), options.band);
  std::fflush(stdout);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  while (!stop) {
    host.turn();
  }
  if (!options.link.empty()) unlink(options.link.c_str());
  return 0;
}
