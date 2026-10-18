// Answers route corpus queries on stdin, one JSON line each, for
// tools/route_corpus.py. Every string argument is hex-encoded UTF-8, so a
// case may hold any bytes. Commands:
//   T NAME N            a table of N following rows: R NAME PATTERN PARENT TAB NOTFOUND
//   canonical S | location_of S | component S | segment S
//   search URL NAME     searchParam of an entry with that URL
//   match T LOC | pattern T LOC | chain T LOC | names T
//   path T NAME [P...]
//   seq T               start a sequence on table T from an empty router
//   step VERB [ARG]     apply a verb; prints {"router":…,"refusal":…}
//   params NAME | reads the selected stack's params, stack, top, depth
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "rt_route.h"

using namespace rt::route;

namespace {

// "-" stands for the empty string, which is no token.
std::string unhex(const std::string& h) {
  std::string out;
  if (h == "-") return out;
  for (size_t i = 0; i + 1 < h.size(); i += 2) out += static_cast<char>(std::stoi(h.substr(i, 2), nullptr, 16));
  return out;
}

std::string quote(const std::string& s) {
  static const char kHex[] = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c < 0x20) out += std::string("\\u00") + kHex[c >> 4] + kHex[c & 15];
    else out += static_cast<char>(c);
  }
  return out + "\"";
}

std::string params_json(const Params& p) {
  std::string out = "{";
  for (size_t i = 0; i < p.items().size(); ++i) {
    out += (i ? "," : "") + quote(p.items()[i].first) + ":" + quote(p.items()[i].second);
  }
  return out + "}";
}

std::string entry_json(const Entry& e) {
  return "{\"id\":" + std::to_string(e.id) + ",\"name\":" + quote(e.name) + ",\"url\":" + quote(e.url) +
         ",\"tab\":" + quote(e.tab) + ",\"params\":" + params_json(e.params) + "}";
}

std::string router_json(const Router& r) {
  std::string out = "{\"tab\":" + quote(r.tab) + ",\"tabs\":[";
  for (size_t i = 0; i < r.tabs.size(); ++i) {
    out += (i ? ",{" : "{") + std::string("\"name\":") + quote(r.tabs[i].name) + ",\"stack\":[";
    for (size_t k = 0; k < r.tabs[i].stack.size(); ++k) out += (k ? "," : "") + entry_json(r.tabs[i].stack[k]);
    out += "]}";
  }
  return out + "],\"next\":" + std::to_string(r.next) + "}";
}

std::string strings_json(const std::vector<std::string>& v) {
  std::string out = "[";
  for (size_t i = 0; i < v.size(); ++i) out += (i ? "," : "") + quote(v[i]);
  return out + "]";
}

std::string match_json(const Table& t, const std::optional<Match>& m) {
  if (!m) return "null";
  return "{\"name\":" + quote(t.routes()[m->index].name) + ",\"params\":" + params_json(m->params) + "}";
}

}  // namespace

int main() {
  std::map<std::string, Table> tables;
  const Table* seq_table = nullptr;
  Router r;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string op;
    in >> op;
    std::vector<std::string> a;
    for (std::string w; in >> w;) a.push_back(w);
    if (op == "T") {
      const int n = std::stoi(a[1]);
      std::vector<Route> routes;
      for (int i = 0; i < n && std::getline(std::cin, line); ++i) {
        std::istringstream row(line);
        std::string tag, name, pattern;
        int parent = -1, tab = 0, notfound = 0;
        row >> tag >> name >> pattern >> parent >> tab >> notfound;
        routes.push_back(Route{unhex(name), unhex(pattern), parent, tab != 0, notfound != 0});
      }
      tables.erase(unhex(a[0]));
      tables.emplace(unhex(a[0]), Table(std::move(routes)));
      std::cout << "{}\n";
    } else if (op == "canonical") {
      std::cout << quote(canonical(unhex(a.empty() ? "-" : a[0]))) << "\n";
    } else if (op == "location_of") {
      std::cout << quote(location_of(unhex(a.empty() ? "-" : a[0]))) << "\n";
    } else if (op == "component") {
      std::cout << quote(encode_uri_component(unhex(a.empty() ? "-" : a[0]))) << "\n";
    } else if (op == "search") {
      Entry e;
      e.url = unhex(a[0]);
      std::cout << quote(search_param(e, unhex(a[1]))) << "\n";
    } else if (op == "match" || op == "pattern") {
      const Table& t = tables.at(unhex(a[0]));
      const std::string loc = unhex(a.size() > 1 ? a[1] : "-");
      std::cout << match_json(t, op == "match" ? t.matches(loc) : t.matches_pattern(loc)) << "\n";
    } else if (op == "chain") {
      const Table& t = tables.at(unhex(a[0]));
      std::string out = "[";
      std::vector<Destination> c = t.chain(unhex(a.size() > 1 ? a[1] : "-"));
      for (size_t i = 0; i < c.size(); ++i) {
        out += (i ? "," : "") + std::string("{\"name\":") + quote(c[i].name) + ",\"url\":" + quote(c[i].url) +
               ",\"tab\":" + quote(c[i].tab) + ",\"params\":" + params_json(c[i].params) + "}";
      }
      std::cout << out << "]\n";
    } else if (op == "names") {
      std::cout << strings_json(tables.at(unhex(a[0])).param_names()) << "\n";
    } else if (op == "path") {
      const Table& t = tables.at(unhex(a[0]));
      std::vector<std::string> ps;
      for (size_t i = 2; i < a.size(); ++i) ps.push_back(unhex(a[i]));
      std::string error;
      std::optional<std::string> p = t.path(unhex(a[1]), ps, &error);
      std::cout << (p ? "{\"value\":" + quote(*p) + ",\"error\":null}" : "{\"value\":null,\"error\":\"route-unknown\"}")
                << "\n";
    } else if (op == "seq") {
      seq_table = &tables.at(unhex(a[0]));
      r = Router{};
      std::cout << "{}\n";
    } else if (op == "step") {
      const std::string verb = a[0];
      const std::string arg = a.size() > 1 ? unhex(a[1]) : "";
      Result res = verb == "launch"    ? launch(*seq_table, arg)
                   : verb == "open"    ? open(*seq_table, r, arg)
                   : verb == "push"    ? push(*seq_table, r, arg)
                   : verb == "replace" ? replace(*seq_table, r, arg)
                   : verb == "back"    ? back(*seq_table, r)
                   : verb == "select"  ? select(*seq_table, r, arg)
                                       : go(*seq_table, r, arg);
      r = res.router;
      std::cout << "{\"router\":" << router_json(r) << ",\"refusal\":"
                << (res.refusal ? "{\"message\":" + quote(*res.refusal) + "}" : std::string("null")) << "}\n";
    } else if (op == "params") {
      std::cout << strings_json(params(r, unhex(a[0]))) << "\n";
    } else if (op == "reads") {
      std::string s = "[";
      for (size_t i = 0; i < stack(r).size(); ++i) s += (i ? "," : "") + entry_json(stack(r)[i]);
      const Entry* t = top(r);
      std::cout << "{\"stack\":" << s << "],\"top\":" << (t ? entry_json(*t) : "null")
                << ",\"depth\":" << depth(r) << "}\n";
    } else {
      std::cout << "{\"error\":\"unknown command\"}\n";
    }
    std::cout.flush();
  }
  return 0;
}
