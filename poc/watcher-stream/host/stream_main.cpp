// exact-stream: runs Exact apps on the Mac and shows them on a SenseCAP
// Watcher over its USB serial line. Each app is an ExactMac bundle (its
// generated app_gen.cpp and glue, built against this program's exact_rt core);
// a 2 s hold of the Watcher's knob, or `next` typed here, switches to the next
// app with no flash.
//
//   exact-stream --port /dev/cu.usbmodemXXXX [--baud N] [--apps DIR] [APP]
//
// Commands on stdin: next | app NAME | key NAME | light PERCENT | stats | keyframe
#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "exact_mac_app.hpp"
#include "exact_rt.h"
#include "plan_blob.h"
#include "speaker_mac.hpp"
#include "stream_board.h"

namespace {

volatile std::sig_atomic_t stop = 0;
void on_signal(int) { stop = 1; }

std::vector<std::string> bundles(const std::string& dir) {
  std::vector<std::string> out;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      std::string name = e->d_name;
      const std::string ext = ".bundle";
      if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
        out.push_back(name.substr(0, name.size() - ext.size()));
      }
    }
    closedir(d);
  }
  std::sort(out.begin(), out.end());
  return out;
}

struct Running {
  std::unique_ptr<rt_mac::Speaker> speaker;
  std::unique_ptr<rt::Host> host;
  std::string name;
};

bool load(xs::Board& board, Running& run, const std::string& dir, const std::string& name, bool plan, std::string& error) {
  run.host.reset();
  board.set_speaker(nullptr);
  run.speaker.reset();
  const std::string path = dir + "/" + name + ".bundle";
  void* bundle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!bundle) {
    error = dlerror();
    return false;
  }
  auto entry = reinterpret_cast<const ExactMacApp* (*)()>(dlsym(bundle, "exact_mac_app"));
  if (!entry) {
    error = path + " has no exact_mac_app entry";
    return false;
  }
  const ExactMacApp* a = entry();
  if (a->abi != kExactMacAbi) {
    error = path + ": another ABI";
    return false;
  }
  if (plan) {
    // The board runs it: write the plan blob and send it; nothing runs here.
    auto vm = reinterpret_cast<const rt_vm::Exports* (*)()>(dlsym(bundle, "exact_vm_exports"));
    if (!vm) {
      error = path + " has no exact_vm_exports (build it from PLAN2CPP_VM output)";
      return false;
    }
    std::vector<uint8_t> blob;
    if (!plan_blob::write_blob(*vm(), blob, error)) return false;
    run.name = a->name;
    std::fprintf(stderr, "%s: plan blob %zu bytes\n", a->name, blob.size());
    board.send_plan(blob);
    return true;
  }
  if (a->app->sound_count > 0) {
    run.speaker = std::make_unique<rt_mac::Speaker>(*a->app, board);
    std::string why;
    if (run.speaker->start(why)) {
      board.set_speaker(run.speaker.get());
    } else {
      std::fprintf(stderr, "exact-stream: no speaker: %s\n", why.c_str());
      run.speaker.reset();
    }
  }
  // The app's own screen size: its root's literal width and height (else the Stopwatch's).
  int w = 468, h = 466;
  if (a->app->node_count > 0) {
    const rt::Node& root = a->app->nodes[0];
    for (int i = 0; i < root.binding_count; ++i) {
      const rt::Binding& b = a->app->bindings[root.binding_start + i];
      if (b.target.prop || b.fn || b.lit.k != 1) continue;
      if (b.target.id == static_cast<uint8_t>(rt::S::Width)) w = static_cast<int>(b.lit.n);
      if (b.target.id == static_cast<uint8_t>(rt::S::Height)) h = static_cast<int>(b.lit.n);
    }
  }
  board.resize(w, h);
  std::fprintf(stderr, "%s: %dx%d\n", a->name, w, h);
  run.host = std::make_unique<rt::Host>(*a->app, board, a->extensions());
  if (!run.host->boot()) {
    error = name + ": the display did not start";
    run.host.reset();
    return false;
  }
  run.name = a->name;
  for (const std::string& m : run.host->runtime().missing()) std::fprintf(stderr, "%s: missing %s\n", a->name, m.c_str());
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string port, dir, first;
  bool plan = false;
  int baud = XS_BAUD;
  int width = 468, height = 466;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = argv[++i];
    else if (!std::strcmp(argv[i], "--baud") && i + 1 < argc) baud = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--apps") && i + 1 < argc) dir = argv[++i];
    else if (!std::strcmp(argv[i], "--plan")) plan = true;
    else if (!std::strcmp(argv[i], "--size") && i + 1 < argc && std::sscanf(argv[i + 1], "%dx%d", &width, &height) == 2) ++i;
    else if (argv[i][0] != '-') first = argv[i];
    else {
      std::fprintf(stderr, "usage: exact-stream --port PATH [--baud N] [--apps DIR] [--size WxH] [APP]\n");
      return 2;
    }
  }
  if (port.empty()) {
    std::fprintf(stderr, "exact-stream: --port is required\n");
    return 2;
  }
  if (dir.empty()) {
    std::string self = argv[0];
    dir = self.substr(0, self.find_last_of('/') + 1) + "apps";
  }
  const std::vector<std::string> names = bundles(dir);
  if (names.empty()) {
    std::fprintf(stderr, "exact-stream: no app bundles in %s\n", dir.c_str());
    return 1;
  }
  xs::Board board(width, height);
  std::string error;
  if (!board.begin(port, baud, error)) {
    std::fprintf(stderr, "exact-stream: %s\n", error.c_str());
    return 1;
  }
  Running run;
  size_t index = 0;
  if (!first.empty()) {
    auto it = std::find(names.begin(), names.end(), first);
    if (it == names.end()) {
      std::fprintf(stderr, "exact-stream: no app %s in %s\n", first.c_str(), dir.c_str());
      return 1;
    }
    index = static_cast<size_t>(it - names.begin());
  }
  if (plan) {
    // Wait for the board before sending it a plan.
    for (int t = 0; t < 400 && !board.connected(); ++t) {
      board.step(0);
      board.idle();
      usleep(5000);
    }
    if (!board.connected()) {
      std::fprintf(stderr, "exact-stream: no board on %s\n", port.c_str());
      return 1;
    }
  }
  if (!load(board, run, dir, names[index], plan, error)) {
    std::fprintf(stderr, "exact-stream: %s\n", error.c_str());
    return 1;
  }
  std::printf("PTY %s app=%s apps=%zu\n", board.link_path().c_str(), run.name.c_str(), names.size());
  std::fflush(stdout);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
  std::string line;
  auto switch_to = [&](size_t i) {
    index = i % names.size();
    if (!load(board, run, dir, names[index], plan, error)) {
      std::fprintf(stderr, "exact-stream: %s\n", error.c_str());
      return;
    }
    std::printf("APP %s\n", run.name.c_str());
    std::fflush(stdout);
  };
  bool was_connected = false;
  while (!stop) {
    if (run.host) {
      run.host->turn();
    } else {
      board.step(0);
      board.idle();
    }
    if (board.connected() && !was_connected) {
      std::printf("CONNECTED %dx%d\n", board.board_width(), board.board_height());
      std::fflush(stdout);
    }
    was_connected = board.connected();
    if (board.take_next_app()) switch_to(index + 1);
    char c;
    while (read(0, &c, 1) == 1) {
      if (c != '\n') {
        line += c;
        continue;
      }
      if (line == "next") switch_to(index + 1);
      else if (line.rfind("app ", 0) == 0) {
        auto it = std::find(names.begin(), names.end(), line.substr(4));
        if (it != names.end()) switch_to(static_cast<size_t>(it - names.begin()));
        else std::printf("ERR no app %s\n", line.substr(4).c_str());
      } else if (line.rfind("key ", 0) == 0 && plan) {
        board.send_agent("{\"op\":\"key\",\"key\":\"" + line.substr(4) + "\"}");
      } else if (line.rfind("agent ", 0) == 0) {
        board.send_agent(line.substr(6));
      } else if (line == "plan" || line == "stream") {
        plan = line == "plan";
        if (!plan) board.leave_plan();
        switch_to(index);
      } else if (line.rfind("key ", 0) == 0) {
        static std::vector<std::string> keys;  // names stay alive for the queue
        keys.push_back(line.substr(4));
        board.push_key(keys.back().c_str());
      } else if (line.rfind("light ", 0) == 0) {
        board.set_light(std::atoi(line.c_str() + 6));
      } else if (line == "keyframe") {
        board.keyframe();
      } else if (line == "stats") {
        if (plan) board.send_agent("{\"op\":\"stats\"}");
        else std::printf("STATS app=%s %s\n", run.name.c_str(), board.stats_line().c_str());
      }
      std::fflush(stdout);
      line.clear();
    }
  }
  return 0;
}
