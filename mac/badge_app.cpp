// The badge app's glue on the Mac: its `init-badge` extension answers from
// the made-up badge in apps/badge/fixture (BADGE_FIXTURE): profile.bin, a
// record in the badge firmware's format, read as the Stopwatch reads its own,
// and the saved settings and battery in data.json. Its `panel` op reports the
// brightness the app last set.
#include <fstream>
#include <memory>
#include <iterator>
#include <sstream>

#include "app_glue.h"
#include "badge_data.h"

namespace app {

const char* const kName = "badge";

namespace {

std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::unique_ptr<badge::Data> read_fixture() {
  auto out = std::make_unique<badge::Data>();
  const std::string dir = BADGE_FIXTURE;
  const std::string record = slurp(dir + "/profile.bin");
  badge::Profile profile;
  std::string error;
  if (badge::read_profile(reinterpret_cast<const uint8_t*>(record.data()), record.size(), profile, error)) {
    out->set_profile(profile);
  }
  const std::string json = slurp(dir + "/data.json");
  badge::Saved saved;
  saved.brightness = static_cast<int>(rt::number_field(json, "brightness"));
  saved.orientation = rt::string_field(json, "orientation");
  saved.unlocked = json.find("\"unlocked\": true") != std::string::npos;
  out->set_saved(saved);
  out->set_battery(static_cast<int>(rt::number_field(json, "battery")));
  return out;
}

}  // namespace

std::vector<rt::Extension*> extensions() {
  // One Data per runtime: it publishes the profile photo to the first runtime
  // that senses it and remembers that it has (badge_data.cpp, `published_`),
  // so two runtimes in one process must not share one.
  static std::vector<std::unique_ptr<badge::Data>> owned;
  owned.push_back(read_fixture());
  return {owned.back().get()};
}

}  // namespace app
