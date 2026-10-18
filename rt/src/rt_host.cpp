// Host: runs an app on a board. Each turn lets the extensions sense, takes the
// board's inputs, runs the timers, paints, and answers agent lines from the
// board's link, if it has one:
// `CE_AGENT {json}` for each request, and `CE_SHOT begin|CE_SHOT_DATA|end`
// (base64 RGB565 big-endian with a CRC-32) for a screenshot.
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "contract_rt.h"

namespace rt {

namespace {

constexpr size_t kMaxLine = 512;
constexpr size_t kChunk = 3072;

uint32_t crc32(const uint8_t* data, size_t n) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

void base64(const uint8_t* data, size_t n, std::string& out) {
  static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  out.clear();
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t(data[i]) << 16) | (i + 1 < n ? uint32_t(data[i + 1]) << 8 : 0) |
                 (i + 2 < n ? data[i + 2] : 0);
    out += kTable[(v >> 18) & 63];
    out += kTable[(v >> 12) & 63];
    out += i + 1 < n ? kTable[(v >> 6) & 63] : '=';
    out += i + 2 < n ? kTable[v & 63] : '=';
  }
}

// Where the value of `"name":` starts in a flat JSON line (whitespace allowed
// around the colon), or npos. The name must be followed by a colon, so a value
// spelled like a name (`"op": "key"`) is not taken for it.
size_t value_at(const std::string& line, const char* name) {
  std::string key = std::string("\"") + name + "\"";
  for (size_t at = line.find(key); at != std::string::npos; at = line.find(key, at + 1)) {
    size_t i = at + key.size();
    while (i < line.size() && line[i] == ' ') ++i;
    if (i >= line.size() || line[i] != ':') continue;
    ++i;
    while (i < line.size() && line[i] == ' ') ++i;
    return i;
  }
  return std::string::npos;
}

}  // namespace

// Escapes are not decoded.
std::string string_field(const std::string& line, const char* name) {
  size_t i = value_at(line, name);
  if (i == std::string::npos || i >= line.size() || line[i] != '"') return "";
  size_t end = line.find('"', i + 1);
  return end == std::string::npos ? "" : line.substr(i + 1, end - i - 1);
}

double number_field(const std::string& line, const char* name) {
  size_t i = value_at(line, name);
  return i == std::string::npos ? 0 : std::strtod(line.c_str() + i, nullptr);
}

Host::Host(const App& app, Board& board, std::vector<Extension*> extensions)
    : board_(board), runtime_(app, board, std::move(extensions)) {}

bool Host::boot() {
  if (!runtime_.boot()) {
    write("CE_ERROR source=boot detail=display", false);
    return false;
  }
  runtime_.tick();
  runtime_.stats.ready_ms = board_.millis();
  report();
  write("CE_READY planb=1", false);
  return true;
}

void Host::report() {
  for (const std::string& part : runtime_.missing()) {
    write("CE_WARN missing=" + part + (part == "display" ? " detail=headless" : ""), false);
  }
  for (const std::string& name : runtime_.stale()) write("CE_WARN extension=" + name + " detail=digest", false);
}

void Host::write(const std::string& line, bool bulk) {
  if (Link* link = board_.link()) link->write_line(line, bulk);
}

void Host::turn() {
  runtime_.sense();
  Input in;
  while (board_.input(in)) {
    if (in.k == Input::K::Key) runtime_.key(in.key);
    else runtime_.pointer(in.k, in.x, in.y);
  }
  runtime_.tick();
  board_.step(board_.millis());
  Link* link = board_.link();
  for (int c = link ? link->read() : -1; c >= 0; c = link->read()) {
    if (c != '\n' && c != '\r') {
      if (line_.size() < kMaxLine) line_ += static_cast<char>(c);
      continue;
    }
    if (line_.empty()) continue;
    serve(line_);
    line_.clear();
  }
  board_.idle();
}

void Host::serve(const std::string& line) {
  if (string_field(line, "op") == "screenshot") {
    screenshot();
    return;
  }
  const std::string answer = runtime_.agent(line);
  // An answer has a reader waiting for it, so it may wait for the reader as
  // a bulk line does; a long one (a state with records) would otherwise be
  // dropped when the link's buffer is full.
  write("CE_AGENT " + answer, true);
}

void Host::screenshot() {
  Display* d = board_.display();
  uint32_t bytes = 0;
  const uint8_t* data = d ? d->frame(&bytes) : nullptr;
  if (!data) {
    write(d ? "CE_AGENT {\"error\":\"this board keeps no copy of the panel\"}"
            : "CE_AGENT {\"error\":\"this board has no display\"}",
          false);
    return;
  }
  char head[160];
  std::snprintf(head, sizeof head, "CE_SHOT begin width=%d height=%d format=rgb565be bytes=%u crc32=%08x",
                d->width(), d->height(), static_cast<unsigned>(bytes),
                static_cast<unsigned>(crc32(data, bytes)));
  write(head, true);
  std::string chunk;
  for (size_t off = 0; off < bytes; off += kChunk) {
    base64(data + off, std::min<size_t>(kChunk, bytes - off), chunk);
    write("CE_SHOT_DATA " + std::to_string(off) + " " + chunk, true);
  }
  write("CE_SHOT end", true);
}

}  // namespace rt
