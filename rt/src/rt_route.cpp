// The port of exact2's route/src (location.rs, table.rs, router.rs). Each
// function keeps the Rust function's name and order of checks, so a refusal
// names the same reason.
#include "rt_route.h"

#include <algorithm>

namespace rt {
namespace route {

namespace {

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

bool ends_with(const std::string& s, char c) { return !s.empty() && s.back() == c; }

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == sep) {
      out.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

std::string lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

bool alpha(unsigned char b) { return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z'); }

bool alnum(unsigned char b) { return alpha(b) || (b >= '0' && b <= '9'); }

template <typename Escape>
std::string encode(const std::string& input, Escape escape) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(input.size());
  for (unsigned char b : input) {
    if (escape(b)) {
      out += '%';
      out += kHex[b >> 4];
      out += kHex[b & 15];
    } else {
      out += static_cast<char>(b);
    }
  }
  return out;
}

bool path_escape(unsigned char b) {
  return b < 0x21 || b > 0x7e || b == '"' || b == '#' || b == '<' || b == '>' || b == '?' || b == '^' ||
         b == '`' || b == '{' || b == '}' || b == '|';
}

bool query_escape(unsigned char b) {
  return b < 0x21 || b > 0x7e || b == '"' || b == '#' || b == '<' || b == '>' || b == '\'';
}

// WHATWG URL's fragment percent-encode set: C0 controls, space, `"`, `<`,
// `>`, `` ` `` and every non-ASCII byte.
bool fragment_escape(unsigned char b) {
  return b < 0x21 || b > 0x7e || b == '"' || b == '<' || b == '>' || b == '`';
}

// Trims bytes up to ' ' at both ends (every char <= ' ' is one ASCII byte),
// then drops tab, LF, and CR.
std::string clean(const std::string& input) {
  size_t a = 0, b = input.size();
  while (a < b && static_cast<unsigned char>(input[a]) <= ' ') ++a;
  while (b > a && static_cast<unsigned char>(input[b - 1]) <= ' ') --b;
  std::string out;
  for (size_t i = a; i < b; ++i) {
    if (input[i] != '\t' && input[i] != '\n' && input[i] != '\r') out += input[i];
  }
  return out;
}

std::optional<std::pair<std::string, std::string>> scheme(const std::string& input) {
  size_t colon = input.find(':');
  if (colon == std::string::npos) return std::nullopt;
  std::string name = input.substr(0, colon);
  if (name.empty() || !alpha(static_cast<unsigned char>(name[0]))) return std::nullopt;
  for (size_t i = 1; i < name.size(); ++i) {
    unsigned char b = static_cast<unsigned char>(name[i]);
    if (!alnum(b) && b != '+' && b != '-' && b != '.') return std::nullopt;
  }
  return std::make_pair(name, input.substr(colon + 1));
}

std::string without_authority(const std::string& input) {
  size_t end = input.find_first_of("/\\?#");
  return end == std::string::npos ? std::string() : input.substr(end);
}

std::string path_query(const std::string& whole) {
  std::string input = whole.substr(0, whole.find('#'));
  size_t q = input.find('?');
  std::string path = q == std::string::npos ? input : input.substr(0, q);
  std::string query = q == std::string::npos ? std::string() : input.substr(q + 1);
  std::replace(path.begin(), path.end(), '\\', '/');
  if (starts_with(path, "/")) path.erase(0, 1);
  std::vector<std::string> parts = split(path, '/'), segments;
  for (size_t i = 0; i < parts.size(); ++i) {
    const bool last = i + 1 == parts.size();
    const std::string dot = lower(parts[i]);
    if (dot == "." || dot == "%2e") {
      if (last) segments.emplace_back();
    } else if (dot == ".." || dot == ".%2e" || dot == "%2e." || dot == "%2e%2e") {
      if (!segments.empty()) segments.pop_back();
      if (last) segments.emplace_back();
    } else {
      segments.push_back(encode(parts[i], path_escape));
    }
  }
  std::string out = "/";
  for (size_t i = 0; i < segments.size(); ++i) {
    if (i) out += '/';
    out += segments[i];
  }
  if (!query.empty()) out += "?" + encode(query, query_escape);
  return out;
}

int hex(unsigned char b) {
  if (b >= '0' && b <= '9') return b - '0';
  if (b >= 'a' && b <= 'f') return b - 'a' + 10;
  if (b >= 'A' && b <= 'F') return b - 'A' + 10;
  return -1;
}

// Rust's String::from_utf8_lossy: each maximal invalid subpart becomes U+FFFD.
std::string lossy(const std::string& in) {
  static const char kReplacement[] = "\xEF\xBF\xBD";
  std::string out;
  const size_t n = in.size();
  auto at = [&](size_t i) { return static_cast<unsigned char>(in[i]); };
  auto cont = [&](size_t i) { return i < n && at(i) >= 0x80 && at(i) <= 0xBF; };
  size_t i = 0;
  while (i < n) {
    const unsigned char b = at(i);
    if (b < 0x80) {
      out += static_cast<char>(b);
      ++i;
      continue;
    }
    int width = 0;
    unsigned char lo = 0x80, hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) width = 2;
    else if (b == 0xE0) width = 3, lo = 0xA0;
    else if ((b >= 0xE1 && b <= 0xEC) || b == 0xEE || b == 0xEF) width = 3;
    else if (b == 0xED) width = 3, hi = 0x9F;
    else if (b == 0xF0) width = 4, lo = 0x90;
    else if (b >= 0xF1 && b <= 0xF3) width = 4;
    else if (b == 0xF4) width = 4, hi = 0x8F;
    if (width == 0 || i + 1 >= n || at(i + 1) < lo || at(i + 1) > hi) {
      out += kReplacement;
      ++i;
      continue;
    }
    size_t k = 2;
    while (static_cast<int>(k) < width && cont(i + k)) ++k;
    if (static_cast<int>(k) < width) {
      out += kReplacement;
      i += k;
      continue;
    }
    out.append(in, i, width);
    i += width;
  }
  return out;
}

std::vector<std::string> names_in(const std::string& pattern) {
  std::vector<std::string> out;
  for (const std::string& segment : split(pattern, '/')) {
    if (starts_with(segment, ":")) out.push_back(segment.substr(1));
  }
  return out;
}

std::vector<std::string> segments(const std::string& path) {
  if (path == "/") return {};
  return split(starts_with(path, "/") ? path.substr(1) : path, '/');
}

std::string no_match(const std::string& location) { return "no route matches " + canonical(location); }

std::optional<size_t> selected(const Router& r) {
  for (size_t i = 0; i < r.tabs.size(); ++i) {
    if (r.tabs[i].name == r.tab && !r.tabs[i].stack.empty()) return i;
  }
  return std::nullopt;
}

const char* const kNoSelected = "router has no selected stack";

// A visit id; false when the ids would pass 2^53 - 1.
bool mint(uint64_t& next, Destination d, Entry& out) {
  if (next >= (uint64_t{1} << 53) - 1) return false;
  out = Entry{next, std::move(d.name), std::move(d.url), std::move(d.tab), std::move(d.params), ""};
  ++next;
  return true;
}

const char* const kExhausted = "router entry ids exhausted";

Result refuse(const Router& r, std::string why) { return Result{r, std::move(why)}; }

std::optional<Destination> destination(const Table& t, const Router& r, const std::string& location) {
  std::string url = canonical(location);
  std::optional<Match> m = t.match_index(url);
  if (!m) return std::nullopt;
  return Destination{t.routes()[m->index].name, url, r.tab, m->params};
}

}  // namespace

const std::string* Params::get(const std::string& name) const {
  for (const auto& kv : items_) {
    if (kv.first == name) return &kv.second;
  }
  return nullptr;
}

void Params::insert(const std::string& name, const std::string& value) {
  auto it = std::lower_bound(items_.begin(), items_.end(), name,
                             [](const std::pair<std::string, std::string>& kv, const std::string& n) {
                               return kv.first < n;
                             });
  if (it != items_.end() && it->first == name) it->second = value;
  else items_.insert(it, {name, value});
}

std::string canonical(const std::string& location) {
  return path_query(clean(starts_with(location, "/") ? location : "/" + location));
}

std::string fragment_of(const std::string& location) {
  const std::string input = clean(location);
  const size_t hash = input.find('#');
  return hash == std::string::npos ? std::string() : encode(input.substr(hash + 1), fragment_escape);
}

std::string location_of(const std::string& href) {
  const std::string input = clean(href);
  std::string location;
  auto s = scheme(input);
  if (!s) {
    location = canonical(input);
  } else if (lower(s->first) != "http" && lower(s->first) != "https") {
    const std::string& after = s->second;
    location = path_query("/" + (starts_with(after, "//") ? after.substr(2) : after));
  } else {
    const std::string& after = s->second;
    size_t a = after.find_first_not_of("/\\");
    location = path_query(without_authority(a == std::string::npos ? std::string() : after.substr(a)));
  }
  const std::string fragment = fragment_of(input);
  return fragment.empty() ? location : location + "#" + fragment;
}

std::string encode_uri_component(const std::string& value) {
  return encode(value, [](unsigned char b) {
    return !alnum(b) && b != '-' && b != '_' && b != '.' && b != '!' && b != '~' && b != '*' && b != '\'' &&
           b != '(' && b != ')';
  });
}

std::optional<std::string> encode_route_segment(const std::string& value) {
  if (value.empty() || value == "." || value == "..") return std::nullopt;
  return encode_uri_component(value);
}

std::string decode(const std::string& input, bool plus) {
  std::string out;
  for (size_t i = 0; i < input.size(); ++i) {
    const unsigned char b = static_cast<unsigned char>(input[i]);
    if (b == '%' && i + 2 < input.size()) {
      int h = hex(static_cast<unsigned char>(input[i + 1])), l = hex(static_cast<unsigned char>(input[i + 2]));
      if (h >= 0 && l >= 0) {
        out += static_cast<char>(h * 16 + l);
        i += 2;
        continue;
      }
    }
    out += plus && b == '+' ? ' ' : static_cast<char>(b);
  }
  return lossy(out);
}

std::string search_param(const Entry& entry, const std::string& name) {
  size_t q = entry.url.find('?');
  if (q == std::string::npos) return "";
  std::string query = entry.url.substr(q + 1);
  query = query.substr(0, query.find('#'));
  for (const std::string& pair : split(query, '&')) {
    if (pair.empty()) continue;
    size_t eq = pair.find('=');
    std::string key = eq == std::string::npos ? pair : pair.substr(0, eq);
    std::string value = eq == std::string::npos ? std::string() : pair.substr(eq + 1);
    if (decode(key, true) == name) return decode(value, true);
  }
  return "";
}

Table::Table(std::vector<Route> routes) : routes_(std::move(routes)) {
  for (const Route& r : routes_) {
    if (r.notfound) continue;
    for (const std::string& n : names_in(r.pattern)) {
      if (std::find(names_.begin(), names_.end(), n) == names_.end()) names_.push_back(n);
    }
  }
}

Params Table::empty_params() const {
  Params p;
  for (const std::string& n : names_) p.insert(n, "");
  return p;
}

std::optional<Match> Table::matches(const std::string& location) const { return match_index(canonical(location)); }

std::optional<Match> Table::matches_pattern(const std::string& location) const {
  return match_pattern_index(canonical(location));
}

std::optional<Match> Table::match_index(const std::string& url) const {
  if (auto m = match_pattern_index(url)) return m;
  for (size_t i = 0; i < routes_.size(); ++i) {
    if (routes_[i].notfound) return Match{i, empty_params()};
  }
  return std::nullopt;
}

std::optional<Match> Table::match_pattern_index(const std::string& url) const {
  const std::string path = url.substr(0, url.find('?'));
  const std::vector<std::string> parts = segments(path);
  if (path != "/" && ends_with(path, '/')) return std::nullopt;
  for (size_t i = 0; i < routes_.size(); ++i) {
    if (routes_[i].notfound) continue;
    const std::vector<std::string> pattern = segments(canonical(routes_[i].pattern));
    if (pattern.size() != parts.size()) continue;
    Params params = empty_params();
    bool matched = true;
    for (size_t k = 0; k < pattern.size() && matched; ++k) {
      if (starts_with(pattern[k], ":")) {
        if (parts[k].empty()) matched = false;
        else params.insert(pattern[k].substr(1), decode(parts[k], false));
      } else {
        matched = pattern[k] == parts[k];
      }
    }
    if (matched) return Match{i, std::move(params)};
  }
  return std::nullopt;
}

std::optional<std::string> Table::path(const std::string& name, const std::vector<std::string>& params,
                                       std::string* error) const {
  const Route* route = nullptr;
  for (const Route& r : routes_) {
    if (r.name == name && !r.notfound) {
      route = &r;
      break;
    }
  }
  if (!route) {
    std::string choices;
    for (const Route& r : routes_) {
      if (r.notfound) continue;
      choices += (choices.empty() ? "`" : ", `") + r.name + "`";
    }
    if (error) {
      *error = "unknown path route `" + name + "`; " +
               (choices.empty() ? std::string("no routes can be used with path") : "available routes: " + choices);
    }
    return std::nullopt;
  }
  const std::vector<std::string> expected = names_in(route->pattern);
  if (expected.size() != params.size()) {
    if (error) {
      std::string names;
      for (const std::string& n : expected) names += (names.empty() ? " (`" : ", `") + n + "`";
      if (!names.empty()) names += ")";
      *error = "`" + name + "` expects " + std::to_string(expected.size()) + " path " +
               (expected.size() == 1 ? "parameter" : "parameters") + names + ", received " +
               std::to_string(params.size());
    }
    return std::nullopt;
  }
  size_t next = 0;
  std::string out;
  const std::vector<std::string> parts = split(route->pattern, '/');
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out += '/';
    if (starts_with(parts[i], ":")) {
      auto seg = encode_route_segment(next < params.size() ? params[next] : "");
      ++next;
      if (!seg) {
        if (error) *error = "a path parameter cannot be empty, `.` or `..`";
        return std::nullopt;
      }
      out += *seg;
    } else {
      out += parts[i];
    }
  }
  return out;
}

std::vector<size_t> Table::roots() const {
  std::vector<size_t> out;
  for (size_t i = 0; i < routes_.size(); ++i) {
    if (routes_[i].tab) out.push_back(i);
  }
  if (out.empty() && !routes_.empty()) out.push_back(0);
  return out;
}

std::optional<size_t> Table::root_for(size_t index) const {
  const std::vector<size_t> rs = roots();
  if (index >= routes_.size()) return std::nullopt;
  if (!routes_[index].notfound) {
    int current = static_cast<int>(index);
    for (size_t step = 0; step < routes_.size() && current >= 0; ++step) {
      if (std::find(rs.begin(), rs.end(), static_cast<size_t>(current)) != rs.end()) {
        return static_cast<size_t>(current);
      }
      if (static_cast<size_t>(current) >= routes_.size()) return std::nullopt;
      current = routes_[current].parent;
    }
  }
  if (rs.empty()) return std::nullopt;
  return rs.front();
}

std::vector<Destination> Table::chain(const std::string& location) const {
  const std::string url = canonical(location);
  std::optional<Match> m = match_index(url);
  if (!m) return {};
  std::optional<size_t> root = root_for(m->index);
  if (!root) return {};
  const std::string tab = routes_[*root].name;
  std::vector<size_t> indices{m->index};
  if (!routes_[m->index].notfound) {
    int parent = routes_[m->index].parent;
    while (indices.back() != *root) {
      if (parent < 0) break;
      const size_t p = static_cast<size_t>(parent);
      if (std::find(indices.begin(), indices.end(), p) != indices.end()) return {};
      if (p >= routes_.size()) return {};
      indices.push_back(p);
      parent = routes_[p].parent;
    }
  }
  if (std::find(indices.begin(), indices.end(), *root) == indices.end()) indices.push_back(*root);
  std::reverse(indices.begin(), indices.end());
  std::vector<Destination> out;
  for (size_t i : indices) {
    const Route& route = routes_[i];
    std::string entry_url = url;
    if (i != m->index) {
      std::vector<std::string> values;
      for (const std::string& n : names_in(route.pattern)) {
        const std::string* v = m->params.get(n);
        values.push_back(v ? *v : "");
      }
      entry_url = canonical(path(route.name, values, nullptr).value_or(""));
    }
    Params own = empty_params();
    for (const std::string& n : names_in(route.pattern)) {
      if (const std::string* v = m->params.get(n)) own.insert(n, *v);
    }
    out.push_back(Destination{route.name, entry_url, tab, std::move(own)});
  }
  return out;
}

const std::vector<Entry>& stack(const Router& r) {
  static const std::vector<Entry> kEmpty;
  for (const Tab& t : r.tabs) {
    if (t.name == r.tab) return t.stack;
  }
  return kEmpty;
}

const Entry* top(const Router& r) {
  const std::vector<Entry>& s = stack(r);
  return s.empty() ? nullptr : &s.back();
}

size_t depth(const Router& r) { return stack(r).size(); }

std::vector<std::string> params(const Router& r, const std::string& name) {
  std::vector<std::string> out;
  for (const Entry& e : stack(r)) {
    const std::string* v = e.params.get(name);
    if (v && !v->empty()) out.push_back(*v);
  }
  return out;
}

Result launch(const Table& t, const std::string& location) { return open(t, Router{}, location); }

Result open(const Table& t, const Router& r, const std::string& location) {
  std::vector<Destination> chain = t.chain(location);
  if (chain.empty()) return refuse(r, no_match(location));
  const std::string target = chain.front().tab;
  Router out = r;
  if (out.tabs.empty()) {
    for (size_t root : t.roots()) {
      const Route& route = t.routes()[root];
      Entry e;
      if (!mint(out.next, Destination{route.name, canonical(route.pattern), route.name, t.empty_params()}, e)) {
        return refuse(r, kExhausted);
      }
      out.tabs.push_back(Tab{route.name, {std::move(e)}});
    }
  }
  size_t index = out.tabs.size();
  for (size_t i = 0; i < out.tabs.size(); ++i) {
    if (out.tabs[i].name == target) {
      index = i;
      break;
    }
  }
  if (index == out.tabs.size()) return refuse(r, "unknown tab " + target);
  // Same URL at the same position keeps its id and, above the leaf, its
  // fragment; the leaf takes the location's.
  std::vector<Entry> entries;
  for (size_t position = 0; position < chain.size(); ++position) {
    Destination& d = chain[position];
    const std::vector<Entry>& old = out.tabs[index].stack;
    Entry e;
    if (position < old.size() && old[position].url == d.url) {
      e = Entry{old[position].id, d.name, d.url, d.tab, d.params, old[position].fragment};
    } else if (!mint(out.next, d, e)) {
      return refuse(r, kExhausted);
    }
    if (position + 1 == chain.size()) e.fragment = fragment_of(location);
    entries.push_back(std::move(e));
  }
  out.tabs[index].stack = std::move(entries);
  out.tab = target;
  return Result{std::move(out), std::nullopt};
}

Result push(const Table& t, const Router& r, const std::string& location) {
  if (r.tabs.empty()) return open(t, r, location);
  std::optional<Destination> d = destination(t, r, location);
  if (!d) return refuse(r, no_match(location));
  std::optional<size_t> index = selected(r);
  if (!index) return refuse(r, kNoSelected);
  const std::string fragment = fragment_of(location);
  // A push of the location already on top is no new visit; a new fragment
  // changes that visit in place.
  const std::vector<Entry>& s = r.tabs[*index].stack;
  if (!s.empty() && s.back().url == d->url) {
    if (s.back().fragment == fragment) return Result{r, std::nullopt};
    Router out = r;
    out.tabs[*index].stack.back().fragment = fragment;
    return Result{std::move(out), std::nullopt};
  }
  Router out = r;
  Entry e;
  if (!mint(out.next, *d, e)) return refuse(r, kExhausted);
  e.fragment = fragment;
  out.tabs[*index].stack.push_back(std::move(e));
  return Result{std::move(out), std::nullopt};
}

Result replace(const Table& t, const Router& r, const std::string& location) {
  if (r.tabs.empty()) return open(t, r, location);
  std::optional<Destination> d = destination(t, r, location);
  if (!d) return refuse(r, no_match(location));
  std::optional<size_t> index = selected(r);
  if (!index) return refuse(r, kNoSelected);
  if (r.tabs[*index].stack.size() == 1 && d->name != r.tabs[*index].name) {
    return refuse(r, "replace cannot change the tab's root route");
  }
  Router out = r;
  if (out.tabs[*index].stack.empty()) return refuse(r, "router has no top");
  Entry& old = out.tabs[*index].stack.back();
  old = Entry{old.id, d->name, d->url, d->tab, d->params, fragment_of(location)};
  return Result{std::move(out), std::nullopt};
}

Result back(const Table&, const Router& r) {
  Router out = r;
  if (std::optional<size_t> index = selected(out)) {
    if (out.tabs[*index].stack.size() > 1) out.tabs[*index].stack.pop_back();
  }
  return Result{std::move(out), std::nullopt};
}

Result select(const Table&, const Router& r, const std::string& name) {
  size_t index = r.tabs.size();
  for (size_t i = 0; i < r.tabs.size(); ++i) {
    if (r.tabs[i].name == name && !r.tabs[i].stack.empty()) {
      index = i;
      break;
    }
  }
  if (index == r.tabs.size()) return refuse(r, "unknown tab " + name);
  Router out = r;
  if (out.tab == name) out.tabs[index].stack.resize(1);
  out.tab = name;
  return Result{std::move(out), std::nullopt};
}

// The top takes the location's fragment; a visit popped to or selected on
// another tab takes it only when the location has one, so a location without
// one returns to the visit as it was.
Result go(const Table& t, const Router& r, const std::string& location) {
  const std::string url = canonical(location);
  if (!t.match_index(url)) return refuse(r, no_match(location));
  const std::string fragment = fragment_of(location);
  const std::vector<Entry>& s = stack(r);
  for (size_t k = s.size(); k-- > 0;) {
    if (s[k].url == url) {
      Router out = r;
      if (std::optional<size_t> index = selected(out)) {
        std::vector<Entry>& stack = out.tabs[*index].stack;
        if (k + 1 == stack.size() || !fragment.empty()) stack[k].fragment = fragment;
        stack.resize(k + 1);
      }
      return Result{std::move(out), std::nullopt};
    }
  }
  for (size_t i = 0; i < r.tabs.size(); ++i) {
    const Tab& tab = r.tabs[i];
    if (tab.name != r.tab && !tab.stack.empty() && tab.stack.back().url == url) {
      if (fragment.empty()) return select(t, r, tab.name);
      Router out = r;
      out.tabs[i].stack.back().fragment = fragment;
      return select(t, out, tab.name);
    }
  }
  return push(t, r, location);
}

}  // namespace route
}  // namespace rt
