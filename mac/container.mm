// ExactMac: one window in which any of the Mac apps runs, one at a time, chosen from the
// App menu. Each app is a bundle (build/apps/APP.bundle: its app_gen.cpp, glue and
// app_entry.cpp) linked against this program's exact_rt core. The window is a board with a
// display (the same software surface as the -mac programs, drawn 1:1), keys (the arrows,
// Return as Enter, Escape), touch (the mouse), a speaker on the real clock, the trackpad's
// haptics, the system's wall clock, and the agent's pseudo-terminal, so tools/agent.py drives
// it as it drives the board. The Appearance menu picks the color scheme (dark: white text on
// black, the default; light: black text on white), kept across apps.
//
//   ExactMac [APP] [--link PATH]
//     APP          the app to start with (default: the first bundle)
//     --link PATH  also make PATH a symbolic link to the pseudo-terminal
#import <AppKit/AppKit.h>
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "exact_mac_app.hpp"
#include "exact_rt.h"
#include "exact_rt_mac.h"
#include "speaker_mac.hpp"

namespace {

constexpr int kWidth = 468;
constexpr int kHeight = 466;

// The window as a board. Its inputs come from the view's events; it never waits between turns,
// as the window's run loop does that.
class WindowBoard final : public rt::Board {
 public:
  WindowBoard() : display_(kWidth, kHeight, kHeight) {}
  bool begin(std::string& error) { return link_.begin(error); }
  const std::string& link_path() const { return link_.path(); }
  const uint8_t* frame() const { return display_.frame(nullptr); }
  int64_t micros() const override { return clock_.micros(); }
  rt::Display* display() override { return &display_; }
  rt::Link* link() override { return &link_; }
  rt::Speaker* speaker() override { return speaker_; }
  void set_speaker(rt::Speaker* speaker) { speaker_ = speaker; }
  rt::Inputs inputs() const override { return {true, true}; }
  void push(const rt::Input& in) { queue_.push_back(in); }
  bool input(rt::Input& out) override {
    if (queue_.empty()) return false;
    out = queue_.front();
    queue_.pop_front();
    return true;
  }
  bool can(const std::string& name) const override { return name == "haptic"; }
  void command(const std::string& name, const std::vector<rt::V>& /*args*/) override {
    if (name != "haptic") return;
    [[NSHapticFeedbackManager defaultPerformer] performFeedbackPattern:NSHapticFeedbackPatternGeneric
                                                       performanceTime:NSHapticFeedbackPerformanceTimeNow];
  }
  bool wall_clock(int64_t& epoch_ms, int& utc_offset) const override {
    const auto now = std::chrono::system_clock::now();
    epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&t, &local);
    utc_offset = static_cast<int>(local.tm_gmtoff / 60);
    return true;
  }

 private:
  rt_mac::Display display_;
  rt_mac::Link link_;
  rt_mac::Clock clock_;
  rt::Speaker* speaker_ = nullptr;
  std::deque<rt::Input> queue_;
};

// The running app: its bundle's entry, its speaker and its host. A bundle stays loaded once
// opened (its extensions are its globals); opening it again reuses it.
struct Container {
  WindowBoard board;
  std::unique_ptr<rt_mac::Speaker> speaker;
  std::unique_ptr<rt::Host> host;
  std::string name;
  uint32_t frames = 0;
  // The Appearance menu's choice, kept across apps.
  rt::Scheme scheme = rt::Scheme::Dark;

  // Stops the running app and starts the one in `path`; false with a message in `error`.
  bool load(const std::string& path, std::string& error) {
    host.reset();
    board.set_speaker(nullptr);
    speaker.reset();
    void* bundle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!bundle) {
      error = dlerror();
      return false;
    }
    auto entry = reinterpret_cast<const ExactMacApp* (*)()>(dlsym(bundle, "exact_mac_app"));
    if (!entry) {
      error = path + " has no exact_mac_app entry";
      return false;
    }
    const ExactMacApp* a = entry();
    if (a->abi != kExactMacAbi) {
      error = path + " is built for ExactMac ABI " + std::to_string(a->abi) + "; rebuild it";
      return false;
    }
    if (a->app->sound_count > 0) {
      speaker = std::make_unique<rt_mac::Speaker>(*a->app, board);
      std::string why;
      if (speaker->start(why)) {
        board.set_speaker(speaker.get());
      } else {
        NSLog(@"ExactMac: no speaker: %s", why.c_str());
        speaker.reset();
      }
    }
    host = std::make_unique<rt::Host>(*a->app, board, a->extensions());
    host->runtime().set_scheme(scheme);
    if (!host->boot()) {
      error = std::string(a->name) + ": the display did not start";
      host.reset();
      return false;
    }
    name = a->name;
    frames = 0;
    return true;
  }
};

Container* container = nullptr;

// The bundles beside this program, in build/apps, by name.
std::vector<std::string> bundles(const std::string& dir) {
  std::vector<std::string> out;
  for (NSString* file in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@(dir.c_str()) error:nil]) {
    if ([file.pathExtension isEqualToString:@"bundle"]) out.push_back(file.stringByDeletingPathExtension.UTF8String);
  }
  std::sort(out.begin(), out.end());
  return out;
}

// A KeyboardEvent key name for a key the Stopwatch's buttons give, or nullptr.
const char* key_name(unsigned short code) {
  switch (code) {
    case 123: return "ArrowLeft";
    case 124: return "ArrowRight";
    case 125: return "ArrowDown";
    case 126: return "ArrowUp";
    case 36:
    case 76: return "Enter";
    case 53: return "Escape";
    default: return nullptr;
  }
}

}  // namespace

// The screen, drawn 1:1 from the display's RGB565 surface with no smoothing; its key and
// mouse events are the board's inputs.
@interface ScreenView : NSView
@end

@implementation ScreenView

- (BOOL)isFlipped {
  return YES;
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (void)drawRect:(NSRect)dirty {
  (void)dirty;
  const uint8_t* rgb565 = container->board.frame();
  NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                  pixelsWide:kWidth
                                                                  pixelsHigh:kHeight
                                                               bitsPerSample:8
                                                             samplesPerPixel:3
                                                                    hasAlpha:NO
                                                                    isPlanar:NO
                                                              colorSpaceName:NSDeviceRGBColorSpace
                                                                 bytesPerRow:kWidth * 3
                                                                bitsPerPixel:24];
  uint8_t* out = rep.bitmapData;
  for (int i = 0; i < kWidth * kHeight; ++i) {
    const unsigned v = static_cast<unsigned>(rgb565[2 * i] << 8 | rgb565[2 * i + 1]);
    const unsigned r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
    out[3 * i] = static_cast<uint8_t>(r << 3 | r >> 2);
    out[3 * i + 1] = static_cast<uint8_t>(g << 2 | g >> 4);
    out[3 * i + 2] = static_cast<uint8_t>(b << 3 | b >> 2);
  }
  [NSGraphicsContext currentContext].imageInterpolation = NSImageInterpolationNone;
  [rep drawInRect:NSMakeRect(0, 0, kWidth, kHeight)
         fromRect:NSZeroRect
        operation:NSCompositingOperationCopy
         fraction:1.0
   respectFlipped:YES
            hints:nil];
}

- (void)keyDown:(NSEvent*)event {
  const char* key = key_name(event.keyCode);
  if (!key) return [super keyDown:event];
  if (event.isARepeat) return;
  rt::Input in;
  in.k = rt::Input::K::Key;
  in.key = key;
  container->board.push(in);
}

- (void)contact:(NSEvent*)event phase:(rt::Input::K)phase {
  const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  rt::Input in;
  in.k = phase;
  in.x = static_cast<int>(p.x);
  in.y = static_cast<int>(p.y);
  container->board.push(in);
}

- (void)mouseDown:(NSEvent*)event {
  [self contact:event phase:rt::Input::K::Down];
}

- (void)mouseDragged:(NSEvent*)event {
  [self contact:event phase:rt::Input::K::Move];
}

- (void)mouseUp:(NSEvent*)event {
  [self contact:event phase:rt::Input::K::Up];
}

@end

@interface Controller : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) ScreenView* screen;
@property(nonatomic, strong) NSMenu* apps;
@property(nonatomic, strong) NSMenu* appearance;
@property(nonatomic, copy) NSString* dir;
@end

@implementation Controller

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
  (void)app;
  return YES;
}

- (void)open:(NSString*)name {
  std::string error;
  NSString* path = [self.dir stringByAppendingPathComponent:[name stringByAppendingPathExtension:@"bundle"]];
  if (!container->load(path.UTF8String, error)) {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = [NSString stringWithFormat:@"%@ did not start", name];
    alert.informativeText = @(error.c_str());
    [alert runModal];
    return;
  }
  self.window.title = name;
  for (NSMenuItem* item in self.apps.itemArray) {
    item.state = [item.title isEqualToString:name] ? NSControlStateValueOn : NSControlStateValueOff;
  }
  self.screen.needsDisplay = YES;
}

- (void)choose:(NSMenuItem*)item {
  [self open:item.title];
}

// Checks the Appearance item of the scheme in use.
- (void)markScheme {
  for (NSMenuItem* item in self.appearance.itemArray) {
    item.state = item.tag == static_cast<NSInteger>(container->scheme) ? NSControlStateValueOn : NSControlStateValueOff;
  }
}

- (void)chooseScheme:(NSMenuItem*)item {
  container->scheme = static_cast<rt::Scheme>(item.tag);
  if (container->host) container->host->runtime().set_scheme(container->scheme);
  [self markScheme];
  self.screen.needsDisplay = YES;
}

- (void)turn {
  if (!container->host) return;
  container->host->turn();
  // The agent's `scheme` op can change it too.
  if (container->host->runtime().scheme() != container->scheme) {
    container->scheme = container->host->runtime().scheme();
    [self markScheme];
  }
  const uint32_t frames = container->host->runtime().stats.frames;
  if (frames != container->frames) {
    container->frames = frames;
    self.screen.needsDisplay = YES;
  }
}

@end

namespace {

NSMenu* main_menu(Controller* controller, const std::vector<std::string>& names) {
  NSMenu* bar = [[NSMenu alloc] init];
  NSMenuItem* app_item = [[NSMenuItem alloc] init];
  [bar addItem:app_item];
  NSMenu* app_menu = [[NSMenu alloc] init];
  [app_menu addItemWithTitle:@"Quit ExactMac" action:@selector(terminate:) keyEquivalent:@"q"];
  app_item.submenu = app_menu;
  NSMenuItem* apps_item = [[NSMenuItem alloc] init];
  [bar addItem:apps_item];
  NSMenu* apps = [[NSMenu alloc] initWithTitle:@"App"];
  int n = 0;
  for (const std::string& name : names) {
    NSString* key = ++n <= 9 ? [NSString stringWithFormat:@"%d", n] : @"";
    NSMenuItem* item = [apps addItemWithTitle:@(name.c_str()) action:@selector(choose:) keyEquivalent:key];
    item.target = controller;
  }
  apps_item.submenu = apps;
  controller.apps = apps;
  NSMenuItem* appearance_item = [[NSMenuItem alloc] init];
  [bar addItem:appearance_item];
  NSMenu* appearance = [[NSMenu alloc] initWithTitle:@"Appearance"];
  const std::pair<NSString*, rt::Scheme> schemes[] = {{@"Dark", rt::Scheme::Dark}, {@"Light", rt::Scheme::Light}};
  for (const auto& scheme : schemes) {
    NSString* key = scheme.second == rt::Scheme::Dark ? @"d" : @"l";
    NSMenuItem* item = [appearance addItemWithTitle:scheme.first action:@selector(chooseScheme:) keyEquivalent:key];
    item.tag = static_cast<NSInteger>(scheme.second);
    item.target = controller;
  }
  appearance_item.submenu = appearance;
  controller.appearance = appearance;
  [controller markScheme];
  return bar;
}

struct Options {
  std::string app;
  std::string link;
};

bool parse(int argc, const char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--link" && i + 1 < argc) {
      o.link = argv[++i];
    } else if (argv[i][0] != '-' && o.app.empty()) {
      o.app = argv[i];
    } else {
      std::fprintf(stderr, "usage: ExactMac [APP] [--link PATH]\n");
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, const char** argv) {
  Options options;
  if (!parse(argc, argv, options)) return 2;
  @autoreleasepool {
    static Container shared;
    container = &shared;
    std::string error;
    if (!container->board.begin(error)) {
      std::fprintf(stderr, "ExactMac: %s\n", error.c_str());
      return 1;
    }
    if (!options.link.empty()) {
      unlink(options.link.c_str());
      if (symlink(container->board.link_path().c_str(), options.link.c_str()) != 0) {
        std::fprintf(stderr, "ExactMac: cannot link %s: %s\n", options.link.c_str(), std::strerror(errno));
        return 1;
      }
    }
    NSString* exe = [NSString stringWithUTF8String:argv[0]];
    NSString* dir = [[exe stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"apps"];
    const std::vector<std::string> names = bundles(dir.UTF8String);
    if (names.empty()) {
      std::fprintf(stderr, "ExactMac: no app bundles in %s (make -C mac container)\n", dir.UTF8String);
      return 1;
    }
    NSApplication* app = [NSApplication sharedApplication];
    app.activationPolicy = NSApplicationActivationPolicyRegular;
    Controller* controller = [[Controller alloc] init];
    controller.dir = dir;
    app.delegate = controller;
    app.mainMenu = main_menu(controller, names);
    controller.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kWidth, kHeight)
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                              NSWindowStyleMaskMiniaturizable
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
    controller.window.subtitle = [NSString stringWithFormat:@"agent: %s", container->board.link_path().c_str()];
    controller.screen = [[ScreenView alloc] initWithFrame:NSMakeRect(0, 0, kWidth, kHeight)];
    controller.window.contentView = controller.screen;
    [controller.window center];
    [controller.window makeKeyAndOrderFront:nil];
    [controller.window makeFirstResponder:controller.screen];
    std::printf("PTY %s\n", container->board.link_path().c_str());
    std::fflush(stdout);
    [controller open:@((options.app.empty() ? names.front() : options.app).c_str())];
    [NSTimer scheduledTimerWithTimeInterval:1.0 / 120
                                    repeats:YES
                                      block:^(NSTimer* timer) {
                                        (void)timer;
                                        [controller turn];
                                      }];
    [app activateIgnoringOtherApps:YES];
    [app run];
  }
  return 0;
}
