// Exact values with the runner's semantics (Exact runner/src/stdlib.rs,
// format.rs): JavaScript equality and ordering, max/min that skip NaN,
// toFixed on the exact binary value with ties away from zero, and
// JavaScript's number printing.
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "exact_rt.h"

namespace rt {

V V::num(double v) {
  V out;
  out.k = K::Num;
  out.n = v;
  return out;
}

V V::boolean(bool v) {
  V out;
  out.k = K::Bool;
  out.b = v;
  return out;
}

V V::str(const char* v) { return str(std::string(v)); }

V V::str(std::string v) {
  V out;
  out.k = K::Str;
  out.s = std::move(v);
  return out;
}

V V::record(std::vector<V> fields) {
  V out;
  out.k = K::Rec;
  out.rec = std::make_shared<const std::vector<V>>(std::move(fields));
  return out;
}

V V::list(std::vector<V> items) {
  V out;
  out.k = K::List;
  out.rec = std::make_shared<const std::vector<V>>(std::move(items));
  return out;
}

V V::field(uint64_t i) const {
  if (k != K::Rec || !rec || i >= rec->size()) return V();
  return (*rec)[i];
}

V add(const V& a, const V& b) { return V::num(a.n + b.n); }
V sub(const V& a, const V& b) { return V::num(a.n - b.n); }
V mul(const V& a, const V& b) { return V::num(a.n * b.n); }
V div(const V& a, const V& b) { return V::num(a.n / b.n); }
V rem(const V& a, const V& b) { return V::num(std::fmod(a.n, b.n)); }

bool equal(const V& a, const V& b) {
  if (a.k != b.k) return false;
  switch (a.k) {
    case V::K::None: return true;
    case V::K::Num: return a.n == b.n;  // NaN != NaN, 0 == -0, as ===
    case V::K::Bool: return a.b == b.b;
    case V::K::Str: return a.s == b.s;
    case V::K::Rec:
    case V::K::List: {
      if (a.rec->size() != b.rec->size()) return false;
      for (size_t i = 0; i < a.rec->size(); ++i) {
        if (!equal((*a.rec)[i], (*b.rec)[i])) return false;
      }
      return true;
    }
  }
  return false;
}

V eq(const V& a, const V& b) { return V::boolean(equal(a, b)); }
V ne(const V& a, const V& b) { return V::boolean(!equal(a, b)); }

// The sign of a - b; `ordered` is false when either number is NaN (every
// comparison is then false, as in JavaScript). Strings order by bytes, which
// for UTF-8 matches UTF-16 order except across surrogate pairs.
static int compare(const V& a, const V& b, bool& ordered) {
  ordered = true;
  if (a.k == V::K::Str) {
    int c = a.s.compare(b.s);
    return (c > 0) - (c < 0);
  }
  if (std::isnan(a.n) || std::isnan(b.n)) {
    ordered = false;
    return 0;
  }
  return (a.n > b.n) - (a.n < b.n);
}

V lt(const V& a, const V& b) { bool ok; int c = compare(a, b, ok); return V::boolean(ok && c < 0); }
V le(const V& a, const V& b) { bool ok; int c = compare(a, b, ok); return V::boolean(ok && c <= 0); }
V gt(const V& a, const V& b) { bool ok; int c = compare(a, b, ok); return V::boolean(ok && c > 0); }
V ge(const V& a, const V& b) { bool ok; int c = compare(a, b, ok); return V::boolean(ok && c >= 0); }

// Strings join; lists join their items (`[a].concat(b)`).
V concat(const V& a, const V& b) {
  if (a.k == V::K::List && b.k == V::K::List) {
    std::vector<V> items(a.rec->begin(), a.rec->end());
    items.insert(items.end(), b.rec->begin(), b.rec->end());
    return V::list(std::move(items));
  }
  return V::str(a.s + b.s);
}

V max(const V& a, const V& b) {
  if (std::isnan(a.n)) return b;
  if (std::isnan(b.n)) return a;
  return V::num(std::fmax(a.n, b.n));
}

V min(const V& a, const V& b) {
  if (std::isnan(a.n)) return b;
  if (std::isnan(b.n)) return a;
  return V::num(std::fmin(a.n, b.n));
}

V floor(const V& a) { return V::num(std::floor(a.n)); }
V ceil(const V& a) { return V::num(std::ceil(a.n)); }
// JavaScript's Math.round: halves toward +Infinity.
V round(const V& a) { return V::num(std::floor(a.n + 0.5)); }

std::string number_text(double x) {
  if (std::isnan(x)) return "NaN";
  if (std::isinf(x)) return x > 0 ? "Infinity" : "-Infinity";
  if (x == 0) return "0";
  char buf[64];
  double mag = std::fabs(x);
  if (mag >= 1e-7 && mag < 1e21) {
    auto r = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::fixed);
    return std::string(buf, r.ptr);
  }
  auto r = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::scientific);
  std::string s(buf, r.ptr);
  // JavaScript writes e+21 and e-7, without a leading zero in the exponent.
  size_t e = s.find('e');
  if (e != std::string::npos && e + 2 < s.size() && s[e + 2] == '0') s.erase(e + 2, 1);
  return s;
}

V to_string(const V& a) {
  switch (a.k) {
    case V::K::Num: return V::str(number_text(a.n));
    case V::K::Bool: return V::str(a.b ? "true" : "false");
    case V::K::Str: return a;
    default: return V::str("");
  }
}

// toFixed on the exact binary value: print far past the requested digits
// (newlib prints the exact expansion), then round half away from zero.
V to_fixed(const V& x, const V& digits) {
  if (!std::isfinite(x.n)) return V::str("");
  if (std::fabs(x.n) >= 1e21) return V::str(number_text(x.n));
  int d = static_cast<int>(digits.n);
  if (d < 0 || d > 100) return V::str("");
  char buf[400];
  std::snprintf(buf, sizeof buf, "%.*f", d + 40, std::fabs(x.n));
  std::string s(buf);
  size_t dot = s.find('.');
  std::string keep = s.substr(0, dot + 1 + d);
  bool up = s[dot + 1 + d] >= '5';
  if (up) {
    int i = static_cast<int>(keep.size()) - 1;
    for (; i >= 0; --i) {
      if (keep[i] == '.') continue;
      if (keep[i] == '9') { keep[i] = '0'; continue; }
      ++keep[i];
      break;
    }
    if (i < 0) keep.insert(0, "1");
  }
  if (d == 0) keep.pop_back();  // the trailing '.'
  return V::str((x.n < 0 ? "-" : "") + keep);
}

void quote(const std::string& s, std::string& out) {
  static const char kHex[] = "0123456789abcdef";
  out += '"';
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c < 0x20) out += std::string("\\u00") + kHex[c >> 4] + kHex[c & 15];
    else out += ch;
  }
  out += '"';
}

V native_props(const V* pairs, size_t n) {
  std::string out = "{";
  for (size_t i = 0; i < n; ++i) {
    const V& value = pairs[2 * i + 1];
    if (value.k == V::K::None) continue;
    if (out.size() > 1) out += ',';
    quote(pairs[2 * i].s, out);
    out += ':';
    quote(value.k == V::K::Str ? value.s : to_string(value).s, out);
  }
  return V::str(out + "}");
}

namespace {

// The JSON string at `s[i]` (its opening quote), unescaped into `out`; `i`
// ends past the closing quote. False if it is not a whole string.
bool read_string(const std::string& s, size_t& i, std::string& out) {
  if (i >= s.size() || s[i] != '"') return false;
  out.clear();
  for (++i; i < s.size(); ++i) {
    char c = s[i];
    if (c == '"') {
      ++i;
      return true;
    }
    if (c != '\\') {
      out += c;
      continue;
    }
    if (++i >= s.size()) return false;
    switch (s[i]) {
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u':
        // native_props writes \u00XX only, for control characters.
        if (i + 4 >= s.size()) return false;
        out += static_cast<char>(std::strtol(s.substr(i + 3, 2).c_str(), nullptr, 16));
        i += 4;
        break;
      default: out += s[i];
    }
  }
  return false;
}

}  // namespace

bool data_word(const std::string& dataset, const std::string& word, std::string& value) {
  if (dataset.empty() || dataset[0] != '{') return false;
  size_t i = 1;
  std::string key, text;
  while (i < dataset.size() && dataset[i] != '}') {
    if (!read_string(dataset, i, key) || i >= dataset.size() || dataset[i] != ':') return false;
    if (!read_string(dataset, ++i, text)) return false;
    if (key == word) {
      value = text;
      return true;
    }
    if (i < dataset.size() && dataset[i] == ',') ++i;
  }
  return false;
}

void json(const V& v, std::string& out, const Type* types, const Field* fields, int ty) {
  const Type* t = types && ty >= 0 ? &types[ty] : nullptr;
  // some(v) is v itself here (none is None), so it prints as its element type.
  if (t && t->k == Type::K::Option && v.k != V::K::None) return json(v, out, types, fields, t->elem);
  switch (v.k) {
    case V::K::None: out += "null"; break;
    case V::K::Num: out += std::isfinite(v.n) ? number_text(v.n) : "null"; break;
    case V::K::Bool: out += v.b ? "true" : "false"; break;
    case V::K::Str: quote(v.s, out); break;
    case V::K::List:
    case V::K::Rec: {
      const bool record = v.k == V::K::Rec && t && t->k == Type::K::Record && t->field_count == v.rec->size();
      const int elem = v.k == V::K::List && t && t->k == Type::K::List ? t->elem : -1;
      out += record ? '{' : '[';
      for (size_t i = 0; i < v.rec->size(); ++i) {
        if (i) out += ',';
        if (record) {
          const Field& f = fields[t->field_start + i];
          quote(f.name, out);
          out += ':';
          json((*v.rec)[i], out, types, fields, f.ty);
        } else {
          json((*v.rec)[i], out, elem >= 0 ? types : nullptr, fields, elem);
        }
      }
      out += record ? '}' : ']';
      break;
    }
  }
}

}  // namespace rt
