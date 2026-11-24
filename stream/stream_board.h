// A Mac as the host of an Exact app whose screen is a thin-client board on a
// serial line (the SenseCAP Watcher, firmware/watcher_stream). The app runs
// here, on the unchanged exact_rt core, against a software RGB565 surface the
// size the app was written for; each frame's damage is resampled to the
// board's screen, diffed against what the board already shows, and sent as an
// AREA packet. The board's touch and knob come back as rt::Inputs.
#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "exact_rt.h"
#include "exact_rt_mac.h"
#include "exact_stream_proto.h"

namespace xs {

// The serial line to the board: packets out on a writer thread (a big area
// takes the line for a while and must not hold the app's loop), packets in
// from `poll`.
class Line {
 public:
  ~Line();
  bool open(const std::string& path, int baud, std::string& error);
  int fd() const { return fd_; }
  // Queues one packet.
  void send(uint8_t type, const std::vector<uint8_t>& payload);
  // Reads what is waiting; complete packets go to `out` as (type, payload).
  void poll(std::vector<std::pair<uint8_t, std::vector<uint8_t>>>& out);
  uint64_t bytes_out() const { return bytes_out_; }
  uint64_t bytes_in() const { return bytes_in_; }
  // Waits up to `ms` for bytes to read.
  void wait(int ms) const;

 private:
  void writer();
  int fd_ = -1;
  std::thread thread_;
  std::mutex lock_;
  std::condition_variable cv_;
  std::deque<std::vector<uint8_t>> queue_;
  bool stop_ = false;
  std::atomic<uint64_t> bytes_out_{0};
  uint64_t bytes_in_ = 0;
  std::vector<uint8_t> rx_;
};

struct StreamStats {
  uint64_t areas = 0, acks = 0, nacks = 0, keyframes = 0, timeouts = 0;
  uint64_t px_sent = 0, bytes_sent = 0;
  // The last area: its rectangle, encoded size, and times.
  int last_w = 0, last_h = 0;
  uint32_t last_bytes = 0;
  double last_rtt_ms = 0, last_decode_ms = 0, last_push_ms = 0;
  std::vector<double> rtts;  // every ack's round trip, ms
  std::vector<double> input_to_panel;  // an input's arrival to the ack of the first area sent after it, ms
};

// The app's drawing surface (rt_mac::Display, the Stopwatch's integer text) plus
// the damage the board has not been sent yet.
class Display final : public rt::Display {
 public:
  Display(int width, int height) { resize(width, height); }
  // A new surface of `width` x `height` (an app written for another screen).
  void resize(int width, int height) {
    inner_ = std::make_unique<rt_mac::Display>(width, height, height);
    damaged_ = false;
  }
  bool begin() override { return inner_->begin(); }
  int width() const override { return inner_->width(); }
  int height() const override { return inner_->height(); }
  int band_rows() const override { return inner_->band_rows(); }
  void begin_area(rt::Rect area) override {
    area.h = std::min(area.h, inner_->band_rows());
    last_area_ = area;
    inner_->begin_area(area);
  }
  void push_area() override;
  void fill_rect(rt::Rect r, uint32_t rgb) override { inner_->fill_rect(r, rgb); }
  int face(float px, int weight) const override { return inner_->face(px, weight); }
  int text_width(int face, const char* text) const override { return inner_->text_width(face, text); }
  int line_height(int face) const override { return inner_->line_height(face); }
  void draw_text(int face, const char* text, int x, int y, uint32_t rgb) override {
    inner_->draw_text(face, text, x, y, rgb);
  }
  const uint8_t* frame(uint32_t* bytes) const override { return inner_->frame(bytes); }
  uint16_t* row(int y) override { return inner_->row(y); }

  // The damage since the last take, as one rectangle; false if none.
  bool take_damage(rt::Rect& out);
  void damage_all() { damage_ = {0, 0, width(), height()}, damaged_ = true; }
  rt::Rect last_area() const { return last_area_; }

 private:
  std::unique_ptr<rt_mac::Display> inner_;
  rt::Rect last_area_{0, 0, 0, 0};
  rt::Rect damage_{0, 0, 0, 0};
  bool damaged_ = false;
};

class Board final : public rt::Board {
 public:
  // An app drawn at `width` x `height`, shown on the board on `port`.
  Board(int width, int height) : display_(width, height) {}
  bool begin(const std::string& port, int baud, std::string& error);
  const std::string& link_path() const { return link_.path(); }
  // The app surface's size, before an app boots on it.
  void resize(int width, int height);

  int64_t micros() const override { return clock_.micros(); }
  rt::Display* display() override { return &display_; }
  rt::Link* link() override { return &link_; }
  rt::Speaker* speaker() override { return speaker_; }
  void set_speaker(rt::Speaker* s) { speaker_ = s; }
  rt::Inputs inputs() const override { return {true, true}; }
  bool input(rt::Input& out) override;
  std::vector<rt::Counter> counters() const override;
  bool can(const std::string& name) const override { return name == "haptic"; }
  bool wall_clock(int64_t& epoch_ms, int& utc_offset) const override;
  // Reads the board's packets and sends the damage when the line is free.
  void step(uint32_t now_ms) override;
  void idle() override;

  // The board asked for the next app (a 2 s knob hold); cleared by reading.
  bool take_next_app() {
    bool v = next_app_;
    next_app_ = false;
    return v;
  }
  // A key typed at the host, as if from the board.
  void push_key(const char* key);
  void set_light(int percent);
  // The next send repaints the board's whole screen.
  void keyframe();
  bool connected() const { return board_w_ > 0; }
  // Plan mode: the board runs `blob` (vm/plan_blob.h) itself; the line carries
  // only agent lines and the board's answers. The app surface here is idle.
  void send_plan(const std::vector<uint8_t>& blob);
  void send_agent(const std::string& line);
  // Back to thin-client mode: the next damage repaints the board.
  void leave_plan();
  bool plan_mode() const { return plan_mode_; }
  int board_width() const { return board_w_; }
  int board_height() const { return board_h_; }
  const StreamStats& stats() const { return stats_; }
  std::string stats_line() const;

 private:
  void handle(uint8_t type, const std::vector<uint8_t>& p);
  void send_damage();
  // Resamples the app's surface over board rectangle `r` into `out` (RGB565).
  void resample(rt::Rect r, std::vector<uint16_t>& out) const;
  void build_maps();

  Display display_;
  rt_mac::Link link_;
  rt_mac::Clock clock_;
  Line line_;
  rt::Speaker* speaker_ = nullptr;
  std::deque<rt::Input> inputs_;
  int board_w_ = 0, board_h_ = 0;
  std::vector<uint16_t> shadow_;  // what the board shows, RGB565
  bool shadow_valid_ = false;
  rt::Rect pending_{0, 0, 0, 0};  // board damage not sent yet
  bool has_pending_ = false;
  bool in_flight_ = false;
  uint16_t seq_ = 0;
  int64_t sent_us_ = 0;
  int64_t last_hello_us_ = -10000000;
  int64_t input_at_us_ = -1;   // an input waiting for its frame
  int64_t input_area_sent_us_ = -1;
  bool next_app_ = false;
  bool plan_mode_ = false;
  std::vector<uint8_t> plan_payload_;
  int64_t plan_sent_us_ = 0;
  std::string pty_line_;
  StreamStats stats_;
  // Area-average resampling: for each board column (row), the app columns
  // (rows) it covers and their weights in 1/256.
  struct Tap {
    int start, count;
    std::vector<int> w;
  };
  std::vector<Tap> xmap_, ymap_;
};

}  // namespace xs
