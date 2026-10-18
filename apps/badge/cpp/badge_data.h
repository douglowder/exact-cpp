// The badge app's `init-badge` extension (extensions/init-badge.json): its
// saved badge (the badge firmware's profile record), saved settings, and
// battery, answered as the `badgeProfile`, `badgeSettings`, and
// `badgeBattery` sources, the `setBrightness` send, and the `panel` agent op.
// A board reads the stored bytes; this reads the record and answers. The
// photo is an image the app names "app:/data/avatar".
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "contract_rt.h"
#include "ext_init_badge.h"

namespace badge {

// The profile record (conference-manual-v1.bin): a 64-byte header ("INITCF01",
// version 1–3, 160 x 160, metadata and image lengths, SHA-256), the
// metadata, and an optional RGB565 photo (little-endian, 160 x 160).
struct Profile {
  std::string name, company, network, handle;
  std::vector<uint8_t> photo;  // 51,200 bytes, or empty
};

// Reads a whole record; false with `error` set if it is not a valid one.
bool read_profile(const uint8_t* data, size_t size, Profile& out, std::string& error);

// The saved settings: brightness 10–100, orientation "Free", "Default", or
// "180", and the invitation unlock.
struct Saved {
  int brightness = 70;
  std::string orientation = "Default";
  bool unlocked = false;
};

// The settings from the badge firmware's stored values: `prefs`
// (0xC701 << 16 | orientation << 8 | brightness) and `after_dark` (0xA1:
// unlocked); the defaults for a value that is not valid.
Saved saved_from(uint32_t prefs, uint8_t after_dark);

class Data : public ext::InitBadgeExtension {
 public:
  void set_profile(const Profile& p);
  void set_saved(const Saved& s) { saved_ = s; }
  // A new battery reading; the app sees it at the next turn.
  void set_battery(int percent) { battery_ = percent; }
  // Sets the panel's brightness, 10–100 %.
  std::function<void(int)> on_brightness;

  bool badge_profile(BadgeProfile& out) override;
  bool badge_settings(BadgeSettings& out) override;
  bool badge_battery(BadgeBattery& out) override;
  bool set_brightness(double level, SetBrightness& out) override;
  std::string panel(rt::Runtime& runtime) override;
  // Gives the runtime the photo, once there is one, and tells it when the
  // battery changed.
  void sense(rt::Runtime& runtime) override;

 private:
  Profile profile_;
  std::string url_, label_;
  Saved saved_;
  int battery_ = -1;
  int battery_seen_ = -1;
  int brightness_set_ = -1;
  rt::Image photo_{"app:/data/avatar", 160, 160, 0, 0, nullptr, nullptr};
  bool published_ = false;
};

}  // namespace badge
