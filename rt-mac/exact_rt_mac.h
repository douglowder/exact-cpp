// A Mac as a exact_rt board, for host runs and parity checks without
// hardware. `Board`: a software RGB565 surface of a given size, drawn in bands
// of a given height into a simulated panel; the Stopwatch's FreeSans faces,
// read from M5GFX's font data; and a pseudo-terminal link that tools/agent.py
// opens like a serial port. `HeadlessBoard`: the link and the clock only, for
// the parts a board may leave out. Neither has buttons, touch, or an
// accelerometer: the agent drives them.
#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "exact_rt.h"

namespace rt_mac {

// A wall clock set once (from `--clock`), advancing with the board's clock.
struct WallClock {
  bool set = false;
  int64_t epoch_ms = 0;
  int utc_offset = 0;
  int64_t at_us = 0;
  bool read(int64_t now_us, int64_t& epoch, int& offset) const {
    if (!set) return false;
    epoch = epoch_ms + (now_us - at_us) / 1000;
    offset = utc_offset;
    return true;
  }
};

class Display final : public rt::Display {
 public:
  // `band_rows` of at most `height`; `height` draws the whole screen at once.
  Display(int width, int height, int band_rows);
  bool begin() override;
  int width() const override { return width_; }
  int height() const override { return height_; }
  int band_rows() const override { return band_rows_; }
  void begin_area(rt::Rect area) override;
  void push_area() override;
  void fill_rect(rt::Rect r, uint32_t rgb) override;
  int face(float px, int weight) const override;
  int text_width(int face, const char* text) const override;
  int line_height(int face) const override;
  void draw_text(int face, const char* text, int x, int y, uint32_t rgb) override;
  const uint8_t* frame(uint32_t* bytes) const override;
  uint16_t* row(int y) override;

 private:
  void fill(int x, int y, int w, int h);

  int width_, height_, band_rows_;
  std::vector<uint16_t> band_;  // band_rows_ rows of the screen, from area_.y
  std::vector<uint8_t> panel_;  // the whole panel, RGB565 big-endian
  rt::Rect area_{0, 0, 0, 0};
  uint16_t color_ = 0;
};

// A pseudo-terminal. The slave side stays open here, in raw mode, so clients
// can come and go.
class Link final : public rt::Link {
 public:
  ~Link() override;
  // Opens the pseudo-terminal; false with a message in `error` if it cannot.
  bool begin(std::string& error);
  const std::string& path() const { return path_; }
  int read() override;
  void write_line(const std::string& line, bool bulk) override;
  // Waits up to `ms` for a byte to read.
  void wait(int ms) const;

 private:
  int master_ = -1;
  int slave_ = -1;
  std::string path_;
};

// A board's clock: the time since it started, or, set manual, a clock that starts at 0 and
// moves only by the agent's `clock` op (rt::Board::advance_us), so a script is the same on
// every run, as Exact's driver's clock is.
class Clock {
 public:
  int64_t micros() const;
  void set_manual() { manual_ = true; }
  bool advance(int64_t us) {
    if (!manual_) return false;
    manual_us_ += us;
    return true;
  }

 private:
  bool manual_ = false;
  int64_t manual_us_ = 0;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

class Board final : public rt::Board {
 public:
  Board(int width, int height, int band_rows);
  // Opens the link; false with a message in `error` if it cannot.
  bool begin(std::string& error) { return link_.begin(error); }
  const std::string& link_path() const { return link_.path(); }
  int64_t micros() const override { return clock_now_.micros(); }
  void set_manual_clock() { clock_now_.set_manual(); }
  bool advance_us(int64_t us) override { return clock_now_.advance(us); }
  rt::Display* display() override { return &display_; }
  rt::Link* link() override { return &link_; }
  // The speaker, for an app with sounds (speaker_mac.hpp); none by default.
  void set_speaker(rt::Speaker* speaker) { speaker_ = speaker; }
  rt::Speaker* speaker() override { return speaker_; }
  std::vector<rt::Counter> counters() const override;
  // A wall clock from `epoch_ms` (UTC) now, with `utc_offset` minutes east.
  void set_clock(int64_t epoch_ms, int utc_offset) { clock_ = {true, epoch_ms, utc_offset, micros()}; }
  bool wall_clock(int64_t& epoch_ms, int& utc_offset) const override { return clock_.read(micros(), epoch_ms, utc_offset); }
  // Counts haptics, for the agent's `stats`.
  bool can(const std::string& name) const override { return name == "haptic"; }
  void command(const std::string& name, const std::vector<rt::V>& args) override;
  void idle() override;

 private:
  Display display_;
  Link link_;
  Clock clock_now_;
  rt::Speaker* speaker_ = nullptr;
  int64_t haptics_ = 0;
  WallClock clock_;
};

// No display and no commands: the app runs headless, and the agent reads its
// state and drives it with keys and taps by testId.
class HeadlessBoard final : public rt::Board {
 public:
  // Opens the link; false with a message in `error` if it cannot.
  bool begin(std::string& error) { return link_.begin(error); }
  const std::string& link_path() const { return link_.path(); }
  int64_t micros() const override { return clock_now_.micros(); }
  void set_manual_clock() { clock_now_.set_manual(); }
  bool advance_us(int64_t us) override { return clock_now_.advance(us); }
  rt::Link* link() override { return &link_; }
  void set_clock(int64_t epoch_ms, int utc_offset) { clock_ = {true, epoch_ms, utc_offset, micros()}; }
  bool wall_clock(int64_t& epoch_ms, int& utc_offset) const override { return clock_.read(micros(), epoch_ms, utc_offset); }
  std::vector<rt::Counter> counters() const override;
  void idle() override;

 private:
  Link link_;
  Clock clock_now_;
  WallClock clock_;
};

}  // namespace rt_mac
