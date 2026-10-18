#include "badge_data.h"

#include <cstring>

#include "sha256.h"
#include "social_networks.h"

namespace badge {

namespace {

constexpr size_t kHeader = 64, kMaxMetadata = 768, kSide = 160;
constexpr size_t kPhotoBytes = kSide * kSide * 2;
constexpr char kMagic[8] = {'I', 'N', 'I', 'T', 'C', 'F', '0', '1'};

uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t* p) { return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// `count` fields, each a little-endian u16 length and that many bytes with no
// NUL, filling the metadata exactly.
bool fields(const uint8_t* p, size_t n, std::string* const* out, int count) {
  size_t at = 0;
  for (int i = 0; i < count; ++i) {
    if (at + 2 > n) return false;
    const size_t len = u16(p + at);
    at += 2;
    if (len > n - at || std::memchr(p + at, 0, len)) return false;
    out[i]->assign(reinterpret_cast<const char*>(p + at), len);
    at += len;
  }
  return at == n;
}

// A name or company: at most 60 code points of valid UTF-8, with no control,
// line-separator, or bidirectional-override characters.
bool printable(const std::string& s) {
  int count = 0;
  for (size_t i = 0; i < s.size();) {
    const uint8_t c = static_cast<uint8_t>(s[i++]);
    uint32_t cp = c;
    int more = 0;
    if (c >= 0xc2 && c <= 0xdf) cp = c & 31, more = 1;
    else if (c >= 0xe0 && c <= 0xef) cp = c & 15, more = 2;
    else if (c >= 0xf0 && c <= 0xf4) cp = c & 7, more = 3;
    else if (c >= 0x80) return false;
    if (i + more > s.size()) return false;
    for (int k = 0; k < more; ++k) {
      const uint8_t next = static_cast<uint8_t>(s[i++]);
      if ((next & 0xc0) != 0x80) return false;
      cp = cp << 6 | (next & 63);
    }
    const bool overlong = (more == 1 && cp < 0x80) || (more == 2 && cp < 0x800) || (more == 3 && cp < 0x10000);
    const bool banned = cp < 32 || (cp >= 127 && cp <= 159) || (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff ||
                        cp == 0x2028 || cp == 0x2029 || (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069);
    if (overlong || banned || ++count > 60) return false;
  }
  return s.size() <= 120;
}

// Versions 1 and 2 kept GitHub, X, and LinkedIn URLs (and, from 2, the
// company); the first one filled becomes the account.
bool legacy(const uint8_t* p, size_t n, uint16_t version, Profile& out) {
  std::string urls[3];
  std::string* slots[] = {&out.name, &urls[0], &urls[1], &urls[2], &out.company};
  if (!fields(p, n, slots, version == 1 ? 4 : 5)) return false;
  const char* keys[3] = {"github", "x", "linkedin"};
  for (int i = 0; i < 3; ++i) {
    if (urls[i].empty()) continue;
    const std::string handle = badge_social::handle(badge_social::find(keys[i]), urls[i]);
    if (handle.empty()) return false;
    if (out.network.empty()) out.network = keys[i], out.handle = handle;
  }
  return true;
}

}  // namespace

bool read_profile(const uint8_t* data, size_t size, Profile& out, std::string& error) {
  error = "not a badge record";
  if (size < kHeader || std::memcmp(data, kMagic, 8) != 0) return false;
  const uint16_t version = u16(data + 8);
  const size_t meta = u32(data + 16), image = u32(data + 20);
  if (version < 1 || version > 3 || u16(data + 10) != kHeader || u16(data + 12) != kSide || u16(data + 14) != kSide ||
      meta == 0 || meta > kMaxMetadata || (image && image != kPhotoBytes) || u32(data + 24) || u32(data + 28) ||
      size != kHeader + meta + image) {
    return false;
  }
  Sha256 hash;
  hash.update(data, 32);
  hash.update(data + kHeader, meta + image);
  uint8_t sum[32];
  hash.finish(sum);
  if (std::memcmp(sum, data + 32, 32) != 0) {
    error = "the badge record's checksum does not match";
    return false;
  }
  Profile p;
  const uint8_t* m = data + kHeader;
  if (version == 3) {
    std::string* slots[] = {&p.name, &p.network, &p.handle, &p.company};
    if (!fields(m, meta, slots, 4)) return false;
  } else if (!legacy(m, meta, version, p)) {
    return false;
  }
  const int network = p.network.empty() ? -1 : badge_social::find(p.network);
  const bool account = p.network.empty() ? p.handle.empty() : badge_social::valid(network, p.handle);
  if (!printable(p.name) || !printable(p.company) || !account) {
    error = "the badge record holds a value the badge does not accept";
    return false;
  }
  if (image) p.photo.assign(data + kHeader + meta, data + kHeader + meta + image);
  out = std::move(p);
  error.clear();
  return true;
}

Saved saved_from(uint32_t prefs, uint8_t after_dark) {
  Saved s;
  const uint32_t brightness = prefs & 0xff, orientation = (prefs >> 8) & 0xff;
  if ((prefs >> 16) == 0xc701 && brightness >= 10 && brightness <= 100 && brightness % 10 == 0 && orientation <= 2) {
    s.brightness = static_cast<int>(brightness);
    const char* names[3] = {"Free", "Default", "180"};
    s.orientation = names[orientation];
  }
  s.unlocked = after_dark == 0xa1;
  return s;
}

void Data::set_profile(const Profile& p) {
  profile_ = p;
  const int network = p.network.empty() ? -1 : badge_social::find(p.network);
  url_ = network < 0 ? std::string() : badge_social::url(network, p.handle);
  label_ = network < 0 ? std::string() : badge_social::photo_source(network, p.handle).label;
  photo_.rgb = profile_.photo.empty() ? nullptr : profile_.photo.data();
  published_ = false;
}

bool Data::badge_profile(BadgeProfile& out) {
  out.name = profile_.name;
  out.company = profile_.company;
  out.url = url_;
  out.label = label_;
  out.photo = profile_.photo.empty() ? "" : "app:/data/avatar";
  return true;
}

bool Data::badge_settings(BadgeSettings& out) {
  out.brightness = saved_.brightness;
  out.orientation = saved_.orientation;
  out.unlocked = saved_.unlocked;
  return true;
}

bool Data::badge_battery(BadgeBattery& out) {
  out.percent = battery_;
  return true;
}

bool Data::set_brightness(double level, SetBrightness& out) {
  brightness_set_ = static_cast<int>(level);
  if (on_brightness) on_brightness(brightness_set_);
  out.ok = true;
  return true;
}

std::string Data::panel(rt::Runtime&) { return "{\"brightness\":" + std::to_string(brightness_set_) + "}"; }

void Data::sense(rt::Runtime& runtime) {
  if (battery_ != battery_seen_) {
    battery_seen_ = battery_;
    badge_battery_changed(runtime);
  }
  if (published_ || !photo_.rgb) return;
  published_ = true;
  runtime.put_image(photo_.source, &photo_);
}

}  // namespace badge
