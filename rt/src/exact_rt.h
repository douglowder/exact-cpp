// exact-rt: the runtime for Exact apps compiled to C++ by plan2cpp (Plan B).
// The generated app_gen.cpp provides `app::kApp`: one function per code body
// and constant tables. This runtime gives those functions their values and
// effects, lays out and paints the view, and runs events and timers, with the
// semantics of Exact's runner for the subset plan2cpp admits.
//
// Nothing here knows the board. A board implements `Board` and gives the parts
// it has (`Display`, `Link`, `Accelerometer`, …); `Host` runs an app on it and
// reports at boot what the app needs that the board lacks. Standard C++17 only.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rt_route.h"

namespace rt {

// An Exact value: number, bool, string, record (positional fields), list,
// or none. A record's fields and a list's items are both held in `rec`.
struct V {
  enum class K : uint8_t { None, Num, Bool, Str, Rec, List };
  K k = K::None;
  double n = 0;
  bool b = false;
  std::string s;
  std::shared_ptr<const std::vector<V>> rec;

  static V num(double v);
  static V boolean(bool v);
  static V str(const char* v);
  static V str(std::string v);
  static V record(std::vector<V> fields);
  static V list(std::vector<V> items);
  V field(uint64_t i) const;
  size_t size() const { return rec ? rec->size() : 0; }
};

V add(const V& a, const V& b);
V sub(const V& a, const V& b);
V mul(const V& a, const V& b);
V div(const V& a, const V& b);
V rem(const V& a, const V& b);
V eq(const V& a, const V& b);
V ne(const V& a, const V& b);
V lt(const V& a, const V& b);
V le(const V& a, const V& b);
V gt(const V& a, const V& b);
V ge(const V& a, const V& b);
V concat(const V& a, const V& b);
V max(const V& a, const V& b);
V min(const V& a, const V& b);
V floor(const V& a);
V ceil(const V& a);
V round(const V& a);
V to_fixed(const V& x, const V& digits);
V to_string(const V& a);
bool equal(const V& a, const V& b);
// JavaScript's Number::toString, as the runner prints numbers.
std::string number_text(double x);
// A plan type, for the agent's typed JSON: records print as objects keyed by
// field name, as the runner's `typed_json`.
struct Type {
  enum class K : uint8_t { Other, List, Record, Option } k;
  int16_t elem;  // a list's or option's element type, or -1
  uint16_t field_start, field_count;
};

struct Field {
  const char* name;
  uint16_t ty;
};

// The value as the runner's agent JSON prints it under type `ty` (or
// positionally when `types` is null or the value does not fit the type).
void json(const V& v, std::string& out, const Type* types, const Field* fields, int ty);
// A JSON string, escaped as the runner's agent `quote`.
void quote(const std::string& s, std::string& out);
// A node's `data-*` words (LLP 1075.003 §3.3) from `n` key and value pairs
// in key order, as the runner's NativeProps: a JSON object of strings (a
// number as JavaScript prints it); a none value leaves its key out.
V native_props(const V* pairs, size_t n);
// The value of `word` in a dataset object from `native_props`; false if the
// object does not have it.
bool data_word(const std::string& dataset, const std::string& word, std::string& value);

class Runtime;

// An arm instance: a `when` arm that shows, or one row of an `each`. It is a
// region frame for the code inside it (LoadItem counts frames from the
// innermost), and it holds the slots its arm owns (LLP 1017 P4c), created
// with it and dropped with it.
struct Scope {
  int arm = -1;  // the plan arm; -1 for the root
  Scope* parent = nullptr;
  V item;        // an `each` row's item
  std::vector<std::pair<uint16_t, V>> slots;
};

// What a code body reads and does. Writes and commands are held for the commit.
struct Ctx {
  Runtime* r;
  const V* params;
  bool in_action;
  Scope* scope;
  V slot(uint64_t i);
  V derive(uint64_t i);
  V resource(uint64_t i);
  V param(uint64_t i);
  // The item of the region frame `depth` frames out from the innermost.
  V item(uint64_t depth);
  void write(uint64_t i, const V& v);
  void command(const char* name, const V* args, int argc);
  // A route standard function (open … encodeRouteSegment) on `argc` args.
  V call(uint8_t fn, const V* args, int argc);
  // performanceNow(): milliseconds since the app started.
  V now();
  // A send to mutation `m`: extension source `source` on `argc` args.
  void send(uint16_t m, const char* source, const V* args, int argc);
};

using CodeFn = V (*)(Ctx&);

enum class T : uint8_t { View, Text, Pressable, Image };
enum class E : uint8_t { Press, Key, Navigate, Pan, PanRelease };
enum class P : uint8_t {
  Text, TestId, Autofocus, Id, NavigationKey, NavigationBack, NavigationPresentation, Closedby, Role,
  Controls, Source, Dataset, Ignored,
};
enum class R : uint8_t { When, Each };
enum class S : uint8_t {
  Width, Height, PaddingTop, PaddingRight, PaddingBottom, PaddingLeft, FlexDirection,
  JustifyContent, AlignItems, PositionType, Top, Right, Bottom, Left, Display, BoxSizing,
  BorderWidthTop, BorderWidthRight, BorderWidthBottom, BorderWidthLeft, BackgroundColor,
  BorderRadiusTopLeft, BorderRadiusTopRight, BorderRadiusBottomRight, BorderRadiusBottomLeft,
  BorderColorTop, BorderColorRight, BorderColorBottom, BorderColorLeft, FontSize, FontWeight,
  TextAlign, TextColor, BorderStyleTop, BorderStyleRight, BorderStyleBottom, BorderStyleLeft,
  Appearance, MarginTop, MarginRight, MarginBottom, MarginLeft, RowGap, ColumnGap, Opacity,
  LineHeight, FontFamily, ObjectFit, OverflowX, OverflowY, WhiteSpace, TextOverflow,
};

// A literal binding value, constant-initialized in the generated tables.
struct Lit {
  uint8_t k;  // 0 none, 1 number, 2 bool, 3 string
  double n;
  bool b;
  const char* s;
  static constexpr Lit none() { return Lit{0, 0, false, nullptr}; }
  static constexpr Lit num(double v) { return Lit{1, v, false, nullptr}; }
  static constexpr Lit boolean(bool v) { return Lit{2, 0, v, nullptr}; }
  static constexpr Lit str(const char* v) { return Lit{3, 0, false, v}; }
};

struct Target {
  bool prop;   // a prop (P) or a style row (S)
  uint8_t id;
};

struct Binding {
  Target target;
  CodeFn fn;  // nullptr for a literal
  Lit lit;
};

struct Node {
  T type;
  int16_t parent;
  int16_t arm;
  uint32_t order;
  uint16_t binding_start, binding_count, handler_start, handler_count;
};

// An event handler; its bound arguments are `args[arg_start…]`, evaluated
// in the node's scope when the event comes.
struct Handler {
  E event;
  uint16_t action;
  uint16_t arg_start, arg_count;
};

struct Region {
  R kind;
  int16_t parent;
  int16_t arm;
  uint32_t order;
  CodeFn subject;
  CodeFn key;  // an `each` row's key, in the row's frame; nullptr for `when`
  uint16_t arm_start, arm_count;
};

// A `routes` row (LLP 1038 §3).
struct RouteRow {
  const char* name;
  const char* pattern;  // "" for notfound
  int16_t parent;
  bool tab;
  bool notfound;
};

struct Timer {
  uint32_t interval_ms;
  uint16_t action;
  bool once;
};

// A resource asks `source` with its arguments, `arg_count` code functions
// from `App::args` at `arg_start`, and asks again when their values change.
struct Resource {
  const char* name;
  const char* source;
  uint16_t ty;
  uint16_t arg_start;
  uint16_t arg_count;
};

// The app's fonts, rasterized by plan2cpp (4-bit coverage, ASCII and
// Latin-1). Lengths in 1/64 px. A glyph's coverage is `w * h` nibbles from
// byte `bits` of the app's glyph data, high nibble first; it sits `left` px
// right of the pen and `top` px above the baseline.
struct Glyph {
  uint16_t code;
  int16_t left, top;
  uint8_t w, h;
  int32_t advance;
  uint32_t bits;
};

// A kerning pair of ASCII code points, in font units.
struct Kern {
  uint16_t left, right;
  int16_t units;
};

// A font file: its kerning pairs (sorted by left, then right) and units per em.
struct FontFile {
  uint32_t kern_start, kern_count;
  uint16_t upem;
};

// One face of a declared family at one size.
struct Face {
  int16_t family;
  uint16_t weight;
  float px;
  int32_t ascent, descent, leading;  // 1/64 px
  uint32_t glyph_start;
  uint16_t glyph_count;
  uint16_t file;
};

// An image from the app's assets/. `format` 0: RGB565 pixels (little-endian);
// 1: RGB565 and an alpha byte per pixel; 2: a mask, alpha only, in `color`.
struct Image {
  const char* source;
  uint16_t w, h;
  uint8_t format;
  uint32_t color;
  const uint8_t* rgb;
  const uint8_t* alpha;
};

// A host extension the app lists (extensions.json), as plan2cpp read its
// file: the board must give one with this name and version; `digest` is the
// file's SHA-256, so a board built from another copy of the file is reported.
struct ExtensionUse {
  const char* name;
  uint16_t version;
  const char* digest;
};

// A source the app reads or sends to, the extension that declares it (an
// index into App::extensions), and the plan type of its answer.
struct ExtensionSource {
  const char* source;
  uint16_t extension;
  uint16_t ty;
};

// A design token (Exact LLP 1121, `app.tamber`): a name a colour row may give in place of
// a colour, and the one colour it stands for on this board, as 0xAARRGGBB.
struct Token {
  const char* name;
  uint32_t argb;
};

// A declared sound (Exact LLP 1096): its path, as a step's `src` names it, and its 16-bit
// mono samples at `rate` Hz.
struct Sound {
  const char* src;
  const int16_t* samples;
  uint32_t frames;
  uint32_t rate;
};

// A gated `sequence` (Exact LLP 1125): its gate and `play`'s tracks and tempo, which the
// transport reads after each commit (`rt_transport.cpp`).
struct Sequence {
  const char* name;
  CodeFn gate;
  CodeFn tracks;
  CodeFn bpm;
};

struct App {
  const CodeFn* slots;
  const char* const* slot_names;
  uint16_t slot_count;
  const CodeFn* derives;
  const char* const* derive_names;
  uint16_t derive_count;
  const Resource* resources;
  uint16_t resource_count;
  const CodeFn* actions;
  const char* const* action_names;
  uint16_t action_count;
  const Binding* bindings;
  const Node* nodes;
  uint16_t node_count;
  const Handler* handlers;
  const Region* regions;
  uint16_t region_count;
  const Timer* timers;
  uint16_t timer_count;
  // What the app asks of a board, for the boot report: keys (a key handler or
  // a pressable node), touch (a press handler), and the commands it emits.
  bool keys;
  bool touch;
  const char* const* commands;
  uint16_t command_count;
  const Type* types;
  const Field* fields;
  const uint16_t* slot_types;
  const uint16_t* derive_types;
  const int16_t* slot_owners;    // the owning arm, or -1 for a root slot
  const uint8_t* action_params;  // each action's parameter count
  const CodeFn* args;            // handlers' bound arguments
  const RouteRow* routes;
  uint16_t route_count;
  int16_t router_slot;           // the `routes` slot, or -1
  const Glyph* glyphs;
  const uint8_t* glyph_bits;
  const Kern* kerns;
  const FontFile* font_files;
  const Face* faces;
  uint16_t face_count;
  const int16_t* stack_family;  // each font stack's family, or -1: the board's faces
  uint16_t stack_count;
  const Image* images;
  uint16_t image_count;
  const uint16_t* mutation_slots;  // each mutation's slot, where its answer lands
  uint16_t mutation_count;
  const ExtensionUse* extensions;
  uint16_t extension_count;
  const ExtensionSource* extension_sources;
  uint16_t extension_source_count;
  const Token* tokens;
  uint16_t token_count;
  const Sound* sounds;
  uint16_t sound_count;
  const Sequence* sequences;
  uint16_t sequence_count;
};

struct Rect {
  int x, y, w, h;
};

// A live node that carries a `data-*` word: its testId, the word's value, its
// place on the screen (all zero headless), and whether it lies under an
// unselected route.
struct Mark {
  std::string test_id, value;
  Rect rect;
  bool inactive;
};

// ---- The board interface ----

// The screen: a drawing surface for one area at a time, the board's font
// faces, and the panel. Colors are 0xRRGGBB. Drawing is clipped to the area
// begun last. A board must give the drawing surface, `fill_rect`, and text;
// the core draws the other shapes with `fill_rect` unless the board has faster
// ones.
class Display {
 public:
  virtual ~Display() = default;
  // Allocates the surface; false if it cannot.
  virtual bool begin() = 0;
  virtual int width() const = 0;
  virtual int height() const = 0;
  // The most rows one area may have: height() when the surface holds the
  // whole screen, fewer when the board draws the screen in bands.
  virtual int band_rows() const = 0;
  // Starts drawing `area` (at most band_rows() high). Its old pixels are
  // undefined; the view paints all of it.
  virtual void begin_area(Rect area) = 0;
  // Sends the area begun last to the panel.
  virtual void push_area() = 0;
  virtual void fill_rect(Rect r, uint32_t rgb) = 0;
  // The defaults use M5GFX 0.2.26's integer algorithms (rt_board.cpp).
  virtual void fill_round_rect(Rect r, int radius, uint32_t rgb);
  virtual void draw_round_rect(Rect r, int radius, uint32_t rgb);
  virtual void fill_circle(int cx, int cy, int radius, uint32_t rgb);
  virtual void draw_circle(int cx, int cy, int radius, uint32_t rgb);
  // The board's face nearest to `px` CSS pixels at `weight`.
  virtual int face(float px, int weight) const = 0;
  // Width and line height of ASCII text in a face, in screen pixels.
  virtual int text_width(int face, const char* text) const = 0;
  virtual int line_height(int face) const = 0;
  // Draws ASCII text with its line box's top-left corner at (x, y).
  virtual void draw_text(int face, const char* text, int x, int y, uint32_t rgb) = 0;
  // The panel's pixels as RGB565 big-endian rows, for screenshots; nullptr if
  // the board keeps no copy of the panel.
  virtual const uint8_t* frame(uint32_t* bytes) const;
  // Row `y` of the drawing surface (inside the area begun last) as RGB565
  // pixels from the screen's left edge, for the core to blend antialiased
  // text, images, and opacity into; nullptr if the board gives no surface
  // (the core then draws those at full or no coverage). `big_endian()`: the
  // board keeps the pixels big-endian.
  virtual uint16_t* row(int y);
  virtual bool big_endian() const { return false; }
};

// The agent's byte channel (USB serial on a board, a pseudo-terminal on a Mac).
class Link {
 public:
  virtual ~Link() = default;
  // The next byte received, or -1 when none is waiting.
  virtual int read() = 0;
  // Sends one line (a newline is added). `bulk` lines are part of a long
  // answer and may wait for the reader; other lines are dropped when no one
  // reads, so a closed port never stalls the app.
  virtual void write_line(const std::string& line, bool bulk) = 0;
};

// A board input: a key in Exact terms, or one contact's raw phases on
// the screen (the core recognizes taps and pans from them).
struct Input {
  enum class K : uint8_t { Key, Down, Move, Up } k = K::Key;
  const char* key = nullptr;  // a KeyboardEvent key name, for K::Key
  int x = 0, y = 0;           // the contact, in screen pixels, for Down/Move/Up
};

// The controls a board has.
struct Inputs {
  bool keys = false;
  bool touch = false;
};

// One named board counter for the agent's `stats` (free memory, …).
struct Counter {
  const char* name;
  int64_t value;
};

// An accelerometer, for an extension that reads motion.
class Accelerometer {
 public:
  virtual ~Accelerometer() = default;
  // Acceleration in g on the board's axes; false when no new sample.
  virtual bool read(float& x, float& y, float& z) = 0;
};

// A speaker, for an app's sounds. The transport issues each voice up to a second before it
// starts, so the board can start it on time whatever the loop is doing.
class Speaker {
 public:
  virtual ~Speaker() = default;
  // Starts voice `id`, the app's sound `sound` (an index into `App::sounds`), at board time
  // `at_us` (now if past), times `gain` (0–1).
  virtual void play(uint32_t id, uint16_t sound, int64_t at_us, float gain) = 0;
  // Ends every voice that has not started by `at_us`: it never sounds.
  virtual void cancel_from(int64_t at_us) = 0;
};

// A board gives the clock and any of the other parts. A part it does not have
// keeps the default: no display (the app runs headless), no link (no agent),
// no inputs, no counters, no commands.
class Board {
 public:
  virtual ~Board() = default;
  // Microseconds since the board started.
  virtual int64_t micros() const = 0;
  uint32_t millis() const { return static_cast<uint32_t>(micros() / 1000); }
  virtual Display* display() { return nullptr; }
  virtual Link* link() { return nullptr; }
  virtual Accelerometer* accelerometer() { return nullptr; }
  virtual Speaker* speaker() { return nullptr; }
  // A manual clock moves on by `us`; false on a board whose clock is real. The agent's
  // `clock` op drives it, as Exact's driver drives the runner's clock.
  virtual bool advance_us(int64_t /*us*/) { return false; }
  // The controls `input` can report.
  virtual Inputs inputs() const { return {}; }
  // The next input from the board's own controls, or false when none.
  virtual bool input(Input& out);
  // The counters this board keeps (free memory, …), reported in `stats` under
  // their own names.
  virtual std::vector<Counter> counters() const { return {}; }
  // Whether `command` does something for `name`.
  virtual bool can(const std::string& name) const;
  // A command the runtime does not handle itself (`haptic`, …); a board
  // ignores what it cannot do.
  virtual void command(const std::string& name, const std::vector<V>& args);
  // Board housekeeping once per loop turn (for example, ending a vibration).
  virtual void step(uint32_t now_ms);
  // The wall clock: milliseconds since the Unix epoch, UTC, and the local
  // offset in minutes east of UTC; false if the board has no set clock.
  virtual bool wall_clock(int64_t& epoch_ms, int& utc_offset) const;
  // Waits a little between loop turns.
  virtual void idle() {}
};

// ---- Host extensions ----

// A host extension: the sources, agent ops and props one extension file declares
// (extensions/<name>.json), given by a board or an app's glue. plan2cpp
// writes a typed base class for each file (`plan2cpp ext`), with a method per
// source and op; the overrides below come from it. The reserved `exactTime`
// the core answers itself, from the board's wall clock.
class Extension {
 public:
  virtual ~Extension() = default;
  virtual const char* name() const = 0;
  virtual uint16_t version() const = 0;
  // The SHA-256 of the extension file this was built from.
  virtual const char* digest() const = 0;
  // The sources and ops the file declares.
  virtual const char* const* sources(uint16_t& count) const = 0;
  virtual const char* const* ops(uint16_t& count) const = 0;
  // The `data-*` words it reads on the app's nodes (`Runtime::marks`).
  virtual const char* const* props(uint16_t& count) const = 0;
  // The answer of `source` on `argc` arguments: a resource's (none) or a
  // send's; false when there is none.
  virtual bool answer(const App& app, const char* source, const V* args, int argc, V& out) = 0;
  // One of its ops: answers `line` into `answer`, or returns false.
  virtual bool op(Runtime& runtime, const std::string& line, std::string& answer) = 0;
  // Once a turn, before the runtime's: read sensors, and tell the runtime
  // when a live source changed (`Runtime::changed`).
  virtual void sense(Runtime&) {}
};

// ---- The runtime ----

// A record of the plan's type `ty`, its fields by name; a field left out is none.
V record(const App& app, uint16_t ty, const std::vector<std::pair<const char*, V>>& fields);
// The plan type of the answer of extension source `source`, or -1 if the
// app does not use it.
int source_type(const App& app, const char* source);

struct Stats {
  uint32_t frames = 0;
  uint32_t commits = 0;
  int64_t paint_us = 0;
  int64_t push_us = 0;
  int64_t damage_px = 0;
  int64_t input_to_panel_us = 0;
  uint32_t ready_ms = 0;  // board start to the first frame
};

class View;
class Transport;

class Runtime {
 public:
  // Binds each extension the app lists to the one in `extensions` with its
  // name and version; an app's source with no bound extension answers none.
  Runtime(const App& app, Board& board, std::vector<Extension*> extensions);
  ~Runtime();
  // Starts the view, on the board's display if it has one; false if the
  // display cannot start. A `routes` slot starts at `launch` ("/" on a board).
  bool boot(const std::string& launch = "/");
  // One loop turn: timers due now, then layout and paint of what changed.
  void tick();
  // The data behind resource `i` changed: ask it again and commit.
  void refresh(uint16_t resource);
  // Live source `source` changed: refresh each resource that reads it.
  void changed(const char* source);
  // Each bound extension's `sense`.
  void sense();
  void key(const char* key);
  bool tap(const char* test_id);
  // One phase of a contact at (x, y). A contact that stays within the slop
  // presses the topmost press target under its start when it lifts; past the
  // slop it becomes a pan of the topmost pan target under its start (`pan`
  // with the move since the last event, then `panrelease` with the velocity
  // in px/s), and nothing is pressed. None of it reaches a headless view.
  void pointer(Input::K phase, int x, int y);
  // A pan or pan release at the instance with `test_id` (as the runner's
  // events at a node); false if no live instance has it.
  bool pan(const char* test_id, E event, double a, double b);
  // A location typed into the navigation root (its `navigate` handler, LLP
  // 1038 D8); false if the app has no root with one.
  bool navigate(const std::string& location);
  // Agent ops: one JSON answer per request line.
  std::string agent(const std::string& line);
  // What the app needs that the board lacks: "display", "keys", "touch",
  // "command:<name>", and "extension:<name>" (none with its name and
  // version). The agent's `stats` lists them under "missing".
  std::vector<std::string> missing() const;
  // The bound extensions built from another copy of their file (digest).
  std::vector<std::string> stale() const;
  // The live nodes that carry `data-<word>`, in tree order.
  std::vector<Mark> marks(const char* word) const;
  Stats stats;

  // For Ctx and View.
  V slot(uint64_t i, Scope* scope) const;
  V derive(uint64_t i);
  V resource(uint64_t i) const { return resources_[i]; }
  void write(uint64_t i, const V& v, Scope* scope) { writes_.push_back({i, scope, v}); }
  void command(const char* name, const V* args, int argc);
  void send(uint16_t m, const char* source, const V* args, int argc);
  V call(uint8_t fn, const V* args, int argc);
  // An image the board made at run time (a profile photo), drawn wherever an
  // `image` names `source`; nullptr removes it. The pixels stay the board's.
  void put_image(const std::string& source, const Image* image);
  const Image* image(const std::string& source) const;
  // A failed step (a bad router value, `top` of nothing): the action that
  // took it is rolled back, as the runner traps it.
  void trap(const std::string& why);
  // The owned slots of arm instance `scope`, from their initializers.
  void own(Scope* scope);
  void press(int instance);
  const App& app() const { return app_; }
  Board& board() const { return board_; }
  // Whole milliseconds since boot (performanceNow).
  double elapsed_ms() const { return static_cast<double>((board_.micros() - started_us_) / 1000); }
  // Milliseconds since boot, with the fraction: the transport's time.
  double now_ms() const { return static_cast<double>(board_.micros() - started_us_) / 1000.0; }
  // Board micros for transport time `ms`.
  int64_t board_us(double ms) const { return started_us_ + static_cast<int64_t>(ms * 1000.0); }

 private:
  // The transport reads the gate and `play` in the root scope after each commit, and
  // journals what it drops.
  friend class Transport;
  struct Write {
    uint64_t slot;
    Scope* scope;
    V v;
  };
  void run(uint16_t action, const V* params, Scope* scope);
  V answer(uint16_t resource);
  std::vector<V> resource_args(uint16_t resource);
  void reask();
  void settle();
  void log(const std::string& line);
  // The handler for `event` at the instance or its nearest ancestor that
  // has one, and that instance in `at`; -1 if none.
  int handler(int instance, E event, int* at) const;
  void refocus();
  void step_focus(int by);
  bool focusable(int instance) const;
  bool press_back();
  std::string state_json();
  std::string stats_json();
  std::string nodes_json();
  std::string extensions_json();
  V ask(const char* source, const V* args, int argc);
  V router_value(const route::Router& r) const;
  bool router_of(const V& v, route::Router& out) const;
  route::Entry entry_of_value(const V& v, bool* ok) const;
  V entry_value(const route::Entry& e) const;

  const App& app_;
  Board& board_;
  std::vector<Extension*> bound_;  // per app extension, or nullptr
  std::unique_ptr<route::Table> table_;
  std::vector<V> slots_, derives_, resources_;
  std::vector<std::vector<V>> asked_;  // each resource's arguments when it last asked
  std::vector<bool> derive_valid_;
  std::vector<Write> writes_;
  std::vector<std::pair<std::string, std::vector<V>>> commands_;
  struct Send {
    uint16_t mutation;
    std::string source;
    std::vector<V> args;
  };
  std::vector<Send> sends_;
  std::vector<std::pair<std::string, const Image*>> images_;
  std::vector<uint32_t> timer_due_;
  std::vector<std::string> logs_, refusals_;
  std::unique_ptr<View> view_;
  std::string focus_key_;  // the focused instance's key, kept across updates
  std::vector<std::string> shown_;  // instance keys after the last update
  int focus_ = -1;
  bool prevented_ = false;
  bool trapped_ = false;
  int64_t input_at_us_ = -1;
  int64_t started_us_ = 0;
  // The contact the pointer phases describe.
  struct Contact {
    bool down = false, panning = false, moved = false;
    int start_x = 0, start_y = 0, x = 0, y = 0;
    int target = -1;  // the pan target's instance, or -1
    std::string target_key;
    double vx = 0, vy = 0;  // px/s, from the last moves
    int64_t at_us = 0;
  };
  void dispatch_pan(int instance, E event, double a, double b);
  Contact contact_;
  // Moves a manual clock on by `ms`, running each timer and transport wake at its own due
  // time on the way (Exact's seek rule); false on a board whose clock is real.
  bool advance(double ms);
  std::unique_ptr<Transport> transport_;
};

// The string field `"name": "value"` of a flat JSON request line, or "".
std::string string_field(const std::string& line, const char* name);
// The number field `"name": value` of a flat JSON request line, or 0.
double number_field(const std::string& line, const char* name);

// Runs an app on a board with its extensions: boot, then one turn per loop.
// The agent's lines (`CE_AGENT {json}`, `CE_SHOT …` screenshots) go over the
// board's link; an extension's op is answered by the extension.
class Host {
 public:
  Host(const App& app, Board& board, std::vector<Extension*> extensions);
  // Boots the app and paints the first frame; false (and a `CE_ERROR` line)
  // if the display cannot start. Then one `CE_WARN missing=…` line for each
  // part the app needs and the board lacks, one `CE_WARN extension=…
  // detail=digest` line for each stale extension, and `CE_READY`.
  bool boot();
  // The extensions' sense, board inputs, timers, paint, then waiting agent lines.
  void turn();
  Runtime& runtime() { return runtime_; }

 private:
  void serve(const std::string& line);
  void screenshot();
  void report();
  void write(const std::string& line, bool bulk);

  Board& board_;
  Runtime runtime_;
  std::string line_;
};

}  // namespace rt
