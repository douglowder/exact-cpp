// Runtime: commits, derives, resources, timers, keys, presses, focus, routes,
// and the agent's JSON. A commit runs the action body with every read at the
// starting state, holds its writes and commands, applies the writes
// together, settles, then runs the commands (preventDefault takes effect at
// once), as the runner. A trap, or a write that leaves the routes slot
// holding no valid router, rolls the whole commit back. Commands it does not
// handle itself go to the board. Without a display the view keeps instance
// state only, so keys, taps by testId, routes, and the agent still work.
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>

#include "contract_rt.h"
#include "rt_transport.hpp"
#include "rt_view.h"

namespace rt {

namespace {

constexpr size_t kLogLines = 64;

// The arm instance holding `arm`'s slots, from `scope` outward.
Scope* owner(Scope* scope, int arm) {
  while (scope && scope->arm != arm) scope = scope->parent;
  return scope;
}

}  // namespace

V Ctx::slot(uint64_t i) { return r->slot(i, scope); }
V Ctx::derive(uint64_t i) { return r->derive(i); }
V Ctx::resource(uint64_t i) { return r->resource(i); }
V Ctx::param(uint64_t i) { return params ? params[i] : V(); }
V Ctx::item(uint64_t depth) {
  const Scope* s = scope;
  for (uint64_t k = 0; k < depth && s; ++k) s = s->parent;
  return s ? s->item : V();
}
void Ctx::write(uint64_t i, const V& v) {
  if (in_action) r->write(i, v, scope);
}
void Ctx::command(const char* name, const V* args, int argc) {
  if (in_action) r->command(name, args, argc);
}
V Ctx::call(uint8_t fn, const V* args, int argc) { return r->call(fn, args, argc); }
V Ctx::now() { return V::num(r->elapsed_ms()); }
void Ctx::send(uint16_t m, const char* source, const V* args, int argc) { r->send(m, source, args, argc); }

V record(const App& app, uint16_t ty, const std::vector<std::pair<const char*, V>>& fields) {
  const Type& t = app.types[ty];
  std::vector<V> out(t.field_count);
  for (uint16_t k = 0; k < t.field_count; ++k) {
    for (const auto& f : fields) {
      if (std::strcmp(app.fields[t.field_start + k].name, f.first) == 0) out[k] = f.second;
    }
  }
  return V::record(std::move(out));
}

int source_type(const App& app, const char* source) {
  for (uint16_t i = 0; i < app.extension_source_count; ++i) {
    if (std::strcmp(app.extension_sources[i].source, source) == 0) return app.extension_sources[i].ty;
  }
  return -1;
}

Runtime::Runtime(const App& app, Board& board, std::vector<Extension*> extensions)
    : app_(app), board_(board), bound_(app.extension_count, nullptr) {
  for (uint16_t i = 0; i < app_.extension_count; ++i) {
    for (Extension* e : extensions) {
      if (e && std::strcmp(e->name(), app_.extensions[i].name) == 0 && e->version() == app_.extensions[i].version) {
        bound_[i] = e;
      }
    }
  }
  if (app_.route_count > 0) {
    std::vector<route::Route> rows;
    for (uint16_t i = 0; i < app_.route_count; ++i) {
      const RouteRow& r = app_.routes[i];
      rows.push_back(route::Route{r.name, r.pattern, r.parent, r.tab, r.notfound});
    }
    table_.reset(new route::Table(std::move(rows)));
  }
  transport_.reset(new Transport(*this));
}

Runtime::~Runtime() = default;

bool Runtime::advance(double ms) {
  const double target = now_ms() + ms;
  // Each step lands on the next due time, rounded up to the clock's microsecond so the due
  // work runs; a step that finds nothing due ends the walk at `target`.
  for (int guard = 0; guard < 100000; ++guard) {
    double next = transport_->due();
    for (uint16_t i = 0; i < app_.timer_count; ++i) {
      if (timer_due_[i] != 0) next = std::min(next, static_cast<double>(timer_due_[i]) - started_us_ / 1000.0);
    }
    const bool last = next > target;
    const int64_t step_us = static_cast<int64_t>(std::ceil(((last ? target : next) - now_ms()) * 1000.0));
    if (step_us > 0 && !board_.advance_us(step_us)) return false;
    tick();
    if (last) return true;
  }
  log("clock: due work kept coming; stopped");
  return true;
}

V Runtime::slot(uint64_t i, Scope* scope) const {
  const int arm = app_.slot_owners ? app_.slot_owners[i] : -1;
  if (arm < 0) return slots_[i];
  if (Scope* s = owner(scope, arm)) {
    for (const auto& kv : s->slots) {
      if (kv.first == i) return kv.second;
    }
  }
  return V();
}

void Runtime::own(Scope* scope) {
  for (uint16_t i = 0; i < app_.slot_count; ++i) {
    if (app_.slot_owners && app_.slot_owners[i] == scope->arm) {
      Ctx c{this, nullptr, false, scope};
      scope->slots.push_back({i, app_.slots[i](c)});
    }
  }
}

void Runtime::log(const std::string& line) {
  logs_.push_back(line);
  if (logs_.size() > kLogLines) logs_.erase(logs_.begin());
}

void Runtime::trap(const std::string& why) {
  if (!trapped_) log("trap: " + why);
  trapped_ = true;
}

bool Runtime::boot(const std::string& launch) {
  const uint32_t now_ms = board_.millis();
  view_.reset(new View(*this, board_.display()));
  Ctx c{this, nullptr, false, &view_->root_scope()};
  started_us_ = board_.micros();
  resources_.assign(app_.resource_count, V());
  for (uint16_t i = 0; i < app_.resource_count; ++i) resources_[i] = answer(i);
  slots_.assign(app_.slot_count, V());
  derives_.assign(app_.derive_count, V());
  derive_valid_.assign(app_.derive_count, false);
  if (app_.router_slot >= 0 && table_) {
    route::Result r = route::launch(*table_, launch);
    if (r.refusal) {
      log("router launch refused: " + *r.refusal);
      r = route::launch(*table_, "/");
      if (r.refusal) return false;
    }
    slots_[app_.router_slot] = router_value(r.router);
  }
  for (uint16_t i = 0; i < app_.slot_count; ++i) {
    if (i == app_.router_slot || (app_.slot_owners && app_.slot_owners[i] >= 0)) continue;
    slots_[i] = app_.slots[i](c);
  }
  timer_due_.assign(app_.timer_count, 0);
  for (uint16_t i = 0; i < app_.timer_count; ++i) timer_due_[i] = now_ms + app_.timers[i].interval_ms;
  if (!view_->begin()) return false;
  view_->update();
  refocus();
  return true;
}

V Runtime::derive(uint64_t i) {
  if (!derive_valid_[i]) {
    Ctx c{this, nullptr, false, &view_->root_scope()};
    derives_[i] = app_.derives[i](c);
    derive_valid_[i] = true;
  }
  return derives_[i];
}

// The answer of an extension source, from the extension bound to it; none
// when no extension is bound or it has no answer.
V Runtime::ask(const char* source, const V* args, int argc) {
  for (uint16_t i = 0; i < app_.extension_source_count; ++i) {
    const ExtensionSource& es = app_.extension_sources[i];
    if (std::strcmp(es.source, source) != 0) continue;
    Extension* e = bound_[es.extension];
    V out;
    return e && e->answer(app_, source, args, argc, out) ? out : V();
  }
  return V();
}

// A resource's answer: `exactTime` from the board's wall clock (an epoch of 0
// when it has none, as the app's check for a set clock expects), to the
// second, as a board's clock chip counts; any other source from its
// extension.
V Runtime::answer(uint16_t i) {
  const Resource& r = app_.resources[i];
  if (std::strcmp(r.source, "exactTime") != 0) return ask(r.source, nullptr, 0);
  int64_t epoch = 0;
  int offset = 0;
  if (!board_.wall_clock(epoch, offset)) epoch = 0;
  const double at_zero = epoch ? std::round((static_cast<double>(epoch) - elapsed_ms()) / 1000) * 1000 : 0;
  return record(app_, r.ty, {{"epochAtZero", V::num(at_zero)}, {"utcOffset", V::num(offset)}});
}

void Runtime::send(uint16_t m, const char* source, const V* args, int argc) {
  sends_.push_back({m, source, std::vector<V>(args, args + argc)});
}

void Runtime::put_image(const std::string& source, const Image* image) {
  for (auto it = images_.begin(); it != images_.end(); ++it) {
    if (it->first == source) {
      images_.erase(it);
      break;
    }
  }
  if (image) images_.push_back({source, image});
  if (view_) settle();
}

const Image* Runtime::image(const std::string& source) const {
  for (const auto& kv : images_) {
    if (kv.first == source) return kv.second;
  }
  return nullptr;
}

void Runtime::command(const char* name, const V* args, int argc) {
  if (std::strcmp(name, "preventDefault") == 0) {
    prevented_ = true;
    return;
  }
  commands_.push_back({name, std::vector<V>(args, args + argc)});
}

void Runtime::settle() {
  derive_valid_.assign(app_.derive_count, false);
  view_->update();
  refocus();
  ++stats.commits;
  transport_->stood(now_ms());
}

void Runtime::run(uint16_t action, const V* params, Scope* scope) {
  writes_.clear();
  commands_.clear();
  sends_.clear();
  refusals_.clear();
  trapped_ = false;
  Ctx c{this, params, true, scope};
  app_.actions[action](c);
  bool rolled_back = trapped_;
  for (const Write& w : writes_) {
    route::Router r;
    if (!rolled_back && static_cast<int>(w.slot) == app_.router_slot && !router_of(w.v, r)) {
      log(std::string("action ") + app_.action_names[action] + ": an invalid router value");
      rolled_back = true;
    }
  }
  if (rolled_back) {
    writes_.clear();
    commands_.clear();
    sends_.clear();
    return;
  }
  for (const Write& w : writes_) {
    const int arm = app_.slot_owners ? app_.slot_owners[w.slot] : -1;
    if (arm < 0) {
      slots_[w.slot] = w.v;
      continue;
    }
    if (Scope* s = owner(w.scope, arm)) {
      for (auto& kv : s->slots) {
        if (kv.first == w.slot) kv.second = w.v;
      }
    }
  }
  writes_.clear();
  settle();
  auto commands = std::move(commands_);
  commands_.clear();
  for (auto& cmd : commands) board_.command(cmd.first, cmd.second);
  // Each send goes to its extension after the commit, and its answer
  // lands in the mutation's slot as a commit of its own.
  auto sends = std::move(sends_);
  sends_.clear();
  for (const Send& sd : sends) {
    slots_[app_.mutation_slots[sd.mutation]] =
        ask(sd.source.c_str(), sd.args.data(), static_cast<int>(sd.args.size()));
  }
  if (!sends.empty()) settle();
}

void Runtime::refresh(uint16_t resource) {
  V next = answer(resource);
  if (equal(next, resources_[resource])) return;
  resources_[resource] = next;
  settle();
}

void Runtime::changed(const char* source) {
  for (uint16_t i = 0; i < app_.resource_count; ++i) {
    if (std::strcmp(app_.resources[i].source, source) == 0) refresh(i);
  }
}

void Runtime::sense() {
  for (Extension* e : bound_) {
    if (e) e->sense(*this);
  }
}

void Runtime::tick() {
  transport_->wake(this->now_ms());
  const uint32_t now_ms = board_.millis();
  for (uint16_t i = 0; i < app_.timer_count; ++i) {
    if (timer_due_[i] == 0 || static_cast<int32_t>(now_ms - timer_due_[i]) < 0) continue;
    run(app_.timers[i].action, nullptr, &view_->root_scope());
    timer_due_[i] = app_.timers[i].once ? 0 : now_ms + app_.timers[i].interval_ms;
  }
  int64_t paint_us = 0, push_us = 0;
  int64_t area = view_->present(paint_us, push_us);
  if (area > 0) {
    ++stats.frames;
    stats.paint_us = paint_us;
    stats.push_us = push_us;
    stats.damage_px = area;
    if (input_at_us_ >= 0) {
      stats.input_to_panel_us = board_.micros() - input_at_us_;
      input_at_us_ = -1;
    }
  }
}

int Runtime::handler(int instance, E event, int* at) const {
  for (int i = instance; i >= 0; i = view_->node(i).parent) {
    const Node& n = app_.nodes[view_->node(i).node];
    for (int k = 0; k < n.handler_count; ++k) {
      if (app_.handlers[n.handler_start + k].event == event) {
        *at = i;
        return n.handler_start + k;
      }
    }
  }
  *at = -1;
  return -1;
}

namespace {

// A handler's parameters: its bound arguments in `scope`, then as many of
// the event's payload values (a key, a location, a pan's two numbers) as
// the action takes.
std::vector<V> params_for(Runtime& rt, const App& app, const Handler& h, Scope* scope, const V* payload,
                          int count = 1) {
  std::vector<V> out;
  Ctx c{&rt, nullptr, false, scope};
  for (uint16_t k = 0; k < h.arg_count; ++k) out.push_back(app.args[h.arg_start + k](c));
  for (int k = 0; payload && k < count && app.action_params && app.action_params[h.action] > out.size(); ++k) {
    out.push_back(payload[k]);
  }
  return out;
}

bool inside(const Rect& r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

// The slop a contact may move and still press (LVGL's default scroll limit,
// as the badge firmware's taps use).
constexpr int kSlop = 10;

}  // namespace

void Runtime::press(int instance) {
  if (instance < 0 || view_->node(instance).inactive) return;
  int at = -1;
  const int h = handler(instance, E::Press, &at);
  if (h < 0) return;
  if (input_at_us_ < 0) input_at_us_ = board_.micros();
  Scope* scope = view_->node(at).scope;
  std::vector<V> params = params_for(*this, app_, app_.handlers[h], scope, nullptr);
  run(app_.handlers[h].action, params.data(), scope);
}

void Runtime::key(const char* key) {
  input_at_us_ = board_.micros();
  prevented_ = false;
  int at = -1;
  const int h = focus_ >= 0 ? handler(focus_, E::Key, &at) : -1;
  if (h >= 0) {
    const V payload = V::str(key);
    Scope* scope = view_->node(at).scope;
    std::vector<V> params = params_for(*this, app_, app_.handlers[h], scope, &payload);
    run(app_.handlers[h].action, params.data(), scope);
  }
  if (prevented_) return;
  if (std::strcmp(key, "Enter") == 0 || std::strcmp(key, " ") == 0) {
    if (focus_ >= 0) press(focus_);
  } else if (std::strcmp(key, "ArrowRight") == 0 || std::strcmp(key, "ArrowDown") == 0) {
    step_focus(1);
  } else if (std::strcmp(key, "ArrowLeft") == 0 || std::strcmp(key, "ArrowUp") == 0) {
    step_focus(-1);
  } else if (std::strcmp(key, "Escape") == 0) {
    press_back();
  }
}

// The hardware Back rule (LLP 1008 §9, host/web/navigation.js pressBack):
// while the selected stack can pop, press the selected route's control whose
// id is the root's navigationBack, unless that route is closedby="none".
bool Runtime::press_back() {
  const int root = view_->nav_root();
  if (root < 0) return false;
  int selected = -1;
  const std::vector<int> stack = view_->selected_stack(root, &selected);
  if (selected <= 0) return false;
  if (view_->node(stack[selected]).closedby == "none") return false;
  const std::string& back = view_->node(root).nav_back;
  std::vector<int> todo{stack[selected]};
  while (!todo.empty()) {
    const int i = todo.back();
    todo.pop_back();
    const NodeState& s = view_->node(i);
    if (s.box.hidden) continue;
    if (s.id == back && app_.nodes[s.node].type == T::Pressable && !s.inactive) {
      press(i);
      return true;
    }
    todo.insert(todo.end(), s.kids.rbegin(), s.kids.rend());
  }
  return false;
}

bool Runtime::navigate(const std::string& location) {
  const int root = view_->nav_root();
  if (root < 0) return false;
  const Node& n = app_.nodes[view_->node(root).node];
  for (int k = 0; k < n.handler_count; ++k) {
    const Handler& h = app_.handlers[n.handler_start + k];
    if (h.event != E::Navigate) continue;
    input_at_us_ = board_.micros();
    const V payload = V::str(location);
    Scope* scope = view_->node(root).scope;
    std::vector<V> params = params_for(*this, app_, h, scope, &payload);
    run(h.action, params.data(), scope);
    return true;
  }
  return false;
}

bool Runtime::focusable(int i) const {
  if (i < 0 || i >= view_->count()) return false;
  const NodeState& s = view_->node(i);
  if (s.inactive || s.box.hidden) return false;
  const Node& n = app_.nodes[s.node];
  bool key = false;
  for (int k = 0; k < n.handler_count; ++k) key |= app_.handlers[n.handler_start + k].event == E::Key;
  return n.type == T::Pressable || key || s.autofocus;
}

// After each update: a newly shown autofocus instance takes the focus (as
// autofocus runs once per mount); else the focused instance keeps it while
// it can; else the last autofocus instance, or the first focusable one.
void Runtime::refocus() {
  std::vector<std::string> now;
  int fresh = -1, any = -1;
  for (int i = 0; i < view_->count(); ++i) {
    const NodeState& s = view_->node(i);
    now.push_back(s.key);
    if (!s.autofocus || !focusable(i)) continue;
    any = i;
    if (std::find(shown_.begin(), shown_.end(), s.key) == shown_.end()) fresh = i;
  }
  shown_ = std::move(now);
  int focus = view_->find(focus_key_);
  if (fresh >= 0) focus = fresh;
  else if (!focusable(focus)) focus = any;
  focus_ = focus;
  if (focus_ < 0) step_focus(1);
  focus_key_ = focus_ >= 0 ? view_->node(focus_).key : "";
}

void Runtime::step_focus(int by) {
  std::vector<int> all;
  for (int i = 0; i < view_->count(); ++i) {
    if (focusable(i)) all.push_back(i);
  }
  if (all.empty()) {
    focus_ = -1;
    return;
  }
  int at = 0;
  for (size_t k = 0; k < all.size(); ++k) {
    if (all[k] == focus_) at = static_cast<int>(k);
  }
  int n = static_cast<int>(all.size());
  focus_ = all[((at + by) % n + n) % n];
  focus_key_ = view_->node(focus_).key;
}

// The agent's tap: the instance with `test_id`, an active one first (as the
// runner's agent prefers), pressed unless it is under an unselected route.
bool Runtime::tap(const char* test_id) {
  int match = -1;
  for (int i = 0; i < view_->count(); ++i) {
    const NodeState& s = view_->node(i);
    if (s.test_id != test_id) continue;
    if (!s.inactive) {
      match = i;
      break;
    }
    if (match < 0) match = i;
  }
  if (match < 0) return false;
  input_at_us_ = board_.micros();
  press(match);
  return true;
}

void Runtime::dispatch_pan(int instance, E event, double a, double b) {
  if (instance < 0 || view_->node(instance).inactive) return;
  int at = -1;
  const int h = handler(instance, event, &at);
  if (h < 0) return;
  if (input_at_us_ < 0) input_at_us_ = board_.micros();
  Scope* scope = view_->node(at).scope;
  const V payload[2] = {V::num(a), V::num(b)};
  std::vector<V> params = params_for(*this, app_, app_.handlers[h], scope, payload, 2);
  run(app_.handlers[h].action, params.data(), scope);
}

bool Runtime::pan(const char* test_id, E event, double a, double b) {
  int match = -1;
  for (int i = 0; i < view_->count(); ++i) {
    if (view_->node(i).test_id != test_id) continue;
    if (match < 0 || (view_->node(match).inactive && !view_->node(i).inactive)) match = i;
  }
  if (match < 0) return false;
  input_at_us_ = board_.micros();
  dispatch_pan(match, event, a, b);
  return true;
}

void Runtime::pointer(Input::K phase, int x, int y) {
  Contact& c = contact_;
  const int64_t now = board_.micros();
  if (phase == Input::K::Down) {
    c = Contact{};
    c.down = true;
    c.start_x = c.x = x;
    c.start_y = c.y = y;
    c.at_us = now;
    const auto& pans = view_->pans();
    for (auto it = pans.rbegin(); it != pans.rend(); ++it) {
      if (inside(it->second, x, y)) {
        c.target = it->first;
        c.target_key = view_->node(it->first).key;
        break;
      }
    }
    return;
  }
  if (!c.down) return;
  if (phase == Input::K::Move) {
    if (x == c.x && y == c.y) return;
    if (!c.moved && std::abs(x - c.start_x) <= kSlop && std::abs(y - c.start_y) <= kSlop) return;
    c.moved = true;
    const double dt = std::max<int64_t>(1, now - c.at_us) / 1e6;
    const int from_x = c.panning ? c.x : c.start_x, from_y = c.panning ? c.y : c.start_y;
    c.vx = (x - c.x) / dt;
    c.vy = (y - c.y) / dt;
    c.x = x;
    c.y = y;
    c.at_us = now;
    if (c.target_key.empty()) return;
    c.panning = true;
    dispatch_pan(view_->find(c.target_key), E::Pan, x - from_x, y - from_y);
    return;
  }
  c.down = false;
  if (c.panning) {
    dispatch_pan(view_->find(c.target_key), E::PanRelease, c.vx, c.vy);
    return;
  }
  if (c.moved) return;
  const auto& hits = view_->hits();
  for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
    if (inside(it->second, c.start_x, c.start_y)) {
      input_at_us_ = now;
      press(it->first);
      return;
    }
  }
}

std::vector<std::string> Runtime::missing() const {
  std::vector<std::string> out;
  const Inputs controls = board_.inputs();
  if (app_.node_count > 0 && !board_.display()) out.push_back("display");
  if (app_.keys && !controls.keys) out.push_back("keys");
  if (app_.touch && !controls.touch) out.push_back("touch");
  if (app_.sound_count > 0 && !board_.speaker()) out.push_back("speaker");
  for (uint16_t i = 0; i < app_.command_count; ++i) {
    if (!board_.can(app_.commands[i])) out.push_back(std::string("command:") + app_.commands[i]);
  }
  for (uint16_t i = 0; i < app_.extension_count; ++i) {
    if (!bound_[i]) out.push_back(std::string("extension:") + app_.extensions[i].name);
  }
  return out;
}

std::vector<Mark> Runtime::marks(const char* word) const {
  std::vector<Mark> out;
  std::string value;
  for (int i = 0; i < view_->count(); ++i) {
    const NodeState& s = view_->node(i);
    if (!data_word(s.data, word, value)) continue;
    out.push_back({s.test_id, value, s.rect, s.inactive});
  }
  return out;
}

std::vector<std::string> Runtime::stale() const {
  std::vector<std::string> out;
  for (uint16_t i = 0; i < app_.extension_count; ++i) {
    if (bound_[i] && std::strcmp(bound_[i]->digest(), app_.extensions[i].digest) != 0) {
      out.push_back(app_.extensions[i].name);
    }
  }
  return out;
}

// The extensions the app lists: each one's version and digest, whether the
// board gives it (and from the same file), and the sources, ops and props the
// given one declares, so an agent can find the ops it may send.
std::string Runtime::extensions_json() {
  std::string out = "{\"extensions\":[";
  for (uint16_t i = 0; i < app_.extension_count; ++i) {
    const ExtensionUse& use = app_.extensions[i];
    Extension* e = bound_[i];
    if (i) out += ',';
    out += "{\"name\":";
    quote(use.name, out);
    out += ",\"version\":" + std::to_string(use.version) + ",\"sha256\":";
    quote(use.digest, out);
    out += e ? ",\"bound\":true" : ",\"bound\":false";
    out += e && std::strcmp(e->digest(), use.digest) == 0 ? ",\"same_file\":true" : ",\"same_file\":false";
    static const char* const kLists[] = {",\"sources\":[", ",\"ops\":[", ",\"props\":["};
    for (int list = 0; list < 3; ++list) {
      uint16_t count = 0;
      const char* const* names = !e ? nullptr : list == 0 ? e->sources(count) : list == 1 ? e->ops(count) : e->props(count);
      out += kLists[list];
      for (uint16_t k = 0; k < count; ++k) {
        if (k) out += ',';
        quote(names[k], out);
      }
      out += ']';
    }
    out += '}';
  }
  return out + "]}";
}

std::string Runtime::state_json() {
  std::string out = "{\"slots\":{";
  bool first = true;
  for (uint16_t i = 0; i < app_.slot_count; ++i) {
    // A row slot has a value per row, not one here (LLP 1017 P4c).
    if (app_.slot_owners && app_.slot_owners[i] >= 0) continue;
    if (!first) out += ',';
    first = false;
    quote(app_.slot_names[i], out);
    out += ':';
    json(slots_[i], out, app_.types, app_.fields, app_.slot_types ? app_.slot_types[i] : -1);
  }
  out += "},\"derives\":{";
  for (uint16_t i = 0; i < app_.derive_count; ++i) {
    if (i) out += ',';
    quote(app_.derive_names[i], out);
    out += ':';
    json(derive(i), out, app_.types, app_.fields, app_.derive_types ? app_.derive_types[i] : -1);
  }
  out += "},\"resources\":{";
  for (uint16_t i = 0; i < app_.resource_count; ++i) {
    if (i) out += ',';
    quote(app_.resources[i].name, out);
    out += ':';
    json(resources_[i], out, app_.types, app_.fields, app_.resources[i].ty);
  }
  out += "}}";
  return out;
}

// Every instance with a testId, in tree order: its text (a text node's own),
// its `data-*` words if it has any, and whether it lies under an unselected
// route.
std::string Runtime::nodes_json() {
  std::string out = "{\"nodes\":[";
  bool first = true;
  for (int i = 0; i < view_->count(); ++i) {
    const NodeState& s = view_->node(i);
    if (s.test_id.empty()) continue;
    if (!first) out += ',';
    first = false;
    out += "{\"testId\":";
    quote(s.test_id, out);
    out += ",\"text\":";
    quote(app_.nodes[s.node].type == T::Text ? s.text : "", out);
    if (!s.data.empty()) out += ",\"data\":" + s.data;
    out += s.inactive ? ",\"inactive\":true}" : ",\"inactive\":false}";
  }
  return out + "]}";
}

std::string Runtime::stats_json() {
  char buf[256];
  std::snprintf(buf, sizeof buf,
                "{\"frames\":%u,\"commits\":%u,\"paint_us\":%lld,\"push_us\":%lld,\"damage_px\":%lld,"
                "\"input_to_panel_us\":%lld,\"ready_ms\":%u",
                static_cast<unsigned>(stats.frames), static_cast<unsigned>(stats.commits),
                static_cast<long long>(stats.paint_us), static_cast<long long>(stats.push_us),
                static_cast<long long>(stats.damage_px), static_cast<long long>(stats.input_to_panel_us),
                static_cast<unsigned>(stats.ready_ms));
  std::string out = buf;
  for (const Counter& c : board_.counters()) {
    out += ",\"" + std::string(c.name) + "\":" + std::to_string(c.value);
  }
  out += ",\"missing\":[";
  const std::vector<std::string> gaps = missing();
  for (size_t i = 0; i < gaps.size(); ++i) out += (i ? ",\"" : "\"") + gaps[i] + "\"";
  return out + "]}";
}

std::string Runtime::agent(const std::string& line) {
  std::string op = string_field(line, "op");
  for (Extension* e : bound_) {
    std::string answer;
    if (e && e->op(*this, line, answer)) return answer;
  }
  if (op == "state") return state_json();
  if (op == "extensions") return extensions_json();
  if (op == "nodes") return nodes_json();
  if (op == "key") {
    std::string k = string_field(line, "key");
    if (k.empty()) return "{\"error\":\"key needs \\\"key\\\"\"}";
    key(k.c_str());
    return "{\"ok\":true}";
  }
  if (op == "tap") {
    return tap(string_field(line, "id").c_str()) ? "{\"ok\":true}" : "{\"error\":\"tap needs the \\\"id\\\" of a live testId\"}";
  }
  if (op == "pan" || op == "panrelease") {
    const bool release = op == "panrelease";
    const double a = number_field(line, release ? "vx" : "dx"), b = number_field(line, release ? "vy" : "dy");
    return pan(string_field(line, "id").c_str(), release ? E::PanRelease : E::Pan, a, b)
               ? "{\"ok\":true}"
               : "{\"error\":\"pan needs the \\\"id\\\" of a live testId\"}";
  }
  if (op == "pointer") {
    const std::string phase = string_field(line, "phase");
    const Input::K k = phase == "down" ? Input::K::Down : phase == "move" ? Input::K::Move : Input::K::Up;
    if (phase != "down" && phase != "move" && phase != "up") return "{\"error\":\"pointer needs a phase: down, move, or up\"}";
    pointer(k, static_cast<int>(number_field(line, "x")), static_cast<int>(number_field(line, "y")));
    return "{\"ok\":true}";
  }
  if (op == "navigate") {
    return navigate(string_field(line, "url")) ? "{\"ok\":true}" : "{\"error\":\"no navigation root takes navigate\"}";
  }
  if (op == "logs") {
    std::string out = "{\"lines\":[";
    for (size_t i = 0; i < logs_.size(); ++i) {
      if (i) out += ',';
      quote(logs_[i], out);
    }
    return out + "]}";
  }
  if (op == "stats") return stats_json();
  if (op == "sounds") return transport_->json();
  if (op == "clock") {
    return advance(number_field(line, "ms")) ? "{\"ok\":true}" : "{\"error\":\"this board's clock is real\"}";
  }
  return "{\"error\":\"unknown op\"}";
}

}  // namespace rt
