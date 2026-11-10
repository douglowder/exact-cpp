// contract-rt routes: a C++ port of exact2's `exact-route` crate (route/src,
// LLP 1038 §3): canonical locations, matching, a deep link's chain, the six
// verbs, and the reads. It must give the same bytes as the Rust crate:
// tools/route_corpus.py runs route/tests/corpus.json against it. The static
// table checks (D2) are the compiler's and are not ported. Standard C++17.
#pragma once

#include <stdint.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rt {
namespace route {

// Every table parameter, unbound names "", sorted by name (as Rust's
// BTreeMap<String, String>); insert replaces an equal name.
class Params {
 public:
  const std::string* get(const std::string& name) const;
  void insert(const std::string& name, const std::string& value);
  const std::vector<std::pair<std::string, std::string>>& items() const { return items_; }
  bool operator==(const Params& o) const { return items_ == o.items_; }

 private:
  std::vector<std::pair<std::string, std::string>> items_;
};

struct Route {
  std::string name;
  std::string pattern;  // empty for notfound
  int parent = -1;
  bool tab = false;
  bool notfound = false;
};

struct Match {
  size_t index;
  Params params;
};

// An entry before it has a visit id.
struct Destination {
  std::string name, url, tab;
  Params params;
};

struct Entry {
  uint64_t id = 0;
  std::string name, url, tab;
  Params params;
  // What the location that last named this visit had after its `#`,
  // fragment-encoded (fragment_of); "" when none. Not part of the visit's
  // identity: `url` is.
  std::string fragment;
  bool operator==(const Entry& o) const {
    return id == o.id && name == o.name && url == o.url && tab == o.tab && params == o.params &&
           fragment == o.fragment;
  }
};

struct Tab {
  std::string name;
  std::vector<Entry> stack;
  bool operator==(const Tab& o) const { return name == o.name && stack == o.stack; }
};

struct Router {
  std::string tab;
  std::vector<Tab> tabs;
  uint64_t next = 0;
  bool operator==(const Router& o) const { return tab == o.tab && tabs == o.tabs && next == o.next; }
};

class Table {
 public:
  explicit Table(std::vector<Route> routes);
  const std::vector<Route>& routes() const { return routes_; }
  // Parameter names in first-declaration order: the plan's Params field order.
  const std::vector<std::string>& param_names() const { return names_; }
  Params empty_params() const;
  std::optional<Match> matches(const std::string& location) const;
  // Declared patterns only: no notfound fallback.
  std::optional<Match> matches_pattern(const std::string& location) const;
  std::optional<Match> match_index(const std::string& url) const;
  // A named route formatted with encodeRouteSegment on each parameter, or the
  // error message in `error`.
  std::optional<std::string> path(const std::string& name, const std::vector<std::string>& params,
                                  std::string* error) const;
  std::vector<Destination> chain(const std::string& location) const;
  std::vector<size_t> roots() const;

 private:
  std::optional<Match> match_pattern_index(const std::string& url) const;
  std::optional<size_t> root_for(size_t index) const;

  std::vector<Route> routes_;
  std::vector<std::string> names_;
};

std::string canonical(const std::string& location);
// A host's incoming URL as a location, its fragment kept.
std::string location_of(const std::string& href);
// The browser's `hash` without its `#`, fragment-encoded; "" when none.
std::string fragment_of(const std::string& location);
std::string encode_uri_component(const std::string& value);
// encodeURIComponent, or nullopt for "", "." and "..".
std::optional<std::string> encode_route_segment(const std::string& value);
std::string decode(const std::string& input, bool plus);
std::string search_param(const Entry& entry, const std::string& name);

// A verb's result: the new value, or the input and a refusal to journal.
struct Result {
  Router router;
  std::optional<std::string> refusal;
};

Result launch(const Table& t, const std::string& location);
Result open(const Table& t, const Router& r, const std::string& location);
Result push(const Table& t, const Router& r, const std::string& location);
Result replace(const Table& t, const Router& r, const std::string& location);
Result back(const Table& t, const Router& r);
Result select(const Table& t, const Router& r, const std::string& name);
Result go(const Table& t, const Router& r, const std::string& location);

// The selected stack, root first; empty before launch.
const std::vector<Entry>& stack(const Router& r);
const Entry* top(const Router& r);
size_t depth(const Router& r);
std::vector<std::string> params(const Router& r, const std::string& name);

}  // namespace route
}  // namespace rt
