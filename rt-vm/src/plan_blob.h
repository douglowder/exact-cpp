// Plan blobs: an Exact app (its rt::App tables and its code bodies as rt_vm
// bytecode) as one byte string a board can receive and run with no compiler.
//
// The host writes one from a VM-capable app bundle (`write_blob`); the board
// reads it into memory it allocates (`Loaded`), and each CodeFn in the tables
// becomes a trampoline into the interpreter. Little-endian; every pointer is
// written as its contents, so a 64-bit host and a 32-bit board agree.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "exact_rt.h"
#include "rt_vm.h"

namespace plan_blob {

constexpr uint32_t kMagic = 0x4e4c5058;  // "XPLN"
constexpr uint16_t kVersion = 1;
// The most code bodies (and constant bodies) a board can bind.
constexpr int kMaxBodies = 1024;

// Writes `e`'s app as a blob. False with `error` if it cannot (a CodeFn that
// is neither a body nor a resource fallback, or too many bodies).
bool write_blob(const rt_vm::Exports& e, std::vector<uint8_t>& out, std::string& error);

// A blob read back: the app and everything it points at. The blob's bytes
// must outlive it (strings and pixels point into them).
class Loaded {
 public:
  ~Loaded();
  // Reads `blob` (`size` bytes); false with `error` if it is malformed.
  // Only one Loaded may be bound at a time (the trampolines are global).
  bool load(const uint8_t* blob, size_t size, std::string& error);
  const rt::App& app() const { return app_; }
  uint32_t body_count() const { return static_cast<uint32_t>(bodies_.size()); }

  struct Body {
    const rt_vm::Ins* code = nullptr;
    uint32_t count = 0;
    bool constant = false;
    rt::V value;  // a constant body's answer
  };

 private:
  friend struct Reader;
  void* alloc(size_t bytes);
  rt::App app_{};
  std::vector<Body> bodies_;
  std::vector<void*> blocks_;
};

// The allocator Loaded uses (PSRAM on a board); malloc by default.
extern void* (*allocate)(size_t bytes);
extern void (*release)(void* p);

}  // namespace plan_blob
