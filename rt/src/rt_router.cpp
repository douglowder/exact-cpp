// The router at the plan seam (the runner's runner/router.rs): the routes
// slot holds a positional Router{tab, tabs, next} with Tab{name, stack} and
// Entry{id, name, url, tab, params, fragment}, Params in the table's first-declaration
// order. A verb on a value that is not a valid router traps; a refused verb
// returns its input and is journaled once per commit.
#include <algorithm>
#include <cmath>

#include "exact_rt.h"

namespace rt {

namespace {

// Stdlib ordinals (Exact plan/tables/format.json roster order).
enum Fn : uint8_t {
  kOpen = 8, kPush, kReplace, kBack, kSelect, kGo, kStack, kTop, kDepth, kParams, kSearchParam,
  kEncodeUriComponent, kEncodeRouteSegment,
  kFragment = 60,
};

const char* verb_name(uint8_t fn) {
  switch (fn) {
    case kOpen: return "open";
    case kPush: return "push";
    case kReplace: return "replace";
    case kBack: return "back";
    case kSelect: return "select";
    default: return "go";
  }
}

bool integer(const V& v, uint64_t& out) {
  if (v.k != V::K::Num || !std::isfinite(v.n) || v.n < 0 || std::floor(v.n) != v.n || v.n >= 9007199254740992.0) {
    return false;
  }
  out = static_cast<uint64_t>(v.n);
  return true;
}

bool text(const V& v, std::string& out) {
  if (v.k != V::K::Str) return false;
  out = v.s;
  return true;
}

}  // namespace

V Runtime::entry_value(const route::Entry& e) const {
  std::vector<V> params;
  for (const std::string& n : table_->param_names()) {
    const std::string* v = e.params.get(n);
    params.push_back(V::str(v ? *v : ""));
  }
  return V::record({V::num(static_cast<double>(e.id)), V::str(e.name), V::str(e.url), V::str(e.tab),
                    V::record(std::move(params)), V::str(e.fragment)});
}

V Runtime::router_value(const route::Router& r) const {
  std::vector<V> tabs;
  for (const route::Tab& t : r.tabs) {
    std::vector<V> stack;
    for (const route::Entry& e : t.stack) stack.push_back(entry_value(e));
    tabs.push_back(V::record({V::str(t.name), V::list(std::move(stack))}));
  }
  return V::record({V::str(r.tab), V::list(std::move(tabs)), V::num(static_cast<double>(r.next))});
}

route::Entry Runtime::entry_of_value(const V& v, bool* ok) const {
  route::Entry e;
  *ok = false;
  if (v.k != V::K::Rec || v.size() != 6) return e;
  const V& params = (*v.rec)[4];
  const std::vector<std::string>& names = table_->param_names();
  if (params.k != V::K::Rec || params.size() != names.size()) return e;
  if (!integer((*v.rec)[0], e.id) || !text((*v.rec)[1], e.name) || !text((*v.rec)[2], e.url) ||
      !text((*v.rec)[3], e.tab) || !text((*v.rec)[5], e.fragment)) {
    return e;
  }
  for (size_t i = 0; i < names.size(); ++i) {
    std::string value;
    if (!text((*params.rec)[i], value)) return e;
    e.params.insert(names[i], value);
  }
  *ok = true;
  return e;
}

// A positional Router and the runner's checks on it: distinct tab names,
// each stack rooted at its tab's route, entries of their tab, ids distinct
// and below `next`, canonical URLs that match back to the same name and
// params, and a top.
bool Runtime::router_of(const V& v, route::Router& out) const {
  if (!table_ || v.k != V::K::Rec || v.size() != 3) return false;
  const V& tabs = (*v.rec)[1];
  if (!text((*v.rec)[0], out.tab) || tabs.k != V::K::List || !integer((*v.rec)[2], out.next)) return false;
  out.tabs.clear();
  for (const V& t : *tabs.rec) {
    if (t.k != V::K::Rec || t.size() != 2) return false;
    route::Tab tab;
    const V& stack = (*t.rec)[1];
    if (!text((*t.rec)[0], tab.name) || stack.k != V::K::List) return false;
    for (const V& e : *stack.rec) {
      bool ok = false;
      tab.stack.push_back(entry_of_value(e, &ok));
      if (!ok) return false;
    }
    out.tabs.push_back(std::move(tab));
  }
  if (!route::top(out)) return false;
  std::vector<std::string> names;
  std::vector<uint64_t> ids;
  for (const route::Tab& t : out.tabs) {
    if (std::find(names.begin(), names.end(), t.name) != names.end()) return false;
    names.push_back(t.name);
    if (t.stack.empty() || t.stack.front().name != t.name) return false;
    for (const route::Entry& e : t.stack) {
      if (e.tab != t.name || e.id >= out.next) return false;
      if (std::find(ids.begin(), ids.end(), e.id) != ids.end()) return false;
      ids.push_back(e.id);
      if (route::canonical(e.url) != e.url) return false;
      std::optional<route::Match> m = table_->matches(e.url);
      if (!m || table_->routes()[m->index].name != e.name || !(m->params == e.params)) return false;
    }
  }
  return true;
}

V Runtime::call(uint8_t fn, const V* args, int argc) {
  if (argc < 1 || !table_) {
    trap("a route function with no routes");
    return V();
  }
  if (fn == kEncodeUriComponent) return V::str(route::encode_uri_component(args[0].s));
  if (fn == kEncodeRouteSegment) {
    std::optional<std::string> seg = route::encode_route_segment(args[0].s);
    if (!seg) trap("a path parameter cannot be empty, `.` or `..`");
    return seg ? V::str(*seg) : V();
  }
  if (fn == kSearchParam) {
    bool ok = false;
    route::Entry e = entry_of_value(args[0], &ok);
    if (!ok || argc < 2) {
      trap("searchParam of a value that is not an entry");
      return V();
    }
    return V::str(route::search_param(e, args[1].s));
  }
  if (fn == kFragment) {
    bool ok = false;
    route::Entry e = entry_of_value(args[0], &ok);
    if (!ok) {
      trap("fragment of a value that is not an entry");
      return V();
    }
    return V::str(e.fragment);
  }
  route::Router r;
  if (!router_of(args[0], r)) {
    trap("an invalid router value");
    return V();
  }
  const std::string arg = argc > 1 ? args[1].s : "";
  switch (fn) {
    case kStack: {
      std::vector<V> out;
      for (const route::Entry& e : route::stack(r)) out.push_back(entry_value(e));
      return V::list(std::move(out));
    }
    case kTop: return entry_value(*route::top(r));
    case kDepth: return V::num(static_cast<double>(route::depth(r)));
    case kParams: {
      std::vector<V> out;
      for (const std::string& p : route::params(r, arg)) out.push_back(V::str(p));
      return V::list(std::move(out));
    }
    default: break;
  }
  route::Result res = fn == kOpen      ? route::open(*table_, r, arg)
                      : fn == kPush    ? route::push(*table_, r, arg)
                      : fn == kReplace ? route::replace(*table_, r, arg)
                      : fn == kBack    ? route::back(*table_, r)
                      : fn == kSelect  ? route::select(*table_, r, arg)
                                       : route::go(*table_, r, arg);
  if (res.refusal) {
    const std::string line = std::string("router ") + verb_name(fn) + " refused: " + *res.refusal;
    if (std::find(refusals_.begin(), refusals_.end(), line) == refusals_.end()) {
      refusals_.push_back(line);
      log(line);
    }
    return args[0];
  }
  return router_value(res.router);
}

}  // namespace rt
