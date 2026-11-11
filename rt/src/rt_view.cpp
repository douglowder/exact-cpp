// View: bindings → node boxes, `when` arms, layout, and paint with dirty
// rectangles. Paint: background, border (each side its own when the box is
// square-cornered; uniform, from the top side, when rounded), radius (a circle
// when it covers a square), images, and text: in the app's faces when its
// font stack names a declared family, else in the board's faces with `°`
// drawn as a ring. Opacity multiplies down the tree; it is applied to each
// shape, glyph, and image, not to the group as one layer. All drawing goes
// through a Canvas on the board's Display, one band of a damaged area at a time.
// A node's CSS animations set its opacity, colors, and translate each frame
// over its own style rows; a translate moves it and its subtree as drawn.
#include "rt_view.h"

#include "rt_animation.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace rt {

namespace {

int ring(float px) { return px > 30 ? 5 : 3; }

bool ends_with(const std::string& s, const char* tail) {
  const size_t n = std::strlen(tail);
  return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

// `#rgb`, `#rrggbb`, `#rrggbbaa`, `transparent`, or one of the app's design tokens, as
// 0xAARRGGBB; else -1.
int64_t parse_color(const V& v, const App& app) {
  if (v.k != V::K::Str) return -1;
  if (v.s == "transparent") return 0;
  if (v.s.empty()) return -1;
  if (v.s[0] != '#') {
    for (uint16_t i = 0; i < app.token_count; ++i) {
      if (v.s == app.tokens[i].name) return app.tokens[i].argb;
    }
    return -1;
  }
  const std::string hex = v.s.substr(1);
  if (hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return -1;
  const uint32_t n = static_cast<uint32_t>(std::strtoul(hex.c_str(), nullptr, 16));
  if (hex.size() == 3) {
    const uint32_t r = (n >> 8) & 15, g = (n >> 4) & 15, b = n & 15;
    return 0xff000000LL | (r * 17) << 16 | (g * 17) << 8 | (b * 17);
  }
  if (hex.size() == 6) return 0xff000000LL | n;
  if (hex.size() == 8) return static_cast<int64_t>((n & 0xff) << 24 | n >> 8);
  return -1;
}

// A number, `Npx`, or `N%`.
Dim dim(const V& v) {
  Dim d;
  if (v.k == V::K::Num) d.v = static_cast<float>(v.n);
  if (v.k == V::K::Str && ends_with(v.s, "%")) {
    d.v = std::strtof(v.s.c_str(), nullptr);
    d.pct = true;
  }
  if (v.k == V::K::Str && ends_with(v.s, "px")) d.v = std::strtof(v.s.c_str(), nullptr);
  return d;
}

float length(const V& v) {
  const Dim d = dim(v);
  return std::isnan(d.v) || d.pct ? 0 : d.v;
}

LineHeight line_height(const V& v) {
  if (v.k == V::K::None) return LineHeight{};
  if (v.k == V::K::Num) return LineHeight{LineHeight::K::Ratio, static_cast<float>(v.n)};
  if (v.k == V::K::Str && ends_with(v.s, "px")) return LineHeight{LineHeight::K::Px, std::strtof(v.s.c_str(), nullptr)};
  return LineHeight{LineHeight::K::Normal, 0};
}

uint8_t fit(const std::string& s) {
  return s == "contain" ? 1 : s == "cover" ? 2 : s == "none" ? 3 : 0;
}

// The CSS weight rule over the weights a family has (plan2cpp's `pick_weight`).
int pick_weight(const std::vector<int>& have, int want) {
  int below = -1, above = -1, up_to_500 = -1;
  for (int w : have) {
    if (w == want) return w;
    if (w < want && w > below) below = w;
    if (w > want && (above < 0 || w < above)) above = w;
    if (w > want && w <= 500 && (up_to_500 < 0 || w < up_to_500)) up_to_500 = w;
  }
  if (want >= 400 && want <= 500) return up_to_500 >= 0 ? up_to_500 : below >= 0 ? below : above;
  if (want < 400) return below >= 0 ? below : above;
  return above >= 0 ? above : below;
}

// Code points of UTF-8 text, without control characters.
std::vector<uint32_t> code_points(const std::string& text) {
  std::vector<uint32_t> out;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text.c_str()); *p;) {
    uint32_t c = *p++;
    int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (more) c &= 0x3fu >> more;
    for (; more > 0 && (*p & 0xc0) == 0x80; --more) c = (c << 6) | (*p++ & 0x3f);
    if (c >= 0x20) out.push_back(c);
  }
  return out;
}

void append_utf8(uint32_t c, std::string& out) {
  if (c < 0x80) {
    out += static_cast<char>(c);
  } else if (c < 0x800) {
    out += static_cast<char>(0xc0 | (c >> 6));
    out += static_cast<char>(0x80 | (c & 0x3f));
  } else if (c < 0x10000) {
    out += static_cast<char>(0xe0 | (c >> 12));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (c & 0x3f));
  } else {
    out += static_cast<char>(0xf0 | (c >> 18));
    out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (c & 0x3f));
  }
}

const Glyph* glyph(const App& app, const Face& f, uint32_t code) {
  const Glyph* first = app.glyphs + f.glyph_start;
  const Glyph* last = first + f.glyph_count;
  const Glyph* g = std::lower_bound(first, last, code, [](const Glyph& a, uint32_t c) { return a.code < c; });
  return g != last && g->code == code ? g : nullptr;
}

// The kerning between two code points in face `f`, in 1/64 px.
int32_t kern(const App& app, const Face& f, uint32_t left, uint32_t right) {
  if (left > 0x7e || right > 0x7e) return 0;
  const FontFile& file = app.font_files[f.file];
  const Kern* first = app.kerns + file.kern_start;
  const Kern* last = first + file.kern_count;
  const uint32_t key = left << 16 | right;
  const Kern* k = std::lower_bound(first, last, key, [](const Kern& a, uint32_t c) {
    return (static_cast<uint32_t>(a.left) << 16 | a.right) < c;
  });
  if (k == last || k->left != left || k->right != right) return 0;
  return static_cast<int32_t>(std::lround(k->units * f.px * 64.0f / file.upem));
}

// The pen advance of text in an app face, in 1/64 px.
int64_t advance(const App& app, const Face& f, const std::vector<uint32_t>& codes) {
  int64_t pen = 0;
  uint32_t prev = 0;
  const Glyph* space = glyph(app, f, 0x20);
  for (uint32_t c : codes) {
    const Glyph* g = glyph(app, f, c);
    if (!g) g = space;
    if (prev) pen += kern(app, f, prev, c);
    if (g) pen += g->advance;
    prev = c;
  }
  return pen;
}

float resolve(const Dim& d, float base) { return d.pct ? d.v / 100.0f * base : d.v; }

bool intersects(Rect a, Rect b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

Rect unite(Rect a, Rect b) {
  int x = std::min(a.x, b.x), y = std::min(a.y, b.y);
  return {x, y, std::max(a.x + a.w, b.x + b.w) - x, std::max(a.y + a.h, b.y + b.h) - y};
}

void apply_style(Box& b, S row, const V& v, const App& app) {
  const bool none = v.k == V::K::None;
  switch (row) {
    case S::Width: b.width = none ? Dim() : dim(v); break;
    case S::Height: b.height = none ? Dim() : dim(v); break;
    case S::Top: b.top = none ? Dim() : dim(v); break;
    case S::Right: b.right = none ? Dim() : dim(v); break;
    case S::Bottom: b.bottom = none ? Dim() : dim(v); break;
    case S::Left: b.left = none ? Dim() : dim(v); break;
    case S::PaddingTop: case S::PaddingRight: case S::PaddingBottom: case S::PaddingLeft:
      b.pad[static_cast<int>(row) - static_cast<int>(S::PaddingTop)] = none ? 0 : length(v);
      break;
    case S::MarginTop: case S::MarginRight: case S::MarginBottom: case S::MarginLeft:
      b.margin[static_cast<int>(row) - static_cast<int>(S::MarginTop)] = none ? 0 : length(v);
      break;
    case S::RowGap: b.row_gap = none ? 0 : length(v); break;
    case S::ColumnGap: b.column_gap = none ? 0 : length(v); break;
    case S::Opacity: b.opacity = none ? 1 : std::min(1.0f, std::max(0.0f, static_cast<float>(v.n))); break;
    case S::LineHeight: b.line_height = line_height(v); break;
    case S::FontFamily: b.family = none ? -1 : static_cast<int>(v.n); break;
    case S::ObjectFit: b.fit = none ? 0 : fit(v.s); break;
    case S::OverflowX: case S::OverflowY: b.clip = !none && v.s != "visible"; break;
    case S::WhiteSpace: b.nowrap = !none && (v.s == "nowrap" || v.s == "pre"); break;
    case S::TextOverflow: b.ellipsis = !none && v.s == "ellipsis"; break;
    case S::Animation: break;  // View::evaluate reads it
    case S::FlexDirection: b.column = v.s == "column"; break;
    case S::JustifyContent: b.justify = v.s == "center" ? 1 : (v.s == "flex-end" || v.s == "end") ? 2 : 0; break;
    case S::AlignItems:
      b.align = v.s == "center" ? 2 : (v.s == "flex-start" || v.s == "start") ? 1 : (v.s == "flex-end" || v.s == "end") ? 3 : 0;
      break;
    case S::PositionType: b.absolute = v.s == "absolute"; break;
    case S::Display: b.flex = v.s == "flex"; b.hidden = v.s == "none"; break;
    case S::BoxSizing: b.border_box = v.s == "border-box"; break;
    case S::BorderWidthTop: case S::BorderWidthRight: case S::BorderWidthBottom: case S::BorderWidthLeft:
      b.border[static_cast<int>(row) - static_cast<int>(S::BorderWidthTop)] = none ? 0 : length(v);
      break;
    case S::BorderStyleTop: case S::BorderStyleRight: case S::BorderStyleBottom: case S::BorderStyleLeft:
      b.border_on[static_cast<int>(row) - static_cast<int>(S::BorderStyleTop)] = !none && v.s != "none" && v.s != "hidden";
      break;
    case S::BackgroundColor: b.bg = none ? -1 : parse_color(v, app); break;
    case S::BorderColorTop: case S::BorderColorRight: case S::BorderColorBottom: case S::BorderColorLeft:
      b.border_color[static_cast<int>(row) - static_cast<int>(S::BorderColorTop)] = none ? -1 : parse_color(v, app);
      break;
    case S::BorderRadiusTopLeft: b.radius = none ? 0 : length(v); break;
    case S::BorderRadiusTopRight: case S::BorderRadiusBottomRight: case S::BorderRadiusBottomLeft: break;
    case S::FontSize: b.font_size = none ? NAN : length(v); break;
    case S::FontWeight: b.weight = none ? 0 : static_cast<int>(v.n); break;
    case S::TextAlign:
      b.text_align = v.s == "center" ? 1 : (v.s == "right" || v.s == "end") ? 2 : none ? -1 : 0;
      break;
    case S::TextColor: b.color = none ? -1 : parse_color(v, app); break;
    case S::Appearance: break;
  }
}

}  // namespace

View::View(Runtime& r, Display* d) : r_(r), d_(d) {}

bool View::begin() { return !d_ || d_->begin(); }

Rect View::clamp(Rect r) const {
  int x = std::max(0, r.x), y = std::max(0, r.y);
  int right = std::min(d_->width(), r.x + r.w), bottom = std::min(d_->height(), r.y + r.h);
  return {x, y, std::max(0, right - x), std::max(0, bottom - y)};
}

// The app face for the node's family, weight, and size (the CSS weight
// rule, then the nearest size), or else the board's face.
TextFace View::face_of(const NodeState& s) const {
  const App& app = r_.app();
  TextFace f;
  const int family = s.family >= 0 && s.family < app.stack_count ? app.stack_family[s.family] : -1;
  if (family >= 0) {
    std::vector<int> weights;
    for (int i = 0; i < app.face_count; ++i) {
      if (app.faces[i].family == family) weights.push_back(app.faces[i].weight);
    }
    const int weight = pick_weight(weights, s.weight);
    for (int i = 0; i < app.face_count; ++i) {
      const Face& c = app.faces[i];
      if (c.family != family || c.weight != weight) continue;
      if (!f.app || std::fabs(c.px - s.font_size) < std::fabs(f.app->px - s.font_size)) f.app = &c;
    }
  }
  if (!f.app && d_) f.board = d_->face(s.font_size, s.weight);
  return f;
}

// The height of one line: `normal` is the face's own (ascent, descent, and
// line gap for an app face; the board face's height), a ratio multiplies the
// font size.
int View::line_box(const NodeState& s, const TextFace& f) const {
  const LineHeight& lh = s.line_height;
  if (lh.k == LineHeight::K::Px) return static_cast<int>(std::lround(lh.v));
  if (lh.k == LineHeight::K::Ratio) return static_cast<int>(std::lround(lh.v * s.font_size));
  if (f.app) return static_cast<int>(std::lround((f.app->ascent + f.app->descent + f.app->leading) / 64.0));
  return d_->line_height(f.board);
}

float View::text_width(const std::string& s, const TextFace& f, float px) const {
  if (f.app) return static_cast<float>(advance(r_.app(), *f.app, code_points(s)) / 64.0);
  float w = 0;
  size_t start = 0;
  while (true) {
    size_t deg = s.find("\xc2\xb0", start);
    std::string piece = s.substr(start, deg == std::string::npos ? std::string::npos : deg - start);
    w += d_->text_width(f.board, piece.c_str());
    if (deg == std::string::npos) break;
    w += ring(px) * 2 + 3;
    start = deg + 2;
  }
  return w;
}

// Lines of `text`: broken at each newline, and between words where a line
// would pass `limit` (NAN: no limit).
std::vector<Line> View::wrap(const std::string& text, const TextFace& f, float px, float limit) const {
  std::vector<Line> lines;
  size_t para = 0;
  while (true) {
    const size_t nl = text.find('\n', para);
    const std::string part = text.substr(para, nl == std::string::npos ? std::string::npos : nl - para);
    std::string current;
    size_t start = 0;
    while (start <= part.size()) {
      size_t space = part.find(' ', start);
      std::string word = part.substr(start, space == std::string::npos ? std::string::npos : space - start);
      std::string candidate = current.empty() ? word : current + " " + word;
      if (current.empty() || std::isnan(limit) || text_width(candidate, f, px) <= limit) {
        current = candidate;
      } else {
        lines.push_back({current, text_width(current, f, px)});
        current = word;
      }
      if (space == std::string::npos) break;
      start = space + 1;
    }
    lines.push_back({current, text_width(current, f, px)});
    if (nl == std::string::npos) break;
    para = nl + 1;
  }
  return lines;
}

const Image* View::image_of(const NodeState& s) const {
  const App& app = r_.app();
  if (const Image* made = r_.image(s.source)) return made;
  std::string source = s.source;
  if (!source.empty() && source[0] == '/') source.erase(0, 1);
  for (int i = 0; i < app.image_count; ++i) {
    if (source == app.images[i].source) return &app.images[i];
  }
  return nullptr;
}

int View::find(const std::string& key) const {
  for (int i = 0; i < count(); ++i) {
    if (nodes_[i].key == key) return i;
  }
  return -1;
}

void View::evaluate(NodeState& s, Ctx& c) {
  const App& app = r_.app();
  const Node& n = app.nodes[s.node];
  for (int k = 0; k < n.binding_count; ++k) {
    const Binding& b = app.bindings[n.binding_start + k];
    V v;
    if (b.fn) {
      v = b.fn(c);
    } else if (b.lit.k == 1) {
      v = V::num(b.lit.n);
    } else if (b.lit.k == 2) {
      v = V::boolean(b.lit.b);
    } else if (b.lit.k == 3) {
      v = V::str(b.lit.s);
    }
    if (!b.target.prop) {
      if (static_cast<S>(b.target.id) == S::Animation) {
        s.anim = animation_value(v);
        continue;
      }
      apply_style(s.box, static_cast<S>(b.target.id), v, app);
      continue;
    }
    const std::string text = v.k == V::K::Str ? v.s : to_string(v).s;
    switch (static_cast<P>(b.target.id)) {
      case P::Text: s.text = text; break;
      case P::TestId: s.test_id = v.s; break;
      case P::Autofocus: s.autofocus = v.b; break;
      case P::Id: s.id = v.s; break;
      case P::NavigationKey: s.nav_key = text; s.has_nav_key = v.k != V::K::None; break;
      case P::NavigationBack: s.nav_back = v.s; s.has_nav_back = v.k != V::K::None; break;
      case P::NavigationPresentation: s.presentation = v.s; break;
      case P::Closedby: s.closedby = v.s; break;
      case P::Role: s.role = v.s; break;
      case P::Controls: s.controls = v.s; break;
      case P::Source: s.source = v.s; break;
      case P::Dataset: s.data = v.s; break;
      case P::Ignored: break;
    }
  }
}

void View::inherit(NodeState& s, const NodeState* parent) {
  s.font_size = !std::isnan(s.box.font_size) ? s.box.font_size : parent ? parent->font_size : 16;
  s.weight = s.box.weight ? s.box.weight : parent ? parent->weight : 400;
  const int64_t text = r_.scheme() == Scheme::Dark ? 0xffffffffLL : 0xff000000LL;
  s.color = s.box.color >= 0 ? s.box.color : parent ? parent->color : text;
  s.text_align = s.box.text_align >= 0 ? s.box.text_align : parent ? parent->text_align : 0;
  s.family = s.box.family >= 0 ? s.box.family : parent ? parent->family : 0;
  s.line_height = s.box.line_height.k != LineHeight::K::Inherit ? s.box.line_height
                  : parent ? parent->line_height : LineHeight{LineHeight::K::Normal, 0};
}

// The arm instance or row at `path`, kept from the last update or created
// now (its owned slots from their initializers).
Scope* View::scope_at(const std::string& path, int arm, Scope* parent, const V& item) {
  auto it = scopes_.find(path);
  bool fresh = it == scopes_.end();
  if (fresh) it = scopes_.emplace(path, Held{std::unique_ptr<Scope>(new Scope()), false}).first;
  Scope* sc = it->second.scope.get();
  it->second.used = true;
  sc->arm = arm;
  sc->parent = parent;
  sc->item = item;
  if (fresh) r_.own(sc);
  return sc;
}

int View::add(int node, Scope* scope, int parent_inst, const std::string& path) {
  NodeState s;
  s.node = node;
  s.scope = scope;
  s.parent = parent_inst;
  s.key = path + "#" + std::to_string(node);
  Ctx c{&r_, nullptr, false, scope};
  evaluate(s, c);
  s.base = s.box;
  inherit(s, parent_inst >= 0 ? &nodes_[parent_inst] : nullptr);
  nodes_.push_back(std::move(s));
  const int i = count() - 1;
  if (parent_inst >= 0) nodes_[parent_inst].kids.push_back(i);
  else roots_.push_back(i);
  return i;
}

namespace {

std::string key_text(const V& v) {
  switch (v.k) {
    case V::K::Num: return "n" + number_text(v.n);
    case V::K::Bool: return v.b ? "t" : "f";
    case V::K::Str: return "s" + v.s;
    default: return "?";
  }
}

}  // namespace

// The children of plan node `parent_node` (or, for -1, the top of arm `arm`)
// as instances under `parent_inst`, regions expanded in their scope.
void View::build(int parent_inst, int parent_node, int arm, Scope* scope, const std::string& path) {
  const App& app = r_.app();
  struct Item {
    uint32_t order;
    int node;    // >= 0: a node
    int region;  // >= 0: a region
  };
  std::vector<Item> items;
  for (int i = 0; i < app.node_count; ++i) {
    const Node& n = app.nodes[i];
    if (parent_node >= 0 ? n.parent == parent_node : (n.parent < 0 && n.arm == arm)) {
      items.push_back({n.order, i, -1});
    }
  }
  for (int g = 0; g < app.region_count; ++g) {
    const Region& r = app.regions[g];
    if (parent_node >= 0 ? r.parent == parent_node : (r.parent < 0 && r.arm == arm)) {
      items.push_back({r.order, -1, g});
    }
  }
  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.order < b.order; });
  for (const Item& it : items) {
    if (it.node >= 0) {
      const int i = add(it.node, scope, parent_inst, path);
      if (!nodes_[i].box.hidden) build(i, it.node, -1, scope, path);
      continue;
    }
    const Region& reg = app.regions[it.region];
    Ctx c{&r_, nullptr, false, scope};
    const std::string at = path + "/" + std::to_string(it.region);
    if (reg.kind == R::When) {
      const bool shown = reg.subject(c).b;
      const int a = shown ? reg.arm_start : (reg.arm_count > 1 ? reg.arm_start + 1 : -1);
      if (a < 0) continue;
      const std::string sp = at + "." + std::to_string(a);
      build(parent_inst, -1, a, scope_at(sp, a, scope, V()), sp);
      continue;
    }
    const V list = reg.subject(c);
    std::vector<std::string> seen;
    for (size_t k = 0; k < list.size(); ++k) {
      const V& item = (*list.rec)[k];
      Scope row;
      row.arm = reg.arm_start;
      row.parent = scope;
      row.item = item;
      Ctx kc{&r_, nullptr, false, &row};
      std::string sp = at + ":" + key_text(reg.key(kc));
      while (std::find(seen.begin(), seen.end(), sp) != seen.end()) sp += "'";
      seen.push_back(sp);
      build(parent_inst, -1, reg.arm_start, scope_at(sp, reg.arm_start, scope, item), sp);
    }
  }
}

void View::update() {
  for (auto& held : scopes_) held.second.used = false;
  nodes_.clear();
  roots_.clear();
  build(-1, -1, -1, &root_, "");
  for (auto it = scopes_.begin(); it != scopes_.end();) {
    it = it->second.used ? std::next(it) : scopes_.erase(it);
  }
  project();
  if (d_) {
    for (int root : roots_) place(root, 0, 0, d_->width(), d_->height());
  }
  start_runs(r_.now_ms());
  animate(r_.now_ms());
}

// The index of `animation` value `v` among the plan's literals, or -1; a
// value that is none of them is logged once and plays nothing.
int View::animation_value(const V& v) {
  if (v.k != V::K::Str) return -1;
  const App& app = r_.app();
  for (uint16_t i = 0; i < app.animation_value_count; ++i) {
    if (v.s == app.animation_values[i].text) return i;
  }
  if (std::find(unplayable_.begin(), unplayable_.end(), v.s) == unplayable_.end()) {
    unplayable_.push_back(v.s);
    r_.log("animation `" + v.s + "` is not one of the plan's literals; it does not play");
  }
  return -1;
}

// Matches each node's animations with those playing on it by name, as CSS
// does (CSS Animations 1 §3): a name already playing keeps its start time,
// and a new one starts now. A paused animation holds its local time.
void View::start_runs(double now_ms) {
  const App& app = r_.app();
  std::map<std::string, std::vector<Run>> next;
  for (const NodeState& s : nodes_) {
    if (s.anim < 0) continue;
    const AnimationValue& value = app.animation_values[s.anim];
    if (value.count == 0) continue;
    std::vector<Run> old;
    auto it = runs_.find(s.key);
    if (it != runs_.end()) old = it->second;
    std::vector<bool> used(old.size(), false);
    std::vector<Run>& runs = next[s.key];
    for (uint16_t k = 0; k < value.count; ++k) {
      const Animation& a = app.animations[value.start + k];
      Run run{a.name, now_ms, 0, a.paused};
      for (size_t o = 0; o < old.size(); ++o) {
        if (used[o] || std::strcmp(old[o].name, a.name) != 0) continue;
        used[o] = true;
        run = old[o];
        break;
      }
      if (a.paused && !run.paused) run.held_ms = now_ms - run.start_ms;
      if (!a.paused && run.paused) run.start_ms = now_ms - run.held_ms;
      run.paused = a.paused;
      runs.push_back(run);
    }
  }
  runs_ = std::move(next);
}

void View::animate(double now_ms) {
  if (runs_.empty()) return;
  for (NodeState& s : nodes_) apply_animation(s, s.parent >= 0 ? &nodes_[s.parent] : nullptr, now_ms);
}

// The node's style rows before animation, then each of its animations over
// them in order (a later one over an earlier one's value), and its rect moved
// by the translate it and its ancestors have now.
void View::apply_animation(NodeState& s, const NodeState* parent, double now_ms) {
  s.box = s.base;
  inherit(s, parent);
  s.dx = parent ? parent->dx : 0;
  s.dy = parent ? parent->dy : 0;
  auto it = s.anim >= 0 ? runs_.find(s.key) : runs_.end();
  if (it != runs_.end()) {
    const App& app = r_.app();
    const AnimationValue& value = app.animation_values[s.anim];
    for (uint16_t k = 0; k < value.count && k < it->second.size(); ++k) {
      const Animation& a = app.animations[value.start + k];
      const Run& run = it->second[k];
      const double t = run.paused ? run.held_ms : now_ms - run.start_ms;
      AValue out;
      AValue under;
      under.v[0] = s.box.opacity;
      if (sample(app, a, t, AP::Opacity, under, out)) s.box.opacity = static_cast<float>(std::min(1.0, std::max(0.0, out.v[0])));
      if (sample(app, a, t, AP::BackgroundColor, color_value(s.box.bg), out)) s.box.bg = color_argb(out);
      if (sample(app, a, t, AP::Color, color_value(s.color), out)) s.color = color_argb(out);
      for (int side = 0; side < 4; ++side) {
        const AP prop = static_cast<AP>(static_cast<int>(AP::BorderTopColor) + side);
        const int64_t now = s.box.border_color[side] >= 0 ? s.box.border_color[side] : s.color;
        if (sample(app, a, t, prop, color_value(now), out)) s.box.border_color[side] = color_argb(out);
      }
      if (sample(app, a, t, AP::Translate, AValue{}, out)) {
        s.dx += out.v[0] + out.v[2] * s.layout.w / 100.0;
        s.dy += out.v[1] + out.v[3] * s.layout.h / 100.0;
      }
    }
  }
  s.rect = s.layout;
  s.rect.x += static_cast<int>(std::lround(s.dx));
  s.rect.y += static_cast<int>(std::lround(s.dy));
}

int View::nav_root() const {
  for (int i = 0; i < count(); ++i) {
    if (nodes_[i].has_nav_key && nodes_[i].has_nav_back) return i;
  }
  return -1;
}

std::vector<int> View::routes_of(const std::vector<int>& ids) const {
  std::vector<int> out;
  for (int i : ids) {
    if (nodes_[i].has_nav_key) out.push_back(i);
  }
  return out;
}

// A navigation root's tabpanels, in tab order: those its own tablist's tabs
// name with aria-controls (the runner's `tab_panels`).
std::vector<int> View::tab_panels(const std::vector<int>& kids) const {
  std::vector<std::vector<int>> lists;
  std::vector<int> panels, stack(kids.rbegin(), kids.rend());
  while (!stack.empty()) {
    const int i = stack.back();
    stack.pop_back();
    const NodeState& s = nodes_[i];
    if (s.role == "tabpanel") panels.push_back(i);
    if (s.role == "tablist") lists.push_back(s.kids);
    if (!s.has_nav_key && s.role != "tablist") stack.insert(stack.end(), s.kids.rbegin(), s.kids.rend());
  }
  for (const std::vector<int>& tabs : lists) {
    std::vector<int> named;
    for (int t : tabs) {
      if (nodes_[t].role != "tab" || nodes_[t].controls.empty()) continue;
      for (int p : panels) {
        if (nodes_[p].id == nodes_[t].controls) {
          named.push_back(p);
          break;
        }
      }
    }
    if (!named.empty()) return named;
  }
  return {};
}

std::vector<int> View::selected_stack(int root, int* selected) const {
  *selected = -1;
  const NodeState& at = nodes_[root];
  const std::vector<int> panels = tab_panels(at.kids);
  std::vector<std::vector<int>> stacks;
  if (panels.empty()) stacks.push_back(routes_of(at.kids));
  for (int p : panels) stacks.push_back(routes_of(nodes_[p].kids));
  for (const std::vector<int>& rs : stacks) {
    for (size_t k = 0; k < rs.size(); ++k) {
      if (nodes_[rs[k]].nav_key == at.nav_key) {
        *selected = static_cast<int>(k);
        return rs;
      }
    }
  }
  return {};
}

void View::mark(int i, bool hide) {
  nodes_[i].inactive = true;
  if (hide) nodes_[i].route_hidden = true;
  for (int k : nodes_[i].kids) mark(k, hide);
}

// What each navigation root leaves unselected (the runner's `unselected`,
// host/web/navigation.js `project`): every panel but the selected one, and
// the selected stack's other routes. All of it is inactive; all of it is
// hidden but the route directly under a selected modal route.
void View::project() {
  for (int root = 0; root < count(); ++root) {
    const NodeState& at = nodes_[root];
    if (!at.has_nav_key || !at.has_nav_back) continue;
    int selected = -1;
    const std::vector<int> stack = selected_stack(root, &selected);
    if (selected < 0) continue;
    for (int p : tab_panels(at.kids)) {
      if (std::find(nodes_[p].kids.begin(), nodes_[p].kids.end(), stack[selected]) == nodes_[p].kids.end()) {
        mark(p, true);
      }
    }
    const bool modal = nodes_[stack[selected]].presentation == "modal";
    for (int k = 0; k < static_cast<int>(stack.size()); ++k) {
      if (k != selected) mark(stack[k], !(modal && k == selected - 1));
    }
  }
}

// The border-box size of node i when its parent's box offers `avail_w` by
// `avail_h` (NAN: not known). An unset width or height is the content's:
// text lines, an image's own pixels, or the flow children.
void View::measure(int i, float avail_w, float avail_h, float& w, float& h) {
  NodeState& s = nodes_[i];
  const Box& b = s.box;
  float extra_w = b.border[1] + b.border[3] + b.pad[1] + b.pad[3];
  float extra_h = b.border[0] + b.border[2] + b.pad[0] + b.pad[2];
  auto side = [](const Dim& d, float avail, float extra, bool border_box) {
    if (std::isnan(d.v) || (d.pct && std::isnan(avail))) return NAN;
    return resolve(d, avail) - (border_box ? extra : 0);
  };
  float cw = side(b.width, avail_w, extra_w, b.border_box);
  float ch = side(b.height, avail_h, extra_h, b.border_box);
  const T type = r_.app().nodes[s.node].type;
  if (std::isnan(cw) || std::isnan(ch)) {
    float iw = 0, ih = 0;
    if (type == T::Text) {
      const TextFace f = face_of(s);
      // Unset width: as wide as the text, up to the room the parent offers.
      const float room = std::isnan(avail_w) ? NAN : avail_w - extra_w - b.margin[1] - b.margin[3];
      auto lines = wrap(s.text, f, s.font_size, s.box.nowrap ? NAN : std::isnan(cw) ? room : cw);
      // Whole pixels, rounded up, so the box holds its own widest line.
      for (auto& l : lines) iw = std::max(iw, std::ceil(l.width));
      ih = static_cast<float>(lines.size() * line_box(s, f));
    } else if (type == T::Image) {
      if (const Image* img = image_of(s)) {
        iw = img->w;
        ih = img->h;
        if (!std::isnan(cw) && img->w) ih = cw * img->h / img->w;
        if (!std::isnan(ch) && img->h) iw = ch * img->w / img->h;
      }
    } else {
      content(i, cw, ch, iw, ih);
    }
    if (std::isnan(cw)) cw = iw;
    if (std::isnan(ch)) ch = ih;
  }
  w = std::max(0.0f, cw) + extra_w;
  h = std::max(0.0f, ch) + extra_h;
}

// The size node i's flow children take in a content box of `cw` by `ch`.
void View::content(int i, float cw, float ch, float& w, float& h) {
  const Box& b = nodes_[i].box;
  const bool row = b.flex && !b.column;
  const float gap = row ? b.column_gap : b.row_gap;
  float main = 0, cross = 0;
  int n = 0;
  const std::vector<int> kids = nodes_[i].kids;
  for (int child : kids) {
    const Box& cb = nodes_[child].box;
    if (cb.hidden || cb.absolute) continue;
    float kw, kh;
    measure(child, cw, ch, kw, kh);
    kw += cb.margin[1] + cb.margin[3];
    kh += cb.margin[0] + cb.margin[2];
    main += (row ? kw : kh) + (n++ ? gap : 0);
    cross = std::max(cross, row ? kh : kw);
  }
  w = row ? main : cross;
  h = row ? cross : main;
}

void View::place(int i, float x, float y, float w, float h) {
  NodeState& s = nodes_[i];
  s.rect = {static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)),
            static_cast<int>(std::lround(w)), static_cast<int>(std::lround(h))};
  s.layout = s.rect;
  if (!s.box.hidden) place_children(i);
}

// Absolute children sit at their insets (and margins) in the padding box;
// flow children stack along the flex axis (or down, for a block), with their
// margins, the gap between them, and justify and align.
void View::place_children(int i) {
  const NodeState& s = nodes_[i];
  const Box& b = s.box;
  float px = s.rect.x + b.border[3], py = s.rect.y + b.border[0];
  float pw = s.rect.w - b.border[1] - b.border[3], ph = s.rect.h - b.border[0] - b.border[2];
  float cx = px + b.pad[3], cy = py + b.pad[0];
  float cw = pw - b.pad[1] - b.pad[3], chh = ph - b.pad[0] - b.pad[2];
  std::vector<int> flow;
  const std::vector<int> kids = nodes_[i].kids;
  for (int child : kids) {
    const Box& cb = nodes_[child].box;
    if (cb.hidden) continue;
    if (!cb.absolute) {
      flow.push_back(child);
      continue;
    }
    float w, h;
    measure(child, pw, ph, w, h);
    const float* m = cb.margin;
    float x = !std::isnan(cb.left.v) ? px + resolve(cb.left, pw) + m[3]
              : !std::isnan(cb.right.v) ? px + pw - resolve(cb.right, pw) - w - m[1] : cx + m[3];
    float y = !std::isnan(cb.top.v) ? py + resolve(cb.top, ph) + m[0]
              : !std::isnan(cb.bottom.v) ? py + ph - resolve(cb.bottom, ph) - h - m[2] : cy + m[0];
    place(child, x, y, w, h);
  }
  const bool row = b.flex && !b.column;
  const float gap = row ? b.column_gap : b.row_gap;
  std::vector<std::pair<float, float>> sizes;
  float total = 0;
  for (int child : flow) {
    float w, h;
    measure(child, cw, chh, w, h);
    const Box& cb = nodes_[child].box;
    const bool stretch = (!b.flex || b.align == 0);
    if (stretch && !row && std::isnan(cb.width.v)) w = cw - cb.margin[1] - cb.margin[3];
    if (stretch && row && std::isnan(cb.height.v)) h = chh - cb.margin[0] - cb.margin[2];
    sizes.push_back({w, h});
    total += row ? w + cb.margin[1] + cb.margin[3] : h + cb.margin[0] + cb.margin[2];
  }
  if (!flow.empty()) total += gap * static_cast<float>(flow.size() - 1);
  const float space = (row ? cw : chh) - total;
  float main = (row ? cx : cy) + (b.justify == 1 ? space / 2 : b.justify == 2 ? space : 0);
  for (size_t k = 0; k < flow.size(); ++k) {
    const float* m = nodes_[flow[k]].box.margin;
    float w = sizes[k].first, h = sizes[k].second;
    float cross_space = row ? chh - h - m[0] - m[2] : cw - w - m[1] - m[3];
    float cross = (b.align == 2 ? cross_space / 2 : b.align == 3 ? cross_space : 0);
    if (row) {
      main += m[3];
      place(flow[k], main, cy + m[0] + cross, w, h);
      main += w + m[1] + gap;
    } else {
      main += m[0];
      place(flow[k], cx + m[3] + cross, main, w, h);
      main += h + m[2] + gap;
    }
  }
}

// What a node draws, hashed: rect, colors, border, radius, text, text style.
static uint64_t sign(const NodeState& s) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
  mix(static_cast<uint32_t>(s.rect.x)); mix(static_cast<uint32_t>(s.rect.y));
  mix(static_cast<uint32_t>(s.rect.w)); mix(static_cast<uint32_t>(s.rect.h));
  mix(static_cast<uint64_t>(s.box.bg));
  for (int k = 0; k < 4; ++k) {
    mix(static_cast<uint64_t>(s.box.border_color[k])); mix(static_cast<uint64_t>(s.box.border[k] * 16));
    mix(s.box.border_on[k]);
  }
  mix(static_cast<uint64_t>(s.box.radius * 16)); mix(static_cast<uint64_t>(s.color));
  mix(static_cast<uint64_t>(s.font_size * 16)); mix(static_cast<uint32_t>(s.weight));
  mix(static_cast<uint32_t>(s.text_align)); mix(std::hash<std::string>{}(s.text));
  mix(static_cast<uint64_t>(s.box.opacity * 255)); mix(static_cast<uint32_t>(s.family));
  mix(static_cast<uint32_t>(s.line_height.k)); mix(static_cast<uint64_t>(s.line_height.v * 16));
  mix(std::hash<std::string>{}(s.source)); mix(s.box.fit); mix(s.box.clip);
  return h;
}

std::vector<Rect> View::damage() {
  std::vector<Rect> rects;
  hits_.clear();
  pans_.clear();
  std::map<std::string, std::pair<uint64_t, Rect>> now;
  for (int i = 0; i < count(); ++i) {
    const NodeState& s = nodes_[i];
    if (s.box.hidden || s.route_hidden) continue;
    const uint64_t sig = sign(s);
    now[s.key] = {sig, s.rect};
    auto old = painted_.find(s.key);
    if (old == painted_.end() || old->second.first != sig) {
      if (old != painted_.end()) rects.push_back(old->second.second);
      rects.push_back(s.rect);
    }
    if (!s.inactive && r_.app().nodes[s.node].type == T::Pressable) hits_.push_back({i, s.rect});
    if (!s.inactive) {
      const Node& n = r_.app().nodes[s.node];
      for (int k = 0; k < n.handler_count; ++k) {
        if (r_.app().handlers[n.handler_start + k].event == E::Pan) pans_.push_back({i, s.rect});
      }
    }
  }
  for (const auto& old : painted_) {
    if (!now.count(old.first)) rects.push_back(old.second.second);
  }
  painted_ = std::move(now);
  if (first_) {
    first_ = false;
    return {Rect{0, 0, d_->width(), d_->height()}};
  }
  std::vector<Rect> merged;
  int64_t area = 0;
  for (Rect r : rects) {
    r = clamp(Rect{r.x - 1, r.y - 1, r.w + 2, r.h + 2});
    if (r.w <= 0 || r.h <= 0) continue;
    for (size_t k = 0; k < merged.size();) {
      if (intersects(merged[k], r)) {
        r = unite(r, merged[k]);
        merged.erase(merged.begin() + k);
        k = 0;
      } else {
        ++k;
      }
    }
    merged.push_back(r);
  }
  for (Rect r : merged) area += static_cast<int64_t>(r.w) * r.h;
  if (merged.size() > 8 || area * 10 > static_cast<int64_t>(d_->width()) * d_->height() * 6) {
    return {Rect{0, 0, d_->width(), d_->height()}};
  }
  return merged;
}

namespace {

uint32_t rgb_of(int64_t argb) { return static_cast<uint32_t>(argb & 0xffffff); }

// A color's alpha times the node's opacity, 0–255.
uint8_t alpha_of(int64_t argb, unsigned alpha) {
  return static_cast<uint8_t>(((argb >> 24) & 0xff) * alpha / 255);
}

Rect meet(Rect a, Rect b) {
  int x = std::max(a.x, b.x), y = std::max(a.y, b.y);
  int right = std::min(a.x + a.w, b.x + b.w), bottom = std::min(a.y + a.h, b.y + b.h);
  return {x, y, std::max(0, right - x), std::max(0, bottom - y)};
}

}  // namespace

void View::paint_box(const NodeState& s, Canvas& canvas, unsigned alpha) {
  const Box& b = s.box;
  const Rect r = s.rect;
  const int corner = std::min(static_cast<int>(std::lround(b.radius)), std::min(r.w, r.h) / 2);
  const bool circle = corner * 2 >= r.w && r.w == r.h && corner > 0;
  if (b.bg >= 0 && alpha_of(b.bg, alpha)) {
    const uint8_t a = alpha_of(b.bg, alpha);
    if (circle) canvas.fill_circle(r.x + r.w / 2, r.y + r.h / 2, r.w / 2, rgb_of(b.bg), a);
    else canvas.fill_round_rect(r, corner, rgb_of(b.bg), a);
  }
  auto color = [&](int k) { return b.border_color[k] >= 0 ? b.border_color[k] : s.color; };
  if (corner > 0) {
    const int width = b.border_on[0] ? static_cast<int>(std::lround(b.border[0])) : 0;
    const uint8_t a = alpha_of(color(0), alpha);
    for (int k = 0; k < width && a; ++k) {
      if (circle) canvas.draw_circle(r.x + r.w / 2, r.y + r.h / 2, r.w / 2 - k, rgb_of(color(0)), a);
      else canvas.draw_round_rect(Rect{r.x + k, r.y + k, r.w - 2 * k, r.h - 2 * k}, corner > k ? corner - k : 0, rgb_of(color(0)), a);
    }
    return;
  }
  int w[4];
  for (int k = 0; k < 4; ++k) w[k] = b.border_on[k] ? static_cast<int>(std::lround(b.border[k])) : 0;
  const Rect sides[4] = {
      {r.x, r.y, r.w, w[0]},
      {r.x + r.w - w[1], r.y + w[0], w[1], r.h - w[0] - w[2]},
      {r.x, r.y + r.h - w[2], r.w, w[2]},
      {r.x, r.y + w[0], w[3], r.h - w[0] - w[2]},
  };
  for (int k = 0; k < 4; ++k) {
    if (w[k] > 0) canvas.fill_rect(sides[k], rgb_of(color(k)), alpha_of(color(k), alpha));
  }
}

void View::paint_image(const NodeState& s, Canvas& canvas, unsigned alpha) {
  const Image* img = image_of(s);
  if (!img || !img->w || !img->h) return;
  const Box& b = s.box;
  const Rect r = s.rect;
  const Rect box{r.x + static_cast<int>(b.border[3] + b.pad[3]), r.y + static_cast<int>(b.border[0] + b.pad[0]),
                 r.w - static_cast<int>(b.border[1] + b.border[3] + b.pad[1] + b.pad[3]),
                 r.h - static_cast<int>(b.border[0] + b.border[2] + b.pad[0] + b.pad[2])};
  Rect dst = box;
  if (b.fit != 0) {
    // contain, cover, none: the image keeps its aspect, centered in the box.
    float scale = 1;
    const float sx = static_cast<float>(box.w) / img->w, sy = static_cast<float>(box.h) / img->h;
    if (b.fit == 1) scale = std::min(sx, sy);
    if (b.fit == 2) scale = std::max(sx, sy);
    dst.w = static_cast<int>(std::lround(img->w * scale));
    dst.h = static_cast<int>(std::lround(img->h * scale));
    dst.x = box.x + (box.w - dst.w) / 2;
    dst.y = box.y + (box.h - dst.h) / 2;
  }
  const int corner = std::min(static_cast<int>(std::lround(b.radius)), std::min(r.w, r.h) / 2);
  canvas.image(dst, *img, box, corner, static_cast<uint8_t>(alpha));
}

void View::paint_node(int i, Canvas& canvas, unsigned alpha) {
  const NodeState& s = nodes_[i];
  if (s.box.hidden || s.route_hidden) return;
  alpha = static_cast<unsigned>(std::lround(alpha * s.box.opacity));
  if (alpha == 0) return;
  const Rect r = s.rect;
  // A circle of radius w/2 around the center of an even-sized box reaches one
  // pixel past it, so the box's ink is one pixel wider and taller.
  const Rect ink{r.x - 2, r.y - 2, r.w + 5, r.h + 5};
  if (meet(ink, canvas.clip()).w > 0 && meet(ink, canvas.clip()).h > 0) {
    paint_box(s, canvas, alpha);
    const T type = r_.app().nodes[s.node].type;
    if (type == T::Image) paint_image(s, canvas, alpha);
    if (type == T::Text && !s.text.empty()) paint_text(s, canvas, alpha);
  }
  if (!s.box.clip) {
    for (int child : s.kids) paint_node(child, canvas, alpha);
    return;
  }
  Canvas inner(*d_, meet(canvas.clip(), r), false);
  if (inner.clip().w <= 0 || inner.clip().h <= 0) return;
  for (int child : s.kids) paint_node(child, inner, alpha);
}

void View::paint_text(const NodeState& s, Canvas& canvas, unsigned alpha) {
  const Box& b = s.box;
  const Rect r = s.rect;
  const TextFace f = face_of(s);
  const uint32_t color = rgb_of(s.color);
  const uint8_t a = alpha_of(s.color, alpha);
  if (!a) return;
  float left = r.x + b.border[3] + b.pad[3];
  float inner = r.w - b.border[1] - b.border[3] - b.pad[1] - b.pad[3];
  const int lh = line_box(s, f);
  float top = r.y + b.border[0] + b.pad[0];
  const App& app = r_.app();
  std::vector<Line> lines = wrap(s.text, f, s.font_size, b.nowrap ? NAN : inner);
  // text-overflow: ellipsis cuts a clipped line that does not fit, by code
  // point, and ends it with "…".
  if (b.ellipsis && b.clip) {
    for (Line& l : lines) {
      if (l.width <= inner) continue;
      std::vector<uint32_t> codes = code_points(l.text);
      std::string cut;
      while (!codes.empty()) {
        codes.pop_back();
        cut.clear();
        for (uint32_t c : codes) append_utf8(c, cut);
        cut += "\xe2\x80\xa6";
        if (text_width(cut, f, s.font_size) <= inner) break;
      }
      l.text = cut;
      l.width = text_width(cut, f, s.font_size);
    }
  }
  for (const Line& l : lines) {
    float x = s.text_align == 1 ? left + (inner - l.width) / 2 : s.text_align == 2 ? left + inner - l.width : left;
    int y = static_cast<int>(std::lround(top));
    if (f.app) {
      // The baseline sits half the line's leading below the line box's top,
      // plus the ascent, as CSS places it.
      const double asc = f.app->ascent / 64.0, desc = f.app->descent / 64.0;
      const int base = y + static_cast<int>(std::lround((lh - (asc + desc)) / 2 + asc));
      int64_t pen = std::llround(x * 64);
      uint32_t prev = 0;
      const Glyph* space = glyph(app, *f.app, 0x20);
      for (uint32_t c : code_points(l.text)) {
        const Glyph* g = glyph(app, *f.app, c);
        if (prev) pen += kern(app, *f.app, prev, c);
        prev = c;
        if (!g) {
          if (space) pen += space->advance;
          continue;
        }
        const int gx = static_cast<int>((pen + 32) >> 6) + g->left;
        canvas.mask4(gx, base - g->top, g->w, g->h, app.glyph_bits + g->bits, color, a);
        pen += g->advance;
      }
      top += lh;
      continue;
    }
    // A board face: its line box is centered in the line height.
    y += (lh - d_->line_height(f.board)) / 2;
    int cursor = static_cast<int>(std::lround(x));
    size_t start = 0;
    while (true) {
      size_t deg = l.text.find("\xc2\xb0", start);
      std::string piece = l.text.substr(start, deg == std::string::npos ? std::string::npos : deg - start);
      if (!piece.empty()) d_->draw_text(f.board, piece.c_str(), cursor, y, color);
      cursor += d_->text_width(f.board, piece.c_str());
      if (deg == std::string::npos) break;
      int rr = ring(s.font_size);
      for (int k = 0; k < 1 + rr / 4; ++k) d_->draw_circle(cursor + rr + 1, y + lh / 4, rr - k, color);
      cursor += rr * 2 + 3;
      start = deg + 2;
    }
    top += lh;
  }
}

void View::paint(Rect clip, bool whole) {
  d_->fill_rect(clip, r_.scheme() == Scheme::Dark ? 0x000000 : 0xffffff);
  Canvas canvas(*d_, clip, whole);
  for (int root : roots_) paint_node(root, canvas, 255);
}

int64_t View::present(int64_t& paint_us, int64_t& push_us) {
  if (!d_) return 0;
  std::vector<Rect> rects = damage();
  if (rects.empty()) return 0;
  Board& board = r_.board();
  const int band = std::max(1, d_->band_rows());
  int64_t area = 0;
  paint_us = 0;
  push_us = 0;
  for (Rect r : rects) {
    for (int y = r.y; y < r.y + r.h; y += band) {
      Rect part{r.x, y, r.w, std::min(band, r.y + r.h - y)};
      int64_t t0 = board.micros();
      d_->begin_area(part);
      paint(part, true);
      int64_t t1 = board.micros();
      d_->push_area();
      paint_us += t1 - t0;
      push_us += board.micros() - t1;
    }
    area += static_cast<int64_t>(r.w) * r.h;
  }
  return area;
}

}  // namespace rt
