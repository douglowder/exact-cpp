// vm_check: runs one app twice on the Mac, compiled (its bundle's C++) and as
// a plan blob through the interpreter, with the same inputs on the same manual
// clock, and requires every frame to be the same, pixel for pixel.
//
//   vm_check APP.bundle [OUT.blob]
#include <dlfcn.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "exact_mac_app.hpp"
#include "exact_rt.h"
#include "exact_rt_mac.h"
#include "plan_blob.h"
#include "rt_vm.h"

namespace {

class CheckBoard final : public rt::Board {
 public:
  CheckBoard(int w, int h) : display_(w, h, h) {}
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
  const uint8_t* frame(uint32_t* n) const { return display_.frame(n); }

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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: vm_check APP.bundle [OUT.blob]\n");
    return 2;
  }
  void* bundle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!bundle) {
    std::fprintf(stderr, "vm_check: %s\n", dlerror());
    return 1;
  }
  auto mac = reinterpret_cast<const ExactMacApp* (*)()>(dlsym(bundle, "exact_mac_app"));
  auto vm = reinterpret_cast<const rt_vm::Exports* (*)()>(dlsym(bundle, "exact_vm_exports"));
  if (!mac || !vm) {
    std::fprintf(stderr, "vm_check: %s is not a VM-capable bundle\n", argv[1]);
    return 1;
  }
  const rt_vm::Exports* e = vm();
  std::vector<uint8_t> blob;
  std::string error;
  if (!plan_blob::write_blob(*e, blob, error)) {
    std::fprintf(stderr, "vm_check: %s\n", error.c_str());
    return 1;
  }
  if (argc > 2) {
    FILE* f = std::fopen(argv[2], "wb");
    std::fwrite(blob.data(), 1, blob.size(), f);
    std::fclose(f);
  }
  plan_blob::Loaded loaded;
  if (!loaded.load(blob.data(), blob.size(), error)) {
    std::fprintf(stderr, "vm_check: %s\n", error.c_str());
    return 1;
  }
  int w, h;
  size_of(*e->app, w, h);
  CheckBoard native(w, h), interp(w, h);
  rt::Host a(*e->app, native, mac()->extensions());
  rt::Host b(loaded.app(), interp, mac()->extensions());
  if (!a.boot() || !b.boot()) {
    std::fprintf(stderr, "vm_check: boot failed\n");
    return 1;
  }
  // A script: keys and taps across the screen, 50 ms apart, plus quiet time.
  struct Step {
    const char* key;
    int x, y;
  };
  std::vector<Step> script;
  const char* keys[] = {"ArrowRight", "ArrowRight", "Enter", "ArrowLeft", "Escape", "ArrowRight"};
  for (const char* k : keys) script.push_back({k, 0, 0});
  for (int y = h / 6; y < h; y += h / 5)
    for (int x = w / 6; x < w; x += w / 4) script.push_back({nullptr, x, y});
  for (const char* k : keys) script.push_back({k, 0, 0});
  int frames = 0, diffs = 0;
  auto compare = [&](const char* what) {
    uint32_t na = 0, nb = 0;
    const uint8_t* fa = native.frame(&na);
    const uint8_t* fb = interp.frame(&nb);
    ++frames;
    if (na != nb || std::memcmp(fa, fb, na) != 0) {
      if (diffs++ < 5) {
        int x0 = w, y0 = h, x1 = -1, y1 = -1, n = 0;
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x) {
            const size_t o = (static_cast<size_t>(y) * w + x) * 2;
            if (fa[o] != fb[o] || fa[o + 1] != fb[o + 1]) {
              ++n, x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
            }
          }
        std::fprintf(stderr, "vm_check: frame %d differs after %s: %d px in %d,%d-%d,%d\n", frames, what, n, x0, y0, x1, y1);
        if (const char* dump = std::getenv("VM_CHECK_DUMP")) {
          FILE* f = std::fopen((std::string(dump) + "-native.rgb565").c_str(), "wb");
          std::fwrite(fa, 1, na, f);
          std::fclose(f);
          f = std::fopen((std::string(dump) + "-vm.rgb565").c_str(), "wb");
          std::fwrite(fb, 1, nb, f);
          std::fclose(f);
        }
      }
    }
  };
  for (const Step& s : script) {
    for (CheckBoard* bd : {&native, &interp}) {
      if (s.key) {
        rt::Input in;
        in.k = rt::Input::K::Key;
        in.key = s.key;
        bd->push(in);
      } else {
        rt::Input in;
        in.k = rt::Input::K::Down, in.x = s.x, in.y = s.y;
        bd->push(in);
        in.k = rt::Input::K::Up;
        bd->push(in);
      }
    }
    for (int t = 0; t < 4; ++t) {
      native.tick(50000), interp.tick(50000);
      a.turn(), b.turn();
      compare(s.key ? s.key : "a tap");
    }
  }
  std::string sa = a.runtime().agent("{\"op\":\"state\"}"), sb = b.runtime().agent("{\"op\":\"state\"}");
  std::printf("%s: blob %zu bytes, %u bodies, %d frames, %d differ, state %s\n", mac()->name, blob.size(),
              loaded.body_count(), frames, diffs, sa == sb ? "same" : "DIFFERENT");
  return diffs == 0 && sa == sb ? 0 : 1;
}
