// SenseCAP Watcher as a thin client for Exact apps that run on a host.
//
// The board keeps a copy of its screen (412 x 412 RGB565, big-endian, in
// PSRAM), applies each AREA packet the host sends to it, and pushes the changed
// rectangle to the SPD2010 panel over QSPI. Touch (SPD2010, I2C), the knob's
// rotation (GPIO 41/42) and its button (PCA9535 P0.3) go back as INPUT
// packets. No app code runs here: the app, its layout and its paint are the
// host's, so a new app or a changed one needs no flash.
//
// Plan mode: a PLAN packet carries a whole app as a plan blob (its rt::App
// tables and its code as rt_vm bytecode). The board then runs it itself, on
// the same exact_rt core as the Stopwatch's Plan B firmware, with the bytecode
// interpreter in place of compiled C++; inputs go to the app here, and the host
// only sends a new plan when it wants another app. An AREA packet returns the
// board to thin-client mode.
//
// The knob: a detent is ArrowLeft or ArrowRight; a click is Enter; a hold of
// 600 ms is Escape; a hold of 2 s asks the host for its next app.
#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_rom_crc.h>
#include <esp_timer.h>
#include <freertos/semphr.h>

#include <deque>
#include <memory>
#include <string>

#include "esp_lcd_spd2010.h"
#include "exact_rt.h"
#include "exact_stream_proto.h"
#include "src/fonts/gfx_fonts.h"
#include "src/vm/plan_blob.h"

SET_LOOP_TASK_STACK_SIZE(32 * 1024);

namespace {

constexpr int kW = 412;
constexpr int kH = 412;

// Pins (Seeed's sensecap-watcher BSP).
constexpr int kQspiClk = 7, kQspiD0 = 9, kQspiD1 = 1, kQspiD2 = 14, kQspiD3 = 13;
constexpr int kLcdCs = 45, kLcdBacklight = 8;
constexpr int kTouchSda = 39, kTouchScl = 38;
constexpr int kExpSda = 47, kExpScl = 48;
constexpr int kKnobA = 41, kKnobB = 42;

constexpr uint8_t kExpAddr = 0x21;    // PCA9535
constexpr uint8_t kTouchAddr = 0x53;  // SPD2010 touch
// PCA9535 port 1 outputs: P1.2 system power, P1.1 LCD power.
constexpr uint8_t kPort1System = 1 << 2;
constexpr uint8_t kPort1Lcd = 1 << 1;
constexpr uint8_t kKnobButtonBit = 1 << 3;  // port 0, active low

constexpr int kBandRows = 24;  // rows per DMA bounce buffer

uint8_t* fb = nullptr;  // kW * kH * 2, big-endian RGB565
uint8_t* bounce[2] = {nullptr, nullptr};
SemaphoreHandle_t bounce_free = nullptr;  // counts free bounce buffers
esp_lcd_panel_handle_t panel = nullptr;

// ---- Small helpers ----

void put16(uint8_t* p, uint16_t v) { p[0] = v & 0xff, p[1] = v >> 8; }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 0xff; }
uint16_t get16(const uint8_t* p) { return p[0] | (p[1] << 8); }

void send_packet(uint8_t type, const uint8_t* payload, uint32_t len) {
  uint8_t head[6] = {XS_MAGIC0, XS_MAGIC1, type, uint8_t(len), uint8_t(len >> 8), uint8_t(len >> 16)};
  uint32_t crc = esp_rom_crc32_le(0, head + 2, 4);
  if (len) crc = esp_rom_crc32_le(crc, payload, len);
  uint8_t tail[4];
  put32(tail, crc);
  Serial.write(head, 6);
  if (len) Serial.write(payload, len);
  Serial.write(tail, 4);
}

void send_log(const char* text) { send_packet(XS_LOG, reinterpret_cast<const uint8_t*>(text), strlen(text)); }

void send_info() {
  uint8_t p[8];
  put16(p, kW), put16(p + 2, kH), put16(p + 4, XS_VERSION), put16(p + 6, 0);
  send_packet(XS_INFO, p, sizeof p);
}

void send_input(uint8_t kind, int x, int y, uint8_t key) {
  uint8_t p[6] = {kind, 0, 0, 0, 0, key};
  put16(p + 1, uint16_t(int16_t(x))), put16(p + 3, uint16_t(int16_t(y)));
  send_packet(XS_INPUT, p, sizeof p);
}

// ---- IO expander ----

bool exp_write(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kExpAddr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

int exp_read(uint8_t reg) {
  Wire.beginTransmission(kExpAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(int(kExpAddr), 1) != 1) return -1;
  return Wire.read();
}

bool power_up() {
  // Hold the panel's and touch's lines low until the panel has power, as the
  // BSP does, so the panel is not powered through its inputs.
  for (int pin : {kTouchSda, kTouchScl, kQspiClk, kQspiD0, kQspiD1, kQspiD2, kQspiD3, kLcdCs, kLcdBacklight}) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }
  Wire.begin(kExpSda, kExpScl, 400000);
  // Outputs first, then direction: system power never drops.
  if (!exp_write(0x03, kPort1System)) return false;
  exp_write(0x02, 0x00);
  exp_write(0x06, 0xff);  // port 0: inputs
  exp_write(0x07, 0x20);  // port 1: P1.5 input, the rest outputs
  delay(100);
  exp_write(0x03, kPort1System | kPort1Lcd);
  delay(50);
  return true;
}

// ---- Panel ----

bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(bounce_free, &woken);
  return woken == pdTRUE;
}

bool panel_begin() {
  spi_bus_config_t bus = {};
  bus.sclk_io_num = kQspiClk;
  bus.data0_io_num = kQspiD0;
  bus.data1_io_num = kQspiD1;
  bus.data2_io_num = kQspiD2;
  bus.data3_io_num = kQspiD3;
  bus.data4_io_num = bus.data5_io_num = bus.data6_io_num = bus.data7_io_num = -1;
  bus.max_transfer_sz = kW * kBandRows * 2;
  if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

  esp_lcd_panel_io_spi_config_t io_cfg = {};
  io_cfg.cs_gpio_num = kLcdCs;
  io_cfg.dc_gpio_num = -1;
  io_cfg.spi_mode = 3;
  io_cfg.pclk_hz = 40 * 1000 * 1000;
  io_cfg.trans_queue_depth = 4;
  io_cfg.lcd_cmd_bits = 32;
  io_cfg.lcd_param_bits = 8;
  io_cfg.flags.quad_mode = 1;
  io_cfg.on_color_trans_done = on_color_done;
  esp_lcd_panel_io_handle_t io = nullptr;
  if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &io_cfg, &io) != ESP_OK) return false;

  static spd2010_vendor_config_t vendor = {};
  vendor.flags.use_qspi_interface = 1;
  esp_lcd_panel_dev_config_t dev = {};
  dev.reset_gpio_num = -1;
  dev.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  dev.bits_per_pixel = 16;
  dev.vendor_config = &vendor;
  if (esp_lcd_new_panel_spd2010(io, &dev, &panel) != ESP_OK) return false;
  esp_lcd_panel_reset(panel);
  if (esp_lcd_panel_init(panel) != ESP_OK) return false;
  esp_lcd_panel_mirror(panel, false, false);
  esp_lcd_panel_disp_on_off(panel, true);
  return true;
}

// Pushes a rectangle of `fb` to the panel through the bounce buffers. The
// SPD2010 takes columns in fours, so the rectangle grows to whole fours.
void push_rect(int x, int y, int w, int h) {
  int x0 = x & ~3, x1 = (x + w + 3) & ~3;
  int y0 = y & ~1, y1 = (y + h + 1) & ~1;
  if (x1 > kW) x1 = kW;
  if (y1 > kH) y1 = kH;
  if (x0 >= x1 || y0 >= y1) return;
  const int bw = (x1 - x0) * 2;
  const int rows_per = (kW * kBandRows) / (x1 - x0);
  int which = 0;
  for (int yy = y0; yy < y1;) {
    int n = y1 - yy < rows_per ? y1 - yy : rows_per;
    xSemaphoreTake(bounce_free, portMAX_DELAY);
    uint8_t* out = bounce[which];
    for (int r = 0; r < n; ++r) memcpy(out + r * bw, fb + ((yy + r) * kW + x0) * 2, bw);
    esp_lcd_panel_draw_bitmap(panel, x0, yy, x1, yy + n, out);
    which ^= 1;
    yy += n;
  }
  // Wait for both buffers to come back, so the next decode may reuse them.
  xSemaphoreTake(bounce_free, portMAX_DELAY);
  xSemaphoreTake(bounce_free, portMAX_DELAY);
  xSemaphoreGive(bounce_free);
  xSemaphoreGive(bounce_free);
}

void backlight(int percent) {
  ledcWrite(kLcdBacklight, (percent * 1023) / 100);
}

// ---- Touch (SPD2010) ----

bool touch_write(const uint8_t* d, int n) {
  Wire1.beginTransmission(kTouchAddr);
  Wire1.write(d, n);
  bool ok = Wire1.endTransmission() == 0;
  delayMicroseconds(200);
  return ok;
}

bool touch_read(uint8_t reg_hi, uint8_t reg_lo, uint8_t* out, int n) {
  uint8_t reg[2] = {reg_hi, reg_lo};
  if (!touch_write(reg, 2)) return false;
  if (Wire1.requestFrom(int(kTouchAddr), n) != n) return false;
  for (int i = 0; i < n; ++i) out[i] = Wire1.read();
  delayMicroseconds(200);
  return true;
}

void touch_cmd(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  uint8_t cmd[4] = {a, b, c, d};
  touch_write(cmd, 4);
}

// One poll: true with a point while a finger is down.
bool touch_poll(bool& down, int& x, int& y) {
  uint8_t st[4];
  if (!touch_read(0x20, 0x00, st, 4)) return false;
  const bool pt_exist = st[0] & 0x01, gesture = st[0] & 0x02, aux = st[0] & 0x08;
  const bool in_bios = st[1] & 0x40, in_cpu = st[1] & 0x20, cpu_run = st[1] & 0x08;
  const uint16_t len = st[2] | (st[3] << 8);
  if (in_bios) {
    touch_cmd(0x02, 0x00, 0x01, 0x00);
    touch_cmd(0x04, 0x00, 0x01, 0x00);
    return false;
  }
  if (in_cpu) {
    touch_cmd(0x50, 0x00, 0x00, 0x00);
    touch_cmd(0x46, 0x00, 0x00, 0x00);
    touch_cmd(0x02, 0x00, 0x01, 0x00);
    return false;
  }
  if (cpu_run && len == 0) {
    touch_cmd(0x02, 0x00, 0x01, 0x00);
    return false;
  }
  if (pt_exist || gesture) {
    uint8_t d[64];
    if (len < 5 || len > sizeof d) return false;
    if (!touch_read(0x00, 0x03, d, len)) return false;
    bool got = false;
    if (d[4] <= 0x0a && pt_exist && (len - 4) % 6 == 0 && len >= 10) {
      x = ((d[7] & 0xf0) << 4) | d[5];
      y = ((d[7] & 0x0f) << 8) | d[6];
      down = d[8] != 0;
      got = true;
    }
    for (int guard = 0; guard < 4; ++guard) {
      uint8_t hs[8];
      if (!touch_read(0xfc, 0x02, hs, 8)) break;
      if (hs[5] == 0x82) {
        touch_cmd(0x02, 0x00, 0x01, 0x00);
        break;
      }
      if (hs[5] != 0x00) break;
      uint16_t next = hs[2] | (hs[3] << 8);
      if (next == 0 || next > sizeof d) break;
      touch_read(0x00, 0x03, d, next);
    }
    return got;
  }
  if (cpu_run && aux) touch_cmd(0x02, 0x00, 0x01, 0x00);
  return false;
}

// ---- Knob ----

volatile int32_t knob_count = 0;
volatile uint8_t knob_state = 0;

void IRAM_ATTR knob_isr() {
  static const int8_t kTable[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  uint8_t s = (uint8_t(gpio_get_level(gpio_num_t(kKnobA))) << 1) | uint8_t(gpio_get_level(gpio_num_t(kKnobB)));
  knob_count += kTable[(knob_state << 2) | s];
  knob_state = s;
}

// ---- Plan mode: the app runs here ----

// The screen as an rt::Display: `fb` is the drawing surface (one area at a
// time, the whole screen tall), kept big-endian as the panel takes it. Text is
// the Stopwatch's FreeSans, drawn with the integer algorithms of M5GFX 0.2.26
// as exact-cpp's Mac board draws them (rt-mac/rt_mac.cpp), so the Watcher's
// pixels match the Mac's.
namespace gfx = rt_mac::gfx;

const gfx::Font& font_of(int face) {
  const gfx::Face& f = gfx::kFaces[face / 2];
  return face % 2 ? *f.bold : *f.regular;
}

const gfx::Glyph* glyph_of(const gfx::Font& f, uint32_t code) {
  if (code < f.first || code > f.last) return nullptr;
  return &f.glyph[code - f.first];
}

const gfx::Glyph& metric_of(const gfx::Font& f, uint32_t code) {
  const gfx::Glyph* g = glyph_of(f, code);
  return g ? *g : *glyph_of(f, 0x20);
}

std::vector<uint32_t> code_points(const char* text) {
  std::vector<uint32_t> out;
  for (const unsigned char* q = reinterpret_cast<const unsigned char*>(text); *q;) {
    uint32_t c = *q++;
    int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (more) c &= 0x3fu >> more;
    for (; more > 0 && (*q & 0xc0) == 0x80; --more) c = (c << 6) | (*q++ & 0x3f);
    if (c >= 0x20) out.push_back(c);
  }
  return out;
}

void default_metric(const gfx::Font& f, int& baseline, int& height) {
  int above = 0, below = 0;
  const size_t count = static_cast<size_t>(f.last - f.first);
  size_t c = 0;
  do {
    int ab = -f.glyph[c].yOffset;
    above = above > ab ? above : ab;
    const int bl = f.glyph[c].height - ab;
    below = below > bl ? below : bl;
  } while (++c < count);
  baseline = above;
  height = above + below;
}

class PlanDisplay final : public rt::Display {
 public:
  bool begin() override { return fb != nullptr; }
  int width() const override { return kW; }
  int height() const override { return kH; }
  int band_rows() const override { return kH; }
  void begin_area(rt::Rect area) override { area_ = area; }
  void push_area() override { push_rect(area_.x, area_.y, area_.w, area_.h); }
  void fill_rect(rt::Rect r, uint32_t rgb) override {
    color_ = swapped(rgb);
    if (r.w < 0) r.x += r.w, r.w = -r.w;
    if (r.h < 0) r.y += r.h, r.h = -r.h;
    fill(r.x, r.y, r.w, r.h);
  }
  int face(float px, int weight) const override {
    int best = 0;
    for (int i = 1; i < 4; ++i) {
      if (fabsf(gfx::kFaces[i].px - px) < fabsf(gfx::kFaces[best].px - px)) best = i;
    }
    return best * 2 + (weight >= 600 ? 1 : 0);
  }
  int text_width(int face, const char* text) const override {
    const gfx::Font& f = font_of(face);
    int left = 0, right = 0;
    for (uint32_t c : code_points(text)) {
      const gfx::Glyph& g = metric_of(f, c);
      if (left == 0 && right == 0 && g.xOffset < 0) left = right = -g.xOffset;
      const int span = g.width + g.xOffset;
      right = left + (g.xAdvance > span ? g.xAdvance : span);
      left += g.xAdvance;
    }
    return right;
  }
  int line_height(int face) const override {
    int baseline = 0, h = 0;
    default_metric(font_of(face), baseline, h);
    return h;
  }
  void draw_text(int face, const char* text, int x, int y, uint32_t rgb) override {
    color_ = swapped(rgb);
    const gfx::Font& f = font_of(face);
    int baseline = 0, h = 0;
    default_metric(f, baseline, h);
    std::vector<uint32_t> codes = code_points(text);
    if (codes.empty()) return;
    int cursor = x;
    const gfx::Glyph& first = metric_of(f, codes[0]);
    if (first.xOffset < 0) cursor -= first.xOffset;
    for (uint32_t c : codes) {
      const gfx::Glyph* g = glyph_of(f, c);
      if (!g) {
        const int w = metric_of(f, 0x20).xAdvance;
        if (w > 2 && h > 2) {
          const int bx = cursor + 1, by = y + 1, bw = w - 2, bh = h - 2;
          fill(bx, by, bw, 1);
          fill(bx, by + bh - 1, bw, 1);
          fill(bx, by, 1, bh);
          fill(bx + bw - 1, by, 1, bh);
        }
        cursor += w;
        continue;
      }
      const uint8_t* bits = &f.bitmap[g->bitmapOffset];
      const int left = cursor + g->xOffset, top = y + baseline + g->yOffset;
      for (int k = 0; k < g->width * g->height; ++k) {
        if ((bits[k >> 3] >> (7 - (k & 7))) & 1) fill(left + k % g->width, top + k / g->width, 1, 1);
      }
      cursor += g->xAdvance;
    }
  }
  const uint8_t* frame(uint32_t* bytes) const override {
    if (bytes) *bytes = kW * kH * 2;
    return fb;
  }
  uint16_t* row(int y) override {
    if (y < area_.y || y >= area_.y + area_.h) return nullptr;
    return reinterpret_cast<uint16_t*>(fb) + static_cast<size_t>(y) * kW;
  }
  bool big_endian() const override { return true; }

 private:
  // RGB565 with its bytes swapped: written as a uint16 it lands big-endian.
  static uint16_t swapped(uint32_t rgb) {
    const uint16_t c = ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f);
    return uint16_t((c >> 8) | (c << 8));
  }
  void fill(int x, int y, int w, int h) {
    int x0 = x > area_.x ? x : area_.x, y0 = y > area_.y ? y : area_.y;
    int x1 = x + w < area_.x + area_.w ? x + w : area_.x + area_.w;
    int y1 = y + h < area_.y + area_.h ? y + h : area_.y + area_.h;
    for (int yy = y0; yy < y1; ++yy) {
      uint16_t* r = reinterpret_cast<uint16_t*>(fb) + static_cast<size_t>(yy) * kW;
      for (int xx = x0; xx < x1; ++xx) r[xx] = color_;
    }
  }
  rt::Rect area_{0, 0, kW, kH};
  uint16_t color_ = 0;
};

// The agent's channel: request lines arrive in AGENT packets, answers leave
// as LOG packets.
class PlanLink final : public rt::Link {
 public:
  int read() override {
    if (in_.empty()) return -1;
    const int c = static_cast<unsigned char>(in_.front());
    in_.pop_front();
    return c;
  }
  void write_line(const std::string& line, bool) override {
    send_packet(XS_LOG, reinterpret_cast<const uint8_t*>(line.data()), line.size());
  }
  void push(const char* text, size_t n) {
    in_.insert(in_.end(), text, text + n);
    in_.push_back('\n');
  }

 private:
  std::deque<char> in_;
};

class PlanBoard final : public rt::Board {
 public:
  int64_t micros() const override { return esp_timer_get_time(); }
  rt::Display* display() override { return &display_; }
  rt::Link* link() override { return &link_; }
  rt::Inputs inputs() const override { return {true, true}; }
  bool input(rt::Input& out) override {
    if (inputs_.empty()) return false;
    out = inputs_.front();
    inputs_.pop_front();
    return true;
  }
  std::vector<rt::Counter> counters() const override {
    return {
        {"internal_free", static_cast<int64_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL))},
        {"psram_free", static_cast<int64_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM))},
        {"loop_stack_free", static_cast<int64_t>(uxTaskGetStackHighWaterMark(nullptr))},
    };
  }
  bool wall_clock(int64_t& epoch_ms, int& utc_offset) const override {
    if (!clock_set_) return false;
    epoch_ms = epoch_ms_ + (micros() - clock_at_us_) / 1000;
    utc_offset = utc_offset_;
    return true;
  }
  void set_clock(int64_t epoch_ms, int utc_offset) {
    clock_set_ = epoch_ms > 0;
    epoch_ms_ = epoch_ms;
    utc_offset_ = utc_offset;
    clock_at_us_ = micros();
  }
  void key(const char* name) {
    rt::Input in;
    in.k = rt::Input::K::Key;
    in.key = name;
    inputs_.push_back(in);
  }
  void pointer(rt::Input::K k, int x, int y) {
    rt::Input in;
    in.k = k, in.x = x, in.y = y;
    inputs_.push_back(in);
  }
  PlanLink& agent() { return link_; }
  void clear() { inputs_.clear(); }

 private:
  PlanDisplay display_;
  PlanLink link_;
  std::deque<rt::Input> inputs_;
  bool clock_set_ = false;
  int64_t epoch_ms_ = 0, clock_at_us_ = 0;
  int utc_offset_ = 0;
};

PlanBoard plan_board;
std::unique_ptr<plan_blob::Loaded> plan_loaded;
std::unique_ptr<rt::Host> plan_host;
uint8_t* plan_bytes = nullptr;

void send_plan_ok(bool ok, uint32_t load_us, uint32_t boot_us, const char* text) {
  std::vector<uint8_t> p(17);
  p[0] = ok ? 1 : 0;
  put32(&p[1], load_us);
  put32(&p[5], boot_us);
  put32(&p[9], heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  put32(&p[13], heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  p.insert(p.end(), text, text + strlen(text));
  send_packet(XS_PLAN_OK, p.data(), p.size());
}

void stop_plan() {
  plan_host.reset();
  plan_loaded.reset();
  if (plan_bytes) heap_caps_free(plan_bytes);
  plan_bytes = nullptr;
  plan_board.clear();
}

// `bytes` (heap_caps_malloc'd, `len` long): epoch_ms, utc_offset, then the blob.
void start_plan(uint8_t* bytes, uint32_t len) {
  stop_plan();
  plan_bytes = bytes;
  int64_t epoch_ms = 0;
  int16_t offset = 0;
  memcpy(&epoch_ms, bytes, 8);
  memcpy(&offset, bytes + 8, 2);
  plan_board.set_clock(epoch_ms, offset);
  const int64_t t0 = esp_timer_get_time();
  plan_loaded = std::make_unique<plan_blob::Loaded>();
  std::string error;
  if (!plan_loaded->load(bytes + 10, len - 10, error)) {
    send_plan_ok(false, 0, 0, error.c_str());
    stop_plan();
    return;
  }
  const int64_t t1 = esp_timer_get_time();
  plan_host = std::make_unique<rt::Host>(plan_loaded->app(), plan_board, std::vector<rt::Extension*>{});
  const bool booted = plan_host->boot();
  const int64_t t2 = esp_timer_get_time();
  if (!booted) {
    send_plan_ok(false, uint32_t(t1 - t0), 0, "the plan did not boot");
    stop_plan();
    return;
  }
  send_plan_ok(true, uint32_t(t1 - t0), uint32_t(t2 - t1), "");
}

void agent_line(const char* text, size_t n) {
  if (plan_host) plan_board.agent().push(text, n);
}

// ---- The packet reader ----

enum class RxState : uint8_t { Magic0, Magic1, Head, Payload, Crc };

struct Rx {
  RxState state = RxState::Magic0;
  uint8_t head[4];
  int head_n = 0;
  uint32_t len = 0, got = 0, crc = 0;
  uint8_t type = 0;
  uint8_t crc_bytes[4];
  int crc_n = 0;
  // Small packets (an AGENT line) and the AREA header.
  uint8_t small[640];
  // A PLAN's payload, in PSRAM.
  uint8_t* plan = nullptr;
  // The AREA being decoded.
  int ax = 0, ay = 0, aw = 0, ah = 0, aseq = 0, aenc = 0;
  bool area_ok = false;
  uint32_t px = 0, px_total = 0;  // pixels placed
  // The op being decoded.
  uint8_t op = 0, op_need = 0;  // op_need: bytes of count/color still to read
  uint8_t op_bytes[3];
  int op_have = 0;
  uint32_t op_left = 0;  // pixels left in the op
  uint8_t lit_half = 0, lit_hi = 0;
  bool in_op = false;
  int64_t started_us = 0;
} rx;

inline uint8_t* fb_at(uint32_t i) {
  const uint32_t row = i / rx.aw, col = i - row * rx.aw;
  return fb + ((rx.ay + row) * kW + rx.ax + col) * 2;
}

// Feeds AREA pixel data (after the header) to the decoder.
void area_bytes(const uint8_t* p, uint32_t n) {
  if (!rx.area_ok) return;
  uint32_t i = 0;
  if (rx.aenc == XS_ENC_RAW) {
    while (i < n && rx.px < rx.px_total) {
      if (!rx.lit_half) {
        rx.lit_hi = p[i++];
        rx.lit_half = 1;
      } else {
        uint8_t* d = fb_at(rx.px++);
        d[0] = rx.lit_hi, d[1] = p[i++];
        rx.lit_half = 0;
      }
    }
    return;
  }
  while (i < n) {
    if (!rx.in_op) {
      rx.op = p[i] >> 6;
      uint8_t c = p[i] & 0x3f;
      ++i;
      rx.op_have = 0;
      if (c == 63) {
        rx.op_need = 2;
        rx.op_left = 0;
      } else {
        rx.op_need = 0;
        rx.op_left = c + 1;
      }
      if (rx.op == XS_OP_FILL) rx.op_need += 2;
      rx.in_op = true;
      rx.lit_half = 0;
    }
    while (rx.op_need && i < n) {
      rx.op_bytes[rx.op_have++] = p[i++];
      --rx.op_need;
    }
    if (rx.op_need) return;
    if (rx.op_have >= 2 && rx.op_left == 0) rx.op_left = get16(rx.op_bytes);
    if (rx.op_left > rx.px_total - rx.px) {  // a bad count: drop the area
      rx.area_ok = false;
      return;
    }
    if (rx.op == XS_OP_SKIP) {
      rx.px += rx.op_left;
      rx.op_left = 0;
    } else if (rx.op == XS_OP_FILL) {
      const uint8_t hi = rx.op_bytes[rx.op_have - 2], lo = rx.op_bytes[rx.op_have - 1];
      while (rx.op_left) {
        // A run along one row at a time.
        const uint32_t col = rx.px % rx.aw;
        uint32_t run = rx.aw - col;
        if (run > rx.op_left) run = rx.op_left;
        uint8_t* d = fb_at(rx.px);
        for (uint32_t k = 0; k < run; ++k) d[2 * k] = hi, d[2 * k + 1] = lo;
        rx.px += run;
        rx.op_left -= run;
      }
    } else if (rx.op == XS_OP_LIT) {
      while (rx.op_left && i < n) {
        if (!rx.lit_half && i + 1 < n) {
          // Whole pixels along the row.
          const uint32_t col = rx.px % rx.aw;
          uint32_t run = rx.aw - col;
          if (run > rx.op_left) run = rx.op_left;
          const uint32_t avail = (n - i) / 2;
          if (run > avail) run = avail;
          memcpy(fb_at(rx.px), p + i, run * 2);
          i += run * 2;
          rx.px += run;
          rx.op_left -= run;
        } else if (!rx.lit_half) {
          rx.lit_hi = p[i++];
          rx.lit_half = 1;
        } else {
          uint8_t* d = fb_at(rx.px++);
          d[0] = rx.lit_hi, d[1] = p[i++];
          rx.lit_half = 0;
          --rx.op_left;
        }
      }
      if (rx.op_left) return;
    } else {
      rx.area_ok = false;
      return;
    }
    rx.in_op = false;
  }
}

void finish_packet(bool crc_ok) {
  if (rx.type == XS_HELLO) {
    if (crc_ok) send_info();
  } else if (rx.type == XS_PLAN) {
    if (crc_ok) {
      start_plan(rx.plan, rx.len);
      rx.plan = nullptr;  // the plan owns it now
    } else {
      send_plan_ok(false, 0, 0, "the plan's CRC failed");
    }
  } else if (rx.type == XS_AGENT) {
    if (crc_ok) agent_line(reinterpret_cast<const char*>(rx.small), rx.len);
  } else if (rx.type == XS_LIGHT) {
    if (crc_ok && rx.len >= 1) backlight(rx.small[0] > 100 ? 100 : rx.small[0]);
  } else if (rx.type == XS_AREA) {
    const bool ok = crc_ok && rx.area_ok && rx.px == rx.px_total;
    const int64_t decoded = esp_timer_get_time();
    if (ok) push_rect(rx.ax, rx.ay, rx.aw, rx.ah);
    const int64_t pushed = esp_timer_get_time();
    uint8_t p[11];
    put16(p, uint16_t(rx.aseq));
    p[2] = ok ? 1 : 0;
    put32(p + 3, uint32_t(decoded - rx.started_us));
    put32(p + 7, uint32_t(pushed - decoded));
    send_packet(XS_ACK, p, sizeof p);
  }
}

void begin_area_payload() {
  const uint8_t* h = rx.small;
  rx.ax = get16(h), rx.ay = get16(h + 2), rx.aw = get16(h + 4), rx.ah = get16(h + 6);
  rx.aseq = get16(h + 8), rx.aenc = h[10];
  rx.area_ok = rx.aw > 0 && rx.ah > 0 && rx.ax + rx.aw <= kW && rx.ay + rx.ah <= kH &&
               (rx.aenc == XS_ENC_RAW || rx.aenc == XS_ENC_OPS);
  rx.px = 0;
  rx.px_total = uint32_t(rx.aw) * rx.ah;
  rx.in_op = false;
  rx.lit_half = 0;
}

uint32_t rx_last_ms = 0;

void rx_bytes(const uint8_t* p, size_t n) {
  size_t i = 0;
  rx_last_ms = millis();
  while (i < n) {
    switch (rx.state) {
      case RxState::Magic0:
        if (p[i++] == XS_MAGIC0) rx.state = RxState::Magic1;
        break;
      case RxState::Magic1:
        if (p[i] == XS_MAGIC1) {
          rx.state = RxState::Head;
          rx.head_n = 0;
          ++i;
        } else {
          rx.state = RxState::Magic0;
        }
        break;
      case RxState::Head:
        rx.head[rx.head_n++] = p[i++];
        if (rx.head_n == 4) {
          rx.type = rx.head[0];
          rx.len = rx.head[1] | (rx.head[2] << 8) | (uint32_t(rx.head[3]) << 16);
          rx.crc = esp_rom_crc32_le(0, rx.head, 4);
          rx.got = 0;
          rx.crc_n = 0;
          rx.started_us = esp_timer_get_time();
          rx.area_ok = false;
          const bool known = rx.type == XS_HELLO || rx.type == XS_LIGHT || rx.type == XS_AREA ||
                             rx.type == XS_PLAN || rx.type == XS_AGENT;
          bool ok = known;
          if (rx.type == XS_AREA) ok = rx.len >= XS_AREA_HEADER;
          else if (rx.type == XS_PLAN) ok = rx.len > 10 && rx.len <= XS_MAX_PACKET;
          else ok = ok && rx.len <= sizeof rx.small;
          if (ok && rx.type == XS_PLAN) {
            // The running app stops now: a turn must not hold the loop while a
            // big plan streams in, or the UART's buffer overflows.
            stop_plan();
            if (rx.plan) heap_caps_free(rx.plan);
            rx.plan = static_cast<uint8_t*>(heap_caps_malloc(rx.len, MALLOC_CAP_SPIRAM));
            ok = rx.plan != nullptr;
          }
          if (ok && rx.type == XS_AREA) stop_plan();
          rx.state = !ok ? RxState::Magic0 : rx.len ? RxState::Payload : RxState::Crc;
        }
        break;
      case RxState::Payload: {
        uint32_t take = rx.len - rx.got;
        if (take > n - i) take = n - i;
        rx.crc = esp_rom_crc32_le(rx.crc, p + i, take);
        uint32_t k = 0;
        // The first bytes go to `small` (whole small packets; an AREA's header).
        if (rx.type == XS_PLAN) {
          memcpy(rx.plan + rx.got, p + i, take);
          rx.got += take;
          i += take;
          if (rx.got == rx.len) rx.state = RxState::Crc;
          break;
        }
        const uint32_t small_n = rx.type == XS_AREA ? XS_AREA_HEADER : rx.len;
        const uint32_t before = rx.got;
        while (rx.got < small_n && k < take) rx.small[rx.got++] = p[i + k++];
        if (rx.type == XS_AREA && before < XS_AREA_HEADER && rx.got == XS_AREA_HEADER) begin_area_payload();
        if (rx.type == XS_AREA && k < take) {
          area_bytes(p + i + k, take - k);
          rx.got += take - k;
        }
        i += take;
        if (rx.got == rx.len) rx.state = RxState::Crc;
        break;
      }
      case RxState::Crc:
        rx.crc_bytes[rx.crc_n++] = p[i++];
        if (rx.crc_n == 4) {
          const uint32_t want = rx.crc_bytes[0] | (rx.crc_bytes[1] << 8) | (rx.crc_bytes[2] << 16) |
                                (uint32_t(rx.crc_bytes[3]) << 24);
          finish_packet(want == rx.crc);
          rx.state = RxState::Magic0;
        }
        break;
    }
  }
}

// Draws a splash into `fb`: a dark dial and a ring, so a board with no host
// still shows it is alive.
void splash() {
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      const int dx = x - kW / 2, dy = y - kH / 2, d2 = dx * dx + dy * dy;
      uint16_t c = 0x0000;
      if (d2 < 200 * 200 && d2 > 192 * 192) c = 0x2a7f;  // a blue ring
      else if (d2 < 30 * 30) c = 0x4208;
      fb[(y * kW + x) * 2] = c >> 8;
      fb[(y * kW + x) * 2 + 1] = c & 0xff;
    }
  }
}

uint32_t last_touch_ms = 0, last_button_ms = 0, button_down_ms = 0;
bool touching = false, button_down = false, next_sent = false;
int last_x = 0, last_y = 0;
int32_t knob_sent = 0;

}  // namespace

void setup() {
  esp_log_level_set("*", ESP_LOG_NONE);
  Serial.setRxBufferSize(65536);
  Serial.setTxBufferSize(4096);
  Serial.begin(XS_BAUD);
  if (!power_up()) {
    send_log("io expander did not answer");
  }
  fb = static_cast<uint8_t*>(heap_caps_malloc(kW * kH * 2, MALLOC_CAP_SPIRAM));
  for (auto& b : bounce) b = static_cast<uint8_t*>(heap_caps_malloc(kW * kBandRows * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  bounce_free = xSemaphoreCreateCounting(2, 2);
  plan_blob::allocate = [](size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); };
  plan_blob::release = [](void* q) { heap_caps_free(q); };
  if (!fb || !bounce[0] || !bounce[1]) send_log("out of memory");
  ledcAttach(kLcdBacklight, 5000, 10);
  backlight(0);
  if (!panel_begin()) send_log("panel did not start");
  splash();
  push_rect(0, 0, kW, kH);
  backlight(100);

  Wire1.begin(kTouchSda, kTouchScl, 400000);
  pinMode(kKnobA, INPUT_PULLUP);
  pinMode(kKnobB, INPUT_PULLUP);
  knob_state = (digitalRead(kKnobA) << 1) | digitalRead(kKnobB);
  attachInterrupt(kKnobA, knob_isr, CHANGE);
  attachInterrupt(kKnobB, knob_isr, CHANGE);
  send_info();
}

void loop() {
  static uint8_t buf[4096];
  int avail = Serial.available();
  while (avail > 0) {
    const int n = Serial.read(buf, avail < int(sizeof buf) ? avail : int(sizeof buf));
    if (n <= 0) break;
    rx_bytes(buf, n);
    avail = Serial.available();
  }
  // Inputs only between packets' pixels: a decode is never interrupted by I2C.
  const uint32_t now = millis();
  // A packet that stopped arriving (bytes lost) is dropped, so the next one is seen.
  if (rx.state != RxState::Magic0 && now - rx_last_ms > 500) {
    if (rx.type == XS_PLAN) send_plan_ok(false, 0, 0, "the plan stopped arriving");
    else if (rx.type == XS_AREA) finish_packet(false);
    rx.state = RxState::Magic0;
  }
  if (now - last_touch_ms >= 12) {
    last_touch_ms = now;
    bool down = false;
    int x = 0, y = 0;
    if (touch_poll(down, x, y) && down) {
      if (plan_host) plan_board.pointer(touching ? rt::Input::K::Move : rt::Input::K::Down, x, y);
      else send_input(touching ? XS_IN_MOVE : XS_IN_DOWN, x, y, 0);
      touching = true;
      last_x = x, last_y = y;
    } else if (touching) {
      // The controller reports a lift as a point of no weight, or stops reporting.
      touching = false;
      if (plan_host) plan_board.pointer(rt::Input::K::Up, last_x, last_y);
      else send_input(XS_IN_UP, last_x, last_y, 0);
    }
  }
  const int32_t count = knob_count;
  const int32_t detents = count / 4 - knob_sent;
  if (detents) {
    for (int32_t k = 0; k < (detents > 0 ? detents : -detents); ++k) {
      if (plan_host) plan_board.key(detents > 0 ? "ArrowRight" : "ArrowLeft");
      else send_input(XS_IN_KEY, 0, 0, detents > 0 ? XS_KEY_RIGHT : XS_KEY_LEFT);
    }
    knob_sent += detents;
  }
  if (now - last_button_ms >= 20) {
    last_button_ms = now;
    const int port0 = exp_read(0x00);
    const bool pressed = port0 >= 0 && !(port0 & kKnobButtonBit);
    if (pressed && !button_down) {
      button_down = true;
      button_down_ms = now;
      next_sent = false;
    } else if (pressed && button_down) {
      if (!next_sent && now - button_down_ms >= 2000) {
        send_input(XS_IN_KEY, 0, 0, XS_KEY_NEXT_APP);
        next_sent = true;
      }
    } else if (!pressed && button_down) {
      button_down = false;
      const uint32_t held = now - button_down_ms;
      if (!next_sent) {
        if (plan_host) plan_board.key(held >= 600 ? "Escape" : "Enter");
        else send_input(XS_IN_KEY, 0, 0, held >= 600 ? XS_KEY_ESCAPE : XS_KEY_ENTER);
      }
    }
  }
  if (plan_host) {
    plan_host->turn();
  } else if (!Serial.available()) {
    delayMicroseconds(200);
  }
}
