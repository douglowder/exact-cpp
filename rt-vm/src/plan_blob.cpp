// Plan blobs (plan_blob.h). One description of every rt::App table, `io`,
// drives both the writer (host) and the reader (board), so the two cannot
// disagree about a field.
#include "plan_blob.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <utility>

namespace plan_blob {

void* (*allocate)(size_t) = [](size_t n) { return std::malloc(n); };
void (*release)(void*) = [](void* p) { std::free(p); };

namespace {

constexpr uint32_t kNull = 0xffffffffu;
constexpr uint16_t kNoFn = 0xffff;

// ---- The bound bodies and their trampolines ----

const Loaded::Body* g_bodies = nullptr;
uint32_t g_body_count = 0;

template <int I>
rt::V tramp(rt::Ctx& c) {
  if (static_cast<uint32_t>(I) >= g_body_count) return rt::V();
  const Loaded::Body& b = g_bodies[I];
  if (b.constant) return b.value;
  return rt_vm::run(b.code, b.count, c);
}

template <int... I>
constexpr std::array<rt::CodeFn, sizeof...(I)> table(std::integer_sequence<int, I...>) {
  return {&tramp<I>...};
}

const std::array<rt::CodeFn, kMaxBodies> kTrampolines = table(std::make_integer_sequence<int, kMaxBodies>());

// ---- Writer ----

struct Writer {
  static constexpr bool kReading = false;
  std::vector<uint8_t>& out;
  const std::map<rt::CodeFn, uint16_t>& fns;
  std::vector<rt::V>& constants;  // fallbacks, called once and written as values
  uint16_t constant_base;
  std::string error;

  void raw(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
  }
  template <class T>
  void num(T& v) {
    T x = v;
    raw(&x, sizeof x);  // the host and the boards are little-endian
  }
  void u8(uint8_t& v) { num(v); }
  void u16(uint16_t& v) { num(v); }
  void i16(int16_t& v) { num(v); }
  void u32(uint32_t& v) { num(v); }
  void i32(int32_t& v) { num(v); }
  void f32(float& v) { num(v); }
  void f64(double& v) { num(v); }
  void b(bool& v) {
    uint8_t x = v ? 1 : 0;
    u8(x);
  }
  template <class E>
  void en(E& e) {
    uint8_t x = static_cast<uint8_t>(e);
    u8(x);
  }
  void pad() {
    while (out.size() % 8) out.push_back(0);
  }
  void str(const char*& s) {
    uint32_t n = s ? static_cast<uint32_t>(std::strlen(s)) : kNull;
    u32(n);
    if (s) {
      raw(s, n);
      out.push_back(0);
    }
  }
  void bytes(const void*& p, size_t n) {
    uint32_t len = p ? static_cast<uint32_t>(n) : kNull;
    u32(len);
    if (!p) return;
    pad();
    raw(p, n);
  }
  void fn(rt::CodeFn& f, bool fallback = false) {
    uint16_t id = kNoFn;
    if (f) {
      auto it = fns.find(f);
      if (it != fns.end()) {
        id = it->second;
      } else if (fallback) {
        rt::Ctx c{nullptr, nullptr, false, nullptr};
        constants.push_back(f(c));
        id = static_cast<uint16_t>(constant_base + constants.size() - 1);
      } else if (error.empty()) {
        error = "a code function that is not a plan body";
      }
    }
    u16(id);
  }
  template <class T, class F>
  void array(const T*& p, uint32_t n, F&& each) {
    u32(n);
    for (uint32_t i = 0; i < n; ++i) each(const_cast<T&>(p[i]));
  }
  void value(rt::V& v);
  void mark(const char*) {}
};

void Writer::value(rt::V& v) {
  uint8_t k = static_cast<uint8_t>(v.k);
  u8(k);
  switch (v.k) {
    case rt::V::K::Num: f64(v.n); break;
    case rt::V::K::Bool: b(v.b); break;
    case rt::V::K::Str: {
      const char* s = v.s.c_str();
      str(s);
      break;
    }
    case rt::V::K::Rec:
    case rt::V::K::List: {
      uint32_t n = static_cast<uint32_t>(v.size());
      u32(n);
      for (uint32_t i = 0; i < n; ++i) {
        rt::V item = (*v.rec)[i];
        value(item);
      }
      break;
    }
    default: break;
  }
}

}  // namespace

// ---- Reader ----

struct Reader {
  static constexpr bool kReading = true;
  Loaded& into;
  const uint8_t* base;
  size_t size;
  size_t at = 0;
  bool bad = false;
  const char* where = "header";
  void mark(const char* w) {
    if (!bad) where = w;
  }

  bool need(size_t n) {
    if (bad || at + n > size) {
      bad = true;
      return false;
    }
    return true;
  }
  template <class T>
  void num(T& v) {
    if (!need(sizeof v)) {
      v = T();
      return;
    }
    std::memcpy(&v, base + at, sizeof v);
    at += sizeof v;
  }
  void u8(uint8_t& v) { num(v); }
  void u16(uint16_t& v) { num(v); }
  void i16(int16_t& v) { num(v); }
  void u32(uint32_t& v) { num(v); }
  void i32(int32_t& v) { num(v); }
  void f32(float& v) { num(v); }
  void f64(double& v) { num(v); }
  void b(bool& v) {
    uint8_t x = 0;
    u8(x);
    v = x != 0;
  }
  template <class E>
  void en(E& e) {
    uint8_t x = 0;
    u8(x);
    e = static_cast<E>(x);
  }
  void pad() { at = (at + 7) & ~size_t(7); }
  void str(const char*& s) {
    uint32_t n = 0;
    u32(n);
    if (n == kNull) {
      s = nullptr;
      return;
    }
    if (!need(size_t(n) + 1) || base[at + n] != 0) {
      bad = true;
      s = "";
      return;
    }
    s = reinterpret_cast<const char*>(base + at);
    at += n + 1;
  }
  template <class P>
  void bytes(P*& p, size_t n) {
    uint32_t len = 0;
    u32(len);
    if (len == kNull) {
      p = nullptr;
      return;
    }
    pad();
    if ((n != size_t(-1) && len != n) || !need(len)) {
      bad = true;
      p = nullptr;
      return;
    }
    p = reinterpret_cast<P*>(base + at);
    at += len;
  }
  void fn(rt::CodeFn& f, bool = false) {
    uint16_t id = kNoFn;
    u16(id);
    if (id == kNoFn) {
      f = nullptr;
    } else if (id >= kMaxBodies) {
      bad = true;
      f = nullptr;
    } else {
      f = kTrampolines[id];
    }
  }
  template <class T, class F>
  void array(const T*& p, uint32_t, F&& each) {
    uint32_t n = 0;
    u32(n);
    if (n > size) {  // a count no blob of this size could hold
      bad = true;
      p = nullptr;
      return;
    }
    T* a = static_cast<T*>(into.alloc(sizeof(T) * (n ? n : 1)));
    for (uint32_t i = 0; i < (n ? n : 1); ++i) new (&a[i]) T();
    for (uint32_t i = 0; i < n && !bad; ++i) each(a[i]);
    p = a;
  }
  void value(rt::V& v) {
    uint8_t k = 0;
    u8(k);
    switch (static_cast<rt::V::K>(k)) {
      case rt::V::K::Num: {
        double n = 0;
        f64(n);
        v = rt::V::num(n);
        break;
      }
      case rt::V::K::Bool: {
        bool x = false;
        b(x);
        v = rt::V::boolean(x);
        break;
      }
      case rt::V::K::Str: {
        const char* s = nullptr;
        str(s);
        v = rt::V::str(s ? s : "");
        break;
      }
      case rt::V::K::Rec:
      case rt::V::K::List: {
        uint32_t n = 0;
        u32(n);
        if (n > size) {
          bad = true;
          return;
        }
        std::vector<rt::V> items(n);
        for (auto& item : items) value(item);
        v = static_cast<rt::V::K>(k) == rt::V::K::Rec ? rt::V::record(std::move(items)) : rt::V::list(std::move(items));
        break;
      }
      default: v = rt::V(); break;
    }
  }
};

namespace {

// Every table of the app, for the writer and the reader alike.
template <class A>
void io(A& ar, rt::App& app, rt_vm::Sizes& z) {
  using namespace rt;
  auto fns = [&](const CodeFn*& p, uint32_t n) { ar.array(p, n, [&](CodeFn& f) { ar.fn(f); }); };
  auto strs = [&](const char* const*& p, uint32_t n) { ar.array(p, n, [&](const char*& s) { ar.str(s); }); };
  auto u16s = [&](const uint16_t*& p, uint32_t n) { ar.array(p, n, [&](uint16_t& v) { ar.u16(v); }); };
  auto i16s = [&](const int16_t*& p, uint32_t n) { ar.array(p, n, [&](int16_t& v) { ar.i16(v); }); };

  ar.u16(app.slot_count);
  ar.u16(app.derive_count);
  ar.u16(app.resource_count);
  ar.u16(app.action_count);
  ar.u16(app.node_count);
  ar.u16(app.region_count);
  ar.u16(app.timer_count);
  ar.b(app.keys);
  ar.b(app.touch);
  ar.u16(app.command_count);
  ar.u16(app.route_count);
  ar.i16(app.router_slot);
  ar.u16(app.face_count);
  ar.u16(app.stack_count);
  ar.u16(app.image_count);
  ar.u16(app.mutation_count);
  ar.u16(app.extension_count);
  ar.u16(app.extension_source_count);
  ar.u16(app.token_count);
  ar.u16(app.sound_count);
  ar.u16(app.sequence_count);
  ar.u16(app.animation_value_count);

  ar.mark("slots");
  fns(app.slots, z.slots);
  ar.mark("slot_names");
  strs(app.slot_names, z.slot_names);
  ar.mark("derives");
  fns(app.derives, z.derives);
  ar.mark("derive_names");
  strs(app.derive_names, z.derive_names);
  ar.mark("resources");
  ar.array(app.resources, z.resources, [&](Resource& r) {
    ar.str(r.name);
    ar.str(r.source);
    ar.u16(r.ty);
    ar.u16(r.arg_start);
    ar.u16(r.arg_count);
    ar.i16(r.placeholder);
    ar.fn(r.fallback, true);
  });
  ar.mark("actions");
  fns(app.actions, z.actions);
  ar.mark("action_names");
  strs(app.action_names, z.action_names);
  ar.mark("bindings");
  ar.array(app.bindings, z.bindings, [&](Binding& b) {
    ar.b(b.target.prop);
    ar.u8(b.target.id);
    ar.fn(b.fn);
    ar.u8(b.lit.k);
    ar.f64(b.lit.n);
    ar.b(b.lit.b);
    ar.str(b.lit.s);
  });
  ar.mark("nodes");
  ar.array(app.nodes, z.nodes, [&](Node& n) {
    ar.en(n.type);
    ar.i16(n.parent);
    ar.i16(n.arm);
    ar.u32(n.order);
    ar.u16(n.binding_start);
    ar.u16(n.binding_count);
    ar.u16(n.handler_start);
    ar.u16(n.handler_count);
  });
  ar.mark("handlers");
  ar.array(app.handlers, z.handlers, [&](Handler& h) {
    ar.en(h.event);
    ar.u16(h.action);
    ar.u16(h.arg_start);
    ar.u16(h.arg_count);
  });
  ar.mark("regions");
  ar.array(app.regions, z.regions, [&](Region& r) {
    ar.en(r.kind);
    ar.i16(r.parent);
    ar.i16(r.arm);
    ar.u32(r.order);
    ar.fn(r.subject);
    ar.fn(r.key);
    ar.u16(r.arm_start);
    ar.u16(r.arm_count);
  });
  ar.mark("timers");
  ar.array(app.timers, z.timers, [&](Timer& t) {
    ar.u32(t.interval_ms);
    ar.u16(t.action);
    ar.b(t.once);
  });
  ar.mark("commands");
  strs(app.commands, z.commands);
  ar.mark("types");
  ar.array(app.types, z.types, [&](Type& t) {
    ar.en(t.k);
    ar.i16(t.elem);
    ar.u16(t.field_start);
    ar.u16(t.field_count);
  });
  ar.mark("fields");
  ar.array(app.fields, z.fields, [&](Field& f) {
    ar.str(f.name);
    ar.u16(f.ty);
  });
  ar.mark("slot_types");
  u16s(app.slot_types, z.slot_types);
  ar.mark("derive_types");
  u16s(app.derive_types, z.derive_types);
  ar.mark("slot_owners");
  i16s(app.slot_owners, z.slot_owners);
  ar.mark("action_params");
  ar.array(app.action_params, z.action_params, [&](uint8_t& v) { ar.u8(v); });
  ar.mark("args");
  fns(app.args, z.args);
  ar.mark("routes");
  ar.array(app.routes, z.routes, [&](RouteRow& r) {
    ar.str(r.name);
    ar.str(r.pattern);
    ar.i16(r.parent);
    ar.b(r.tab);
    ar.b(r.notfound);
  });
  ar.mark("glyphs");
  ar.array(app.glyphs, z.glyphs, [&](Glyph& g) {
    ar.u16(g.code);
    ar.i16(g.left);
    ar.i16(g.top);
    ar.u8(g.w);
    ar.u8(g.h);
    ar.i32(g.advance);
    ar.u32(g.bits);
  });
  {
    const uint8_t* bits = app.glyph_bits;
    if constexpr (A::kReading) {
      ar.bytes(bits, size_t(-1));  // the writer's length
    } else {
      const void* p = bits;
      ar.bytes(p, z.glyph_bits);
    }
    app.glyph_bits = bits;
  }
  ar.mark("kerns");
  ar.array(app.kerns, z.kerns, [&](Kern& k) {
    ar.u16(k.left);
    ar.u16(k.right);
    ar.i16(k.units);
  });
  ar.mark("font_files");
  ar.array(app.font_files, z.font_files, [&](FontFile& f) {
    ar.u32(f.kern_start);
    ar.u32(f.kern_count);
    ar.u16(f.upem);
  });
  ar.mark("faces");
  ar.array(app.faces, z.faces, [&](Face& f) {
    ar.i16(f.family);
    ar.u16(f.weight);
    ar.f32(f.px);
    ar.i32(f.ascent);
    ar.i32(f.descent);
    ar.i32(f.leading);
    ar.u32(f.glyph_start);
    ar.u16(f.glyph_count);
    ar.u16(f.file);
  });
  ar.mark("stack_family");
  i16s(app.stack_family, z.stack_family);
  ar.mark("images");
  ar.array(app.images, z.images, [&](Image& im) {
    ar.str(im.source);
    ar.u16(im.w);
    ar.u16(im.h);
    ar.u8(im.format);
    ar.u32(im.color);
    const size_t px = size_t(im.w) * im.h;
    if constexpr (A::kReading) {
      ar.bytes(im.rgb, px * 2);
      ar.bytes(im.alpha, px);
    } else {
      const void* rgb = im.rgb;
      const void* alpha = im.alpha;
      ar.bytes(rgb, px * 2);
      ar.bytes(alpha, px);
    }
  });
  ar.mark("mutation_slots");
  u16s(app.mutation_slots, z.mutation_slots);
  ar.mark("extensions");
  ar.array(app.extensions, z.extensions, [&](ExtensionUse& e) {
    ar.str(e.name);
    ar.u16(e.version);
    ar.str(e.digest);
  });
  ar.mark("extension_sources");
  ar.array(app.extension_sources, z.extension_sources, [&](ExtensionSource& e) {
    ar.str(e.source);
    ar.u16(e.extension);
    ar.u16(e.ty);
  });
  ar.mark("tokens");
  ar.array(app.tokens, z.tokens, [&](Token& t) {
    ar.str(t.name);
    ar.u32(t.argb);
  });
  ar.mark("sounds");
  ar.array(app.sounds, z.sounds, [&](Sound& s) {
    ar.str(s.src);
    ar.u32(s.frames);
    ar.u32(s.rate);
    if constexpr (A::kReading) {
      ar.bytes(s.samples, size_t(s.frames) * 2);
    } else {
      const void* p = s.samples;
      ar.bytes(p, size_t(s.frames) * 2);
    }
  });
  ar.mark("sequences");
  ar.array(app.sequences, z.sequences, [&](Sequence& q) {
    ar.str(q.name);
    ar.fn(q.gate);
    ar.fn(q.tracks);
    ar.fn(q.bpm);
  });
  ar.mark("ease_stops");
  ar.array(app.ease_stops, z.ease_stops, [&](double& d) { ar.f64(d); });
  ar.mark("eases");
  ar.array(app.eases, z.eases, [&](Ease& e) {
    ar.u8(e.kind);
    ar.f64(e.a);
    ar.f64(e.b);
    ar.f64(e.c);
    ar.f64(e.d);
    ar.u16(e.stop_start);
    ar.u16(e.stop_count);
  });
  ar.mark("key_values");
  ar.array(app.key_values, z.key_values, [&](KeyValue& k) {
    ar.f64(k.offset);
    ar.i16(k.ease);
    ar.en(k.prop);
    for (double& v : k.v) ar.f64(v);
  });
  ar.mark("animations");
  ar.array(app.animations, z.animations, [&](Animation& a) {
    ar.str(a.name);
    ar.f64(a.duration_ms);
    ar.f64(a.delay_ms);
    ar.f64(a.iterations);
    ar.u8(a.direction);
    ar.u8(a.fill);
    ar.u16(a.ease);
    ar.b(a.paused);
    ar.u16(a.value_start);
    ar.u16(a.value_count);
  });
  ar.mark("animation_values");
  ar.array(app.animation_values, z.animation_values, [&](AnimationValue& a) {
    ar.str(a.text);
    ar.u16(a.start);
    ar.u16(a.count);
  });
}

}  // namespace

bool write_blob(const rt_vm::Exports& e, std::vector<uint8_t>& out, std::string& error) {
  std::map<rt::CodeFn, uint16_t> fns;
  for (uint16_t i = 0; i < e.body_count; ++i) {
    if (e.bodies[i].fn) fns.emplace(e.bodies[i].fn, i);
    if (e.bodies[i].max_stack > rt_vm::kMaxStack || e.bodies[i].max_locals > rt_vm::kMaxLocals) {
      error = "a body needs a deeper stack than the interpreter's";
      return false;
    }
  }
  // The tables first (they find the fallbacks), then the header and bodies.
  std::vector<uint8_t> tables;
  std::vector<rt::V> constants;
  Writer tw{tables, fns, constants, e.body_count, {}};
  rt::App app = *e.app;
  rt_vm::Sizes z = *e.sizes;
  io(tw, app, z);
  if (!tw.error.empty()) {
    error = tw.error;
    return false;
  }
  if (e.body_count + constants.size() > kMaxBodies) {
    error = "more bodies than the board can bind";
    return false;
  }
  out.clear();
  Writer w{out, fns, constants, e.body_count, {}};
  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint32_t bodies = e.body_count, consts = static_cast<uint32_t>(constants.size());
  w.u32(magic);
  w.u16(version);
  w.u32(bodies);
  w.u32(consts);
  for (uint16_t i = 0; i < e.body_count; ++i) {
    const rt_vm::Body& b = e.bodies[i];
    uint32_t n = b.code ? b.count : 0;
    w.u32(n);
    for (uint32_t k = 0; k < n; ++k) {
      rt_vm::Ins ins = b.code[k];
      w.u8(ins.op);
      w.u32(ins.a);
      w.u32(ins.b);
      w.u32(ins.c);
      w.f64(ins.n);
      w.str(ins.s);
    }
  }
  for (rt::V& v : constants) w.value(v);
  w.pad();  // the tables pad relative to their own start: keep it 8-aligned
  out.insert(out.end(), tables.begin(), tables.end());
  return true;
}

Loaded::~Loaded() {
  if (g_bodies == bodies_.data()) {
    g_bodies = nullptr;
    g_body_count = 0;
  }
  for (void* p : blocks_) release(p);
}

void* Loaded::alloc(size_t bytes) {
  void* p = allocate(bytes);
  if (p) blocks_.push_back(p);
  return p;
}

bool Loaded::load(const uint8_t* blob, size_t size, std::string& error) {
  Reader r{*this, blob, size};
  uint32_t magic = 0, bodies = 0, consts = 0;
  uint16_t version = 0;
  r.u32(magic);
  r.u16(version);
  r.u32(bodies);
  r.u32(consts);
  if (r.bad || magic != kMagic || version != kVersion) {
    error = "not a plan blob of this version";
    return false;
  }
  if (bodies + consts > static_cast<uint32_t>(kMaxBodies)) {
    error = "more bodies than this board binds";
    return false;
  }
  bodies_.assign(bodies + consts, Body{});
  for (uint32_t i = 0; i < bodies && !r.bad; ++i) {
    uint32_t n = 0;
    r.u32(n);
    if (n > size) {
      r.bad = true;
      break;
    }
    auto* code = static_cast<rt_vm::Ins*>(alloc(sizeof(rt_vm::Ins) * (n ? n : 1)));
    for (uint32_t k = 0; k < n; ++k) {
      rt_vm::Ins& ins = code[k];
      r.u8(ins.op);
      r.u32(ins.a);
      r.u32(ins.b);
      r.u32(ins.c);
      r.f64(ins.n);
      r.str(ins.s);
      if ((ins.op == rt_vm::kJump || ins.op == rt_vm::kJumpIfFalse) && ins.a > n) r.bad = true;
    }
    bodies_[i].code = code;
    bodies_[i].count = n;
  }
  for (uint32_t i = 0; i < consts && !r.bad; ++i) {
    bodies_[bodies + i].constant = true;
    r.value(bodies_[bodies + i].value);
  }
  rt_vm::Sizes z{};
  app_ = rt::App{};
  r.pad();
  io(r, app_, z);
  if (r.bad) {
    error = std::string("a malformed plan blob (in ") + r.where + ", at byte " + std::to_string(r.at) + ")";
    return false;
  }
  g_bodies = bodies_.data();
  g_body_count = static_cast<uint32_t>(bodies_.size());
  return true;
}

}  // namespace plan_blob
