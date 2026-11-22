// rt_vm: Exact plan bytecode, interpreted, for running a plan that arrives at
// run time (a "plan blob") on the unchanged exact_rt core.
//
// plan2cpp (with PLAN2CPP_VM set) writes, beside each code body's C++
// function, the same body as an `Ins` table: the plan's own instructions with
// jump targets as instruction indexes, record sizes and stdlib calls resolved,
// and strings inline. `run` executes one with the semantics of plan2cpp's C++
// (code.rs `statement`), so a body behaves the same compiled or interpreted.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "exact_rt.h"

namespace rt_vm {

enum Op : uint8_t {
  kNumber = 1, kBool, kStr, kNone, kLoadSlot, kLoadDerive, kLoadResource, kLoadParam, kLoadLocal,
  kLoadItem, kField, kRecord, kList, kNot, kNeg, kSome, kAdd, kSub, kMul, kDiv, kRem, kEq, kNe, kLt,
  kLe, kGt, kGe, kConcat, kJump, kJumpIfFalse, kStoreSlot, kPop, kBindLocal, kDropLocal, kCall,
  kCommand, kSend, kNativeProps, kReturn,
};

// Call codes (Ins::a of kCall): 1-7 the pure functions, 50 performanceNow,
// 100 + n route function n (Ctx::call). Ins::b is the arity.
enum CallCode : uint32_t {
  kToFixed = 1, kMax, kMin, kFloor, kCeil, kRound, kToString, kNow = 50, kRouteBase = 100,
};

struct Ins {
  uint8_t op;
  uint32_t a, b, c;
  double n;       // kNumber
  const char* s;  // kStr, kCommand and kSend's name
};

// One body: the compiled function (on the host that built it), its code, and
// the value stack and locals it needs.
struct Body {
  rt::CodeFn fn;
  const Ins* code;
  uint16_t count;
  uint16_t max_stack;
  uint16_t max_locals;
};

// The length of each table rt::App points at (sizeof in the generated file;
// an empty table is one placeholder row).
struct Sizes {
  uint32_t slots, slot_names, derives, derive_names, resources, actions, action_names, bindings, nodes,
      handlers, regions, timers, commands, types, fields, slot_types, derive_types, slot_owners,
      action_params, args, routes, glyphs, glyph_bits, kerns, font_files, faces, stack_family, images,
      mutation_slots, extensions, extension_sources, tokens, sounds, sequences, ease_stops, eases,
      key_values, animations, animation_values;
};

// What a VM-capable app bundle exports (exact_vm_exports).
struct Exports {
  const rt::App* app;
  const Body* bodies;
  uint16_t body_count;
  const Sizes* sizes;
};

constexpr int kMaxStack = 32;
constexpr int kMaxLocals = 16;

// Runs `count` instructions of `code` in `c`; false in `ok` on a malformed one.
rt::V run(const Ins* code, uint32_t count, rt::Ctx& c);

}  // namespace rt_vm
