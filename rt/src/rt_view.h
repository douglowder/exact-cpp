// The view half of exact-rt: node instances from bindings, `when` arms
// and `each` rows, route projection, layout, paint with dirty rectangles, and
// press targets. Without a display (a headless board) it keeps instance state
// only: no layout, paint, or targets.
#pragma once

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "exact_rt.h"
#include "rt_canvas.h"

namespace rt {

struct Dim {
  float v = NAN;  // NAN: unset
  bool pct = false;
};

// A line height: `normal` (the face's own), a multiple of the font size, or px.
struct LineHeight {
  enum class K : uint8_t { Inherit, Normal, Ratio, Px };
  K k = K::Inherit;
  float v = 0;
};

// The style rows the subset admits, resolved for one node. Colors are
// 0xAARRGGBB, or -1 for none (current color, inherit).
struct Box {
  Dim width, height, top, right, bottom, left;
  float pad[4] = {0, 0, 0, 0};     // top, right, bottom, left
  float margin[4] = {0, 0, 0, 0};  // same order
  float border[4] = {0, 0, 0, 0};  // widths, same order
  bool border_on[4] = {false, false, false, false};
  int64_t border_color[4] = {-1, -1, -1, -1};
  float radius = 0;
  float row_gap = 0, column_gap = 0;
  float opacity = 1;
  bool absolute = false, flex = false, column = false, border_box = false, hidden = false, clip = false;
  bool nowrap = false;    // white-space: nowrap (inherited; kept per box here)
  bool ellipsis = false;  // text-overflow: ellipsis, with overflow clipped
  uint8_t justify = 0;  // 0 start, 1 center, 2 end
  uint8_t align = 0;    // 0 stretch, 1 start, 2 center, 3 end
  uint8_t fit = 0;      // object-fit: 0 fill, 1 contain, 2 cover, 3 none
  int64_t bg = -1, color = -1;
  float font_size = NAN;  // NAN: inherit
  int weight = 0;         // 0: inherit
  int family = -1;        // a font stack; -1: inherit
  LineHeight line_height;
  int text_align = -1;  // -1 inherit; 0 left, 1 center, 2 right
};

// The face text is drawn in: one of the app's faces, or else the board's.
struct TextFace {
  const Face* app = nullptr;
  int board = 0;
};

// One node instance: a plan node in one arm instance or `each` row.
struct NodeState {
  int node = -1;          // the plan node
  Scope* scope = nullptr;
  int parent = -1;        // the parent instance, through regions
  std::vector<int> kids;  // child instances, in order, regions expanded
  std::string key;        // stable across updates: scope path and node
  Box box;
  std::string text, test_id, id, source;
  // The `data-*` words, as native_props writes them; empty with none.
  std::string data;
  // Navigation props (LLP 1038 D6) and the accessibility props tabs use.
  std::string nav_key, nav_back, presentation, closedby, role, controls;
  bool has_nav_key = false, has_nav_back = false;
  bool autofocus = false;
  // Under a route its navigation root has not selected: takes no presses,
  // focus, or agent taps (the runner's `inactive`).
  bool inactive = false;
  // Not painted: an unselected route or panel, but the route under a
  // selected modal route.
  bool route_hidden = false;
  // Where it draws: its layout box, moved by its own and its ancestors'
  // animated translate.
  Rect rect{0, 0, 0, 0};
  Rect layout{0, 0, 0, 0};
  // Its `animation` value (an index of `App::animation_values`, or -1), and
  // its style rows before animation.
  int anim = -1;
  Box base;
  double dx = 0, dy = 0;
  // Inherited text style, resolved down the tree.
  float font_size = 16;
  int weight = 400, text_align = 0, family = 0;
  int64_t color = 0xff000000;
  LineHeight line_height{LineHeight::K::Normal, 0};
};

struct Line {
  std::string text;
  float width;
};

class View {
 public:
  View(Runtime& r, Display* d);
  bool begin();
  // Rebuilds the instances from bindings, live arms and rows, projects the
  // routes, and lays out (with a display). Arm instances and rows that no
  // longer show are dropped with their slots.
  void update();
  // Applies each running animation at `now_ms` (the runtime's clock) over its
  // node's own style rows.
  void animate(double now_ms);
  // Paints what changed since the last call and pushes it; returns pixels painted.
  int64_t present(int64_t& paint_us, int64_t& push_us);
  const std::vector<std::pair<int, Rect>>& hits() const { return hits_; }
  // Active instances with a pan handler, in paint order.
  const std::vector<std::pair<int, Rect>>& pans() const { return pans_; }
  const NodeState& node(int i) const { return nodes_[i]; }
  int count() const { return static_cast<int>(nodes_.size()); }
  // The instance whose key is `key`, or -1.
  int find(const std::string& key) const;
  // The navigation root instance (one with navigationKey and navigationBack),
  // or -1.
  int nav_root() const;
  // The routes of navigation root `root` in its selected stack, root first;
  // `selected` is the index of the route its key names, or -1.
  std::vector<int> selected_stack(int root, int* selected) const;
  Scope& root_scope() { return root_; }

 private:
  struct Held {
    std::unique_ptr<Scope> scope;
    bool used;
  };
  void build(int parent_inst, int parent_node, int arm, Scope* scope, const std::string& path);
  int add(int node, Scope* scope, int parent_inst, const std::string& path);
  Scope* scope_at(const std::string& path, int arm, Scope* parent, const V& item);
  // One animation playing on a node: started at `start_ms`; a paused one
  // holds local time `held_ms`.
  struct Run {
    const char* name;
    double start_ms;
    double held_ms;
    bool paused;
  };
  void evaluate(NodeState& s, Ctx& c);
  int animation_value(const V& v);
  void start_runs(double now_ms);
  void apply_animation(NodeState& s, const NodeState* parent, double now_ms);
  void inherit(NodeState& s, const NodeState* parent);
  void project();
  void mark(int i, bool hide);
  std::vector<int> routes_of(const std::vector<int>& ids) const;
  std::vector<int> tab_panels(const std::vector<int>& kids) const;
  void measure(int i, float avail_w, float avail_h, float& w, float& h);
  void content(int i, float cw, float ch, float& w, float& h);
  void place(int i, float x, float y, float w, float h);
  void place_children(int i);
  std::vector<Rect> damage();
  void paint(Rect clip, bool whole);
  void paint_node(int i, Canvas& canvas, unsigned alpha);
  void paint_box(const NodeState& s, Canvas& canvas, unsigned alpha);
  void paint_text(const NodeState& s, Canvas& canvas, unsigned alpha);
  void paint_image(const NodeState& s, Canvas& canvas, unsigned alpha);
  TextFace face_of(const NodeState& s) const;
  int line_box(const NodeState& s, const TextFace& f) const;
  float text_width(const std::string& s, const TextFace& f, float px) const;
  std::vector<Line> wrap(const std::string& text, const TextFace& f, float px, float limit) const;
  const Image* image_of(const NodeState& s) const;
  Rect clamp(Rect r) const;

  Runtime& r_;
  Display* d_;
  Scope root_;
  std::map<std::string, Held> scopes_;
  std::vector<NodeState> nodes_;
  std::vector<int> roots_;
  // What each painted instance last drew, by key: its signature and rect.
  std::map<std::string, std::pair<uint64_t, Rect>> painted_;
  // The animations playing on each node, by its key, in its value's order.
  std::map<std::string, std::vector<Run>> runs_;
  std::vector<std::string> unplayable_;  // values logged as not in the plan's literals
  std::vector<std::pair<int, Rect>> hits_, pans_;
  bool first_ = true;
};

}  // namespace rt
