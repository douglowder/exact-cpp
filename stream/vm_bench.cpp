// vm_bench: times one app run compiled (its bundle's C++) against the same app
// run as a plan blob through the interpreter, on the same manual clock and the
// same inputs, and reports the plan's memory footprint.
//
//   vm_bench APP.bundle [REPEATS] [dense]
//
// `dense` drives an input on every turn instead of every fourth, so the app's
// action handlers run on each one.
//
// Both runtimes paint through the same rt core onto the same kind of surface,
// so a turn's time includes paint, which is common to both: the ratio below is
// what an app's responsiveness actually sees, not the interpreter's cost alone.
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <deque>
#include <string>
#include <vector>

#include "exact_mac_app.hpp"
#include "exact_rt.h"
#include "exact_rt_mac.h"
#include "plan_blob.h"
#include "rt_vm.h"

namespace {

using Clock = std::chrono::steady_clock;

double us_since(Clock::time_point t) {
  return std::chrono::duration<double, std::micro>(Clock::now() - t).count();
}

// ---- The plan's memory footprint: count what Loaded allocates ----
size_t g_live = 0, g_peak = 0, g_total = 0, g_blocks = 0;

void* counting_alloc(size_t bytes) {
  void* p = std::malloc(bytes + 16);
  if (!p) return nullptr;
  *static_cast<size_t*>(p) = bytes;
  g_live += bytes, g_total += bytes, ++g_blocks;
  g_peak = std::max(g_peak, g_live);
  return static_cast<char*>(p) + 16;
}

void counting_release(void* p) {
  if (!p) return;
  char* base = static_cast<char*>(p) - 16;
  g_live -= *reinterpret_cast<size_t*>(base);
  std::free(base);
}

class BenchBoard final : public rt::Board {
 public:
  BenchBoard(int w, int h) : display_(w, h, h) {}
  int64_t micros() const override { return us_; }
  bool advance_us(int64_t us) override {
    us_ += us;
    return true;
  }
  rt::Display* display() override { return &display_; }
  rt::Inputs inputs() const override { return {true, true}; }
  bool input(rt::Input& out) override {
    if (queue_.empty()) return false;
    out = queue_.front();
    queue_.pop_front();
    return true;
  }
  bool wall_clock(int64_t& epoch_ms, int& offset) const override {
    epoch_ms = 1795290000000LL + us_ / 1000;
    offset = -480;
    return true;
  }
  bool can(const std::string& n) const override { return n == "haptic"; }
  void push(const rt::Input& in) { queue_.push_back(in); }
  void tick(int64_t us) { us_ += us; }

 private:
  rt_mac::Display display_;
  int64_t us_ = 0;
  std::deque<rt::Input> queue_;
};

void size_of(const rt::App& a, int& w, int& h) {
  w = 468, h = 466;
  const rt::Node& root = a.nodes[0];
  for (int i = 0; i < root.binding_count; ++i) {
    const rt::Binding& b = a.bindings[root.binding_start + i];
    if (b.target.prop || b.fn || b.lit.k != 1) continue;
    if (b.target.id == static_cast<uint8_t>(rt::S::Width)) w = static_cast<int>(b.lit.n);
    if (b.target.id == static_cast<uint8_t>(rt::S::Height)) h = static_cast<int>(b.lit.n);
  }
}

struct Step {
  const char* key;
  int x, y;
};

std::vector<Step> script_for(int w, int h) {
  std::vector<Step> script;
  const char* keys[] = {"ArrowRight", "ArrowRight", "Enter", "ArrowLeft", "Escape", "ArrowRight"};
  for (const char* k : keys) script.push_back({k, 0, 0});
  for (int y = h / 6; y < h; y += h / 5)
    for (int x = w / 6; x < w; x += w / 4) script.push_back({nullptr, x, y});
  for (const char* k : keys) script.push_back({k, 0, 0});
  return script;
}

void feed(BenchBoard& bd, const Step& s) {
  rt::Input in;
  if (s.key) {
    in.k = rt::Input::K::Key;
    in.key = s.key;
    bd.push(in);
    return;
  }
  in.k = rt::Input::K::Down, in.x = s.x, in.y = s.y;
  bd.push(in);
  in.k = rt::Input::K::Up;
  bd.push(in);
}

struct Timing {
  double boot_us = 0;
  std::vector<double> turns;
};

double pct(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  size_t i = static_cast<size_t>(p * (v.size() - 1));
  return v[i];
}

double mean(const std::vector<double>& v) {
  double s = 0;
  for (double x : v) s += x;
  return v.empty() ? 0 : s / v.size();
}

// Runs one app `repeats` times over the script, timing boot and every turn.
// `dense` feeds an input before every turn instead of every fourth, so the
// app's action handlers - its plan code - run on each one.
Timing run(const rt::App& app, const std::vector<rt::Extension*>& ext, int w, int h, int repeats, bool dense) {
  Timing t;
  BenchBoard bd(w, h);
  rt::Host host(app, bd, ext);
  Clock::time_point b = Clock::now();
  if (!host.boot()) {
    std::fprintf(stderr, "vm_bench: boot failed\n");
    std::exit(1);
  }
  t.boot_us = us_since(b);
  const std::vector<Step> script = script_for(w, h);
  t.turns.reserve(static_cast<size_t>(repeats) * script.size() * 4);
  for (int r = 0; r < repeats; ++r)
    for (const Step& s : script) {
      if (!dense) feed(bd, s);
      for (int k = 0; k < 4; ++k) {
        if (dense) feed(bd, s);
        bd.tick(50000);
        Clock::time_point u = Clock::now();
        host.turn();
        t.turns.push_back(us_since(u));
      }
    }
  return t;
}

void report(const char* what, const Timing& t) {
  std::printf("  %-12s boot %8.2f ms   turns %5zu   mean %8.1f us   p50 %8.1f us   p95 %8.1f us   total %7.1f ms\n",
              what, t.boot_us / 1000.0, t.turns.size(), mean(t.turns), pct(t.turns, 0.50), pct(t.turns, 0.95),
              std::accumulate(t.turns.begin(), t.turns.end(), 0.0) / 1000.0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: vm_bench APP.bundle [REPEATS] [dense]\n");
    return 2;
  }
  const int repeats = argc > 2 ? std::atoi(argv[2]) : 5;
  const bool dense = argc > 3 && std::strcmp(argv[3], "dense") == 0;
  void* bundle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!bundle) {
    std::fprintf(stderr, "vm_bench: %s\n", dlerror());
    return 1;
  }
  auto mac = reinterpret_cast<const ExactMacApp* (*)()>(dlsym(bundle, "exact_mac_app"));
  auto vm = reinterpret_cast<const rt_vm::Exports* (*)()>(dlsym(bundle, "exact_vm_exports"));
  if (!mac || !vm) {
    std::fprintf(stderr, "vm_bench: %s is not a VM-capable bundle\n", argv[1]);
    return 1;
  }
  const rt_vm::Exports* e = vm();
  std::vector<uint8_t> blob;
  std::string error;
  if (!plan_blob::write_blob(*e, blob, error)) {
    std::fprintf(stderr, "vm_bench: %s\n", error.c_str());
    return 1;
  }
  int w, h;
  size_of(*e->app, w, h);

  // Warm up both paths, so neither pays first-touch costs in the timed run.
  run(*e->app, mac()->extensions(), w, h, 1, dense);

  std::printf("%s (%dx%d, %d repeats of a 32-step script, 4 turns each, %s input)\n", mac()->name, w, h, repeats,
              dense ? "per-turn" : "per-step");
  std::printf("  blob         %zu bytes, %u bodies\n", blob.size(), e->body_count);

  const Timing native = run(*e->app, mac()->extensions(), w, h, repeats, dense);

  plan_blob::allocate = counting_alloc;
  plan_blob::release = counting_release;
  plan_blob::Loaded loaded;
  Clock::time_point l = Clock::now();
  if (!loaded.load(blob.data(), blob.size(), error)) {
    std::fprintf(stderr, "vm_bench: %s\n", error.c_str());
    return 1;
  }
  const double load_us = us_since(l);
  std::printf("  plan in RAM  %zu bytes live in %zu blocks (peak %zu), load %.2f ms\n", g_live, g_blocks, g_peak,
              load_us / 1000.0);

  run(loaded.app(), mac()->extensions(), w, h, 1, dense);  // warm the interpreter
  const Timing interp = run(loaded.app(), mac()->extensions(), w, h, repeats, dense);

  report("compiled", native);
  report("interpreted", interp);
  const double rm = mean(interp.turns) / mean(native.turns);
  const double rb = interp.boot_us / native.boot_us;
  std::printf("  ratio        turn mean %.2fx, p50 %.2fx, boot %.2fx\n", rm,
              pct(interp.turns, 0.50) / pct(native.turns, 0.50), rb);
  return 0;
}
