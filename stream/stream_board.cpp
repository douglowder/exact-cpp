// The stream board (stream_board.h).
#include "stream_board.h"

#include <IOKit/serial/ioss.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace xs {

namespace {

constexpr int64_t kAckTimeoutUs = 2000000;
constexpr int64_t kHelloEveryUs = 1000000;

void put16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(x & 0xff);
  v.push_back(x >> 8);
}

uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

rt::Rect unite(rt::Rect a, rt::Rect b) {
  const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
  const int x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
  return {x0, y0, x1 - x0, y1 - y0};
}

const char* key_name(uint8_t key) {
  switch (key) {
    case XS_KEY_LEFT: return "ArrowLeft";
    case XS_KEY_RIGHT: return "ArrowRight";
    case XS_KEY_ENTER: return "Enter";
    case XS_KEY_ESCAPE: return "Escape";
    default: return nullptr;
  }
}

}  // namespace

// ---- Line ----

Line::~Line() {
  {
    std::lock_guard<std::mutex> g(lock_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  if (fd_ >= 0) close(fd_);
}

bool Line::open(const std::string& path, int baud, std::string& error) {
  fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd_ < 0) {
    error = "cannot open " + path + ": " + std::strerror(errno);
    return false;
  }
  termios t{};
  tcgetattr(fd_, &t);
  cfmakeraw(&t);
  t.c_cflag |= CLOCAL | CREAD;
  t.c_cflag &= ~(HUPCL | CRTSCTS);
  cfsetspeed(&t, B115200);
  if (tcsetattr(fd_, TCSANOW, &t) != 0) {
    error = std::string("cannot set up the line: ") + std::strerror(errno);
    return false;
  }
  speed_t speed = static_cast<speed_t>(baud);
  if (ioctl(fd_, IOSSIOSPEED, &speed) != 0) {
    // A pseudo-terminal (tools/fake_watcher.py) has no speed.
    std::fprintf(stderr, "exact-stream: cannot set %d baud on %s: %s\n", baud, path.c_str(), std::strerror(errno));
  }
  tcflush(fd_, TCIOFLUSH);
  thread_ = std::thread([this] { writer(); });
  return true;
}

void Line::send(uint8_t type, const std::vector<uint8_t>& payload) {
  const uint32_t len = static_cast<uint32_t>(payload.size());
  std::vector<uint8_t> pkt;
  pkt.reserve(len + 10);
  pkt.insert(pkt.end(), {XS_MAGIC0, XS_MAGIC1, type, uint8_t(len), uint8_t(len >> 8), uint8_t(len >> 16)});
  pkt.insert(pkt.end(), payload.begin(), payload.end());
  uLong crc = crc32(0L, pkt.data() + 2, static_cast<uInt>(pkt.size() - 2));
  for (int i = 0; i < 4; ++i) pkt.push_back(static_cast<uint8_t>((crc >> (8 * i)) & 0xff));
  {
    std::lock_guard<std::mutex> g(lock_);
    queue_.push_back(std::move(pkt));
  }
  cv_.notify_one();
}

void Line::writer() {
  for (;;) {
    std::vector<uint8_t> pkt;
    {
      std::unique_lock<std::mutex> g(lock_);
      cv_.wait(g, [this] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      pkt = std::move(queue_.front());
      queue_.pop_front();
    }
    size_t sent = 0;
    while (sent < pkt.size()) {
      ssize_t n = ::write(fd_, pkt.data() + sent, pkt.size() - sent);
      if (n > 0) {
        sent += static_cast<size_t>(n);
        bytes_out_ += static_cast<uint64_t>(n);
        continue;
      }
      if (n < 0 && errno != EAGAIN && errno != EINTR) break;
      pollfd p{fd_, POLLOUT, 0};
      ::poll(&p, 1, 50);
      std::lock_guard<std::mutex> g(lock_);
      if (stop_) return;
    }
  }
}

void Line::wait(int ms) const {
  pollfd p{fd_, POLLIN, 0};
  ::poll(&p, 1, ms);
}

void Line::poll(std::vector<std::pair<uint8_t, std::vector<uint8_t>>>& out) {
  uint8_t buf[8192];
  for (;;) {
    ssize_t n = ::read(fd_, buf, sizeof buf);
    if (n <= 0) break;
    bytes_in_ += static_cast<uint64_t>(n);
    rx_.insert(rx_.end(), buf, buf + n);
  }
  size_t i = 0;
  while (rx_.size() - i >= 10) {
    if (rx_[i] != XS_MAGIC0 || rx_[i + 1] != XS_MAGIC1) {
      ++i;
      continue;
    }
    const uint32_t len = rx_[i + 3] | (rx_[i + 4] << 8) | (uint32_t(rx_[i + 5]) << 16);
    if (len > 65536) {  // the board sends small packets (a screenshot line is ~4 KB)
      ++i;
      continue;
    }
    if (rx_.size() - i < len + 10) break;
    const uLong crc = crc32(0L, &rx_[i + 2], 4 + len);
    if (get32(&rx_[i + 6 + len]) != static_cast<uint32_t>(crc)) {
      ++i;
      continue;
    }
    out.emplace_back(rx_[i + 2], std::vector<uint8_t>(rx_.begin() + i + 6, rx_.begin() + i + 6 + len));
    i += len + 10;
  }
  rx_.erase(rx_.begin(), rx_.begin() + i);
}

// ---- Display ----

void Display::push_area() {
  inner_->push_area();
  if (last_area_.w <= 0 || last_area_.h <= 0) return;
  damage_ = damaged_ ? unite(damage_, last_area_) : last_area_;
  damaged_ = true;
}

bool Display::take_damage(rt::Rect& out) {
  if (!damaged_) return false;
  out = damage_;
  damaged_ = false;
  return true;
}

// ---- Board ----

bool Board::begin(const std::string& port, int baud, std::string& error) {
  if (!link_.begin(error)) return false;
  return line_.open(port, baud, error);
}

void Board::resize(int width, int height) {
  if (width == display_.width() && height == display_.height()) return;
  display_.resize(width, height);
  if (board_w_ > 0) {
    build_maps();
    keyframe();
  }
}

bool Board::input(rt::Input& out) {
  if (inputs_.empty()) return false;
  out = inputs_.front();
  inputs_.pop_front();
  return true;
}

void Board::push_key(const char* key) {
  if (input_at_us_ < 0) input_at_us_ = micros();
  rt::Input in;
  in.k = rt::Input::K::Key;
  in.key = key;
  inputs_.push_back(in);
}

std::vector<rt::Counter> Board::counters() const {
  return {
      {"stream_areas", static_cast<int64_t>(stats_.areas)},
      {"stream_bytes_out", static_cast<int64_t>(line_.bytes_out())},
      {"stream_rtt_us", static_cast<int64_t>(stats_.last_rtt_ms * 1000)},
  };
}

bool Board::wall_clock(int64_t& epoch_ms, int& utc_offset) const {
  const auto now = std::chrono::system_clock::now();
  epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  localtime_r(&t, &local);
  utc_offset = static_cast<int>(local.tm_gmtoff / 60);
  return true;
}

void Board::set_light(int percent) {
  line_.send(XS_LIGHT, {static_cast<uint8_t>(std::clamp(percent, 0, 100))});
}

void Board::send_plan(const std::vector<uint8_t>& blob) {
  int64_t epoch_ms = 0;
  int offset = 0;
  wall_clock(epoch_ms, offset);
  plan_payload_.assign(10, 0);
  std::memcpy(&plan_payload_[0], &epoch_ms, 8);
  const int16_t off16 = static_cast<int16_t>(offset);
  std::memcpy(&plan_payload_[8], &off16, 2);
  plan_payload_.insert(plan_payload_.end(), blob.begin(), blob.end());
  plan_mode_ = true;
  in_flight_ = false;
  has_pending_ = false;
  plan_sent_us_ = micros();
  line_.send(XS_PLAN, plan_payload_);
}

void Board::send_agent(const std::string& line) {
  line_.send(XS_AGENT, std::vector<uint8_t>(line.begin(), line.end()));
}

void Board::leave_plan() {
  plan_mode_ = false;
  keyframe();
}

void Board::keyframe() {
  shadow_valid_ = false;
  if (board_w_ > 0) {
    pending_ = {0, 0, board_w_, board_h_};
    has_pending_ = true;
  }
  ++stats_.keyframes;
}

void Board::build_maps() {
  auto build = [](int src, int dst, std::vector<Tap>& map) {
    map.assign(dst, Tap{});
    const double scale = double(src) / dst;
    for (int i = 0; i < dst; ++i) {
      const double a = i * scale, b = (i + 1) * scale;
      Tap& t = map[i];
      t.start = static_cast<int>(std::floor(a));
      const int end = std::min(src, static_cast<int>(std::ceil(b - 1e-9)));
      t.count = end - t.start;
      int total = 0;
      for (int s = t.start; s < end; ++s) {
        const double cover = std::min<double>(b, s + 1) - std::max<double>(a, s);
        int w = static_cast<int>(std::lround(cover / scale * 256));
        t.w.push_back(w);
        total += w;
      }
      // Weights sum to exactly 256.
      if (!t.w.empty()) t.w[std::max_element(t.w.begin(), t.w.end()) - t.w.begin()] += 256 - total;
    }
  };
  build(display_.width(), board_w_, xmap_);
  build(display_.height(), board_h_, ymap_);
}

void Board::resample(rt::Rect r, std::vector<uint16_t>& out) const {
  const uint8_t* src = display_.frame(nullptr);
  const int sw = display_.width();
  out.resize(static_cast<size_t>(r.w) * r.h);
  size_t o = 0;
  for (int y = r.y; y < r.y + r.h; ++y) {
    const Tap& ty = ymap_[y];
    for (int x = r.x; x < r.x + r.w; ++x) {
      const Tap& tx = xmap_[x];
      if (tx.count == 1 && ty.count == 1) {
        const uint8_t* p = src + (static_cast<size_t>(ty.start) * sw + tx.start) * 2;
        out[o++] = static_cast<uint16_t>((p[0] << 8) | p[1]);
        continue;
      }
      int rr = 0, gg = 0, bb = 0;
      for (int j = 0; j < ty.count; ++j) {
        const uint8_t* rowp = src + (static_cast<size_t>(ty.start + j) * sw) * 2;
        for (int i = 0; i < tx.count; ++i) {
          const uint8_t* p = rowp + (tx.start + i) * 2;
          const uint16_t c = static_cast<uint16_t>((p[0] << 8) | p[1]);
          const int w = ty.w[j] * tx.w[i];
          rr += ((c >> 11) & 0x1f) * w;
          gg += ((c >> 5) & 0x3f) * w;
          bb += (c & 0x1f) * w;
        }
      }
      const int half = 1 << 15;
      out[o++] = static_cast<uint16_t>((((rr + half) >> 16) << 11) | (((gg + half) >> 16) << 5) | ((bb + half) >> 16));
    }
  }
}

void Board::send_damage() {
  rt::Rect r = pending_;
  has_pending_ = false;
  std::vector<uint16_t> px;
  resample(r, px);
  const size_t n = px.size();
  // Ops against the shadow.
  std::vector<uint8_t> ops;
  ops.reserve(n / 2);
  auto op_head = [&](int kind, uint32_t count) {
    if (count <= 63) {
      ops.push_back(static_cast<uint8_t>((kind << 6) | (count - 1)));
    } else {
      ops.push_back(static_cast<uint8_t>((kind << 6) | 63));
      ops.push_back(static_cast<uint8_t>(count & 0xff));
      ops.push_back(static_cast<uint8_t>(count >> 8));
    }
  };
  auto shadow_at = [&](size_t i) -> uint16_t& {
    const int row = static_cast<int>(i / r.w), col = static_cast<int>(i % r.w);
    return shadow_[static_cast<size_t>(r.y + row) * board_w_ + r.x + col];
  };
  auto same = [&](size_t i) { return shadow_valid_ && shadow_at(i) == px[i]; };
  long lit_start = -1;
  auto flush_lit = [&](size_t end) {
    if (lit_start < 0) return;
    size_t s = static_cast<size_t>(lit_start);
    while (s < end) {
      const uint32_t c = static_cast<uint32_t>(std::min<size_t>(end - s, 65535));
      op_head(XS_OP_LIT, c);
      for (uint32_t k = 0; k < c; ++k) {
        ops.push_back(static_cast<uint8_t>(px[s + k] >> 8));
        ops.push_back(static_cast<uint8_t>(px[s + k] & 0xff));
      }
      s += c;
    }
    lit_start = -1;
  };
  size_t i = 0;
  while (i < n) {
    if (same(i)) {
      size_t j = i;
      while (j < n && same(j)) ++j;
      if (j - i >= 2 || lit_start < 0) {
        flush_lit(i);
        for (size_t s = i; s < j;) {
          const uint32_t c = static_cast<uint32_t>(std::min<size_t>(j - s, 65535));
          op_head(XS_OP_SKIP, c);
          s += c;
        }
        i = j;
        continue;
      }
    }
    size_t j = i + 1;
    while (j < n && px[j] == px[i]) ++j;
    if (j - i >= 3) {
      flush_lit(i);
      for (size_t s = i; s < j;) {
        const uint32_t c = static_cast<uint32_t>(std::min<size_t>(j - s, 65535));
        op_head(XS_OP_FILL, c);
        ops.push_back(static_cast<uint8_t>(px[i] >> 8));
        ops.push_back(static_cast<uint8_t>(px[i] & 0xff));
        s += c;
      }
      i = j;
      continue;
    }
    if (lit_start < 0) lit_start = static_cast<long>(i);
    ++i;
  }
  flush_lit(n);
  for (size_t k = 0; k < n; ++k) shadow_at(k) = px[k];
  shadow_valid_ = true;

  std::vector<uint8_t> payload;
  payload.reserve(XS_AREA_HEADER + std::min(ops.size(), n * 2));
  put16(payload, static_cast<uint16_t>(r.x));
  put16(payload, static_cast<uint16_t>(r.y));
  put16(payload, static_cast<uint16_t>(r.w));
  put16(payload, static_cast<uint16_t>(r.h));
  put16(payload, ++seq_);
  if (ops.size() <= n * 2) {
    payload.push_back(XS_ENC_OPS);
    payload.insert(payload.end(), ops.begin(), ops.end());
  } else {
    payload.push_back(XS_ENC_RAW);
    for (uint16_t c : px) {
      payload.push_back(static_cast<uint8_t>(c >> 8));
      payload.push_back(static_cast<uint8_t>(c & 0xff));
    }
  }
  line_.send(XS_AREA, payload);
  in_flight_ = true;
  sent_us_ = micros();
  if (input_at_us_ >= 0 && input_area_sent_us_ < 0) input_area_sent_us_ = sent_us_;
  ++stats_.areas;
  stats_.px_sent += n;
  stats_.bytes_sent += payload.size() + 10;
  stats_.last_w = r.w;
  stats_.last_h = r.h;
  stats_.last_bytes = static_cast<uint32_t>(payload.size() + 10);
}

void Board::handle(uint8_t type, const std::vector<uint8_t>& p) {
  if (type == XS_INFO && p.size() >= 8 && plan_mode_) {
    std::fprintf(stderr, "board: restarted; sending the plan again\n");
    board_w_ = get16(&p[0]), board_h_ = get16(&p[2]);
    shadow_.assign(static_cast<size_t>(board_w_) * board_h_, 0);
    build_maps();
    plan_sent_us_ = micros();
    line_.send(XS_PLAN, plan_payload_);
  } else if (type == XS_INFO && p.size() >= 8) {
    const int w = get16(&p[0]), h = get16(&p[2]);
    std::fprintf(stderr, "board: %dx%d, protocol %d\n", w, h, get16(&p[4]));
    board_w_ = w;
    board_h_ = h;
    shadow_.assign(static_cast<size_t>(w) * h, 0);
    build_maps();
    in_flight_ = false;
    keyframe();
  } else if (type == XS_ACK && p.size() >= 11) {
    const uint16_t seq = get16(&p[0]);
    if (!in_flight_ || seq != seq_) return;
    in_flight_ = false;
    const double rtt = (micros() - sent_us_) / 1000.0;
    stats_.last_rtt_ms = rtt;
    stats_.last_decode_ms = get32(&p[3]) / 1000.0;
    stats_.last_push_ms = get32(&p[7]) / 1000.0;
    stats_.rtts.push_back(rtt);
    if (input_area_sent_us_ >= 0 && sent_us_ == input_area_sent_us_) {
      stats_.input_to_panel.push_back((micros() - input_at_us_) / 1000.0);
      input_at_us_ = input_area_sent_us_ = -1;
    }
    if (p[2]) {
      ++stats_.acks;
    } else {
      ++stats_.nacks;
      keyframe();
    }
  } else if (type == XS_INPUT && p.size() >= 6) {
    const uint8_t kind = p[0], key = p[5];
    const int x = static_cast<int16_t>(get16(&p[1])), y = static_cast<int16_t>(get16(&p[3]));
    if (kind == XS_IN_KEY) {
      if (key == XS_KEY_NEXT_APP) {
        next_app_ = true;
      } else if (const char* name = key_name(key)) {
        push_key(name);
      }
      return;
    }
    if (board_w_ <= 0) return;
    if (input_at_us_ < 0) input_at_us_ = micros();
    rt::Input in;
    in.k = kind == XS_IN_DOWN ? rt::Input::K::Down : kind == XS_IN_MOVE ? rt::Input::K::Move : rt::Input::K::Up;
    in.x = std::clamp(x, 0, board_w_ - 1) * display_.width() / board_w_;
    in.y = std::clamp(y, 0, board_h_ - 1) * display_.height() / board_h_;
    inputs_.push_back(in);
  } else if (type == XS_LOG) {
    const std::string line(p.begin(), p.end());
    if (plan_mode_ && line.rfind("CE_", 0) == 0) {
      link_.write_line(line, true);  // the board's agent, for tools/agent.py
      if (line.rfind("CE_SHOT", 0) != 0) std::printf("BOARD %s\n", line.c_str());
      std::fflush(stdout);
    } else {
      std::fprintf(stderr, "board: %s\n", line.c_str());
    }
  } else if (type == XS_PLAN_OK && p.size() >= 17) {
    const double total = (micros() - plan_sent_us_) / 1000.0;
    std::printf("PLAN_OK ok=%d bytes=%zu sent_to_running=%.1fms load=%.1fms boot=%.1fms internal_free=%u psram_free=%u %.*s\n",
                p[0], plan_payload_.size(), total, get32(&p[1]) / 1000.0, get32(&p[5]) / 1000.0, get32(&p[9]),
                get32(&p[13]), static_cast<int>(p.size() - 17), reinterpret_cast<const char*>(p.data() + 17));
    std::fflush(stdout);
  }
}

void Board::step(uint32_t) {
  std::vector<std::pair<uint8_t, std::vector<uint8_t>>> packets;
  line_.poll(packets);
  for (auto& [type, payload] : packets) handle(type, payload);
  const int64_t now = micros();
  if (!connected()) {
    if (now - last_hello_us_ >= kHelloEveryUs) {
      line_.send(XS_HELLO, {});
      last_hello_us_ = now;
    }
    rt::Rect dropped;
    display_.take_damage(dropped);  // the keyframe on INFO covers it
    return;
  }
  if (plan_mode_) {
    rt::Rect dropped;
    display_.take_damage(dropped);
    // The agent PTY's requests go to the board.
    for (int c = link_.read(); c >= 0; c = link_.read()) {
      if (c == '\n' || c == '\r') {
        if (!pty_line_.empty()) send_agent(pty_line_);
        pty_line_.clear();
      } else if (pty_line_.size() < 600) {
        pty_line_ += static_cast<char>(c);
      }
    }
    return;
  }
  if (in_flight_ && now - sent_us_ > kAckTimeoutUs) {
    in_flight_ = false;
    ++stats_.timeouts;
    keyframe();
  }
  rt::Rect d;
  if (display_.take_damage(d)) {
    // App pixels to board pixels, one pixel wider for the resampling's reach.
    const int sw = display_.width(), sh = display_.height();
    int x0 = d.x * board_w_ / sw - 1, y0 = d.y * board_h_ / sh - 1;
    int x1 = ((d.x + d.w) * board_w_ + sw - 1) / sw + 1, y1 = ((d.y + d.h) * board_h_ + sh - 1) / sh + 1;
    x0 = std::max(0, x0), y0 = std::max(0, y0);
    x1 = std::min(board_w_, x1), y1 = std::min(board_h_, y1);
    if (x1 > x0 && y1 > y0) {
      const rt::Rect b{x0, y0, x1 - x0, y1 - y0};
      pending_ = has_pending_ ? unite(pending_, b) : b;
      has_pending_ = true;
    }
  }
  uint32_t surface = 0;
  display_.frame(&surface);
  const bool ready = surface >= static_cast<uint32_t>(display_.width() * display_.height() * 2);
  if (!in_flight_ && has_pending_ && ready) send_damage();
}

void Board::idle() {
  pollfd p[1] = {{line_.fd(), POLLIN, 0}};
  ::poll(p, 1, 1);
  link_.wait(0);
}

std::string Board::stats_line() const {
  std::vector<double> r = stats_.rtts;
  double median = 0, p90 = 0;
  if (!r.empty()) {
    std::sort(r.begin(), r.end());
    median = r[r.size() / 2];
    p90 = r[std::min(r.size() - 1, r.size() * 9 / 10)];
  }
  std::vector<double> k = stats_.input_to_panel;
  double kmed = 0, kmin = 0, kmax = 0;
  if (!k.empty()) {
    std::sort(k.begin(), k.end());
    kmed = k[k.size() / 2], kmin = k.front(), kmax = k.back();
  }
  char kbuf[120];
  std::snprintf(kbuf, sizeof kbuf, " input_to_panel n=%zu median=%.1fms min=%.1fms max=%.1fms", k.size(), kmed, kmin, kmax);
  char buf[320];
  std::snprintf(buf, sizeof buf,
                "areas=%llu acks=%llu nacks=%llu keyframes=%llu timeouts=%llu px=%llu bytes=%llu "
                "rtt_median=%.1fms rtt_p90=%.1fms last=%dx%d/%uB rtt=%.1fms decode=%.1fms push=%.1fms",
                static_cast<unsigned long long>(stats_.areas), static_cast<unsigned long long>(stats_.acks),
                static_cast<unsigned long long>(stats_.nacks), static_cast<unsigned long long>(stats_.keyframes),
                static_cast<unsigned long long>(stats_.timeouts), static_cast<unsigned long long>(stats_.px_sent),
                static_cast<unsigned long long>(stats_.bytes_sent), median, p90, stats_.last_w, stats_.last_h,
                stats_.last_bytes, stats_.last_rtt_ms, stats_.last_decode_ms, stats_.last_push_ms);
  return std::string(buf) + kbuf;
}

}  // namespace xs
