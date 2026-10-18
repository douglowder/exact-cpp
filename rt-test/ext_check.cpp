// Checks how the runtime binds host extensions, on the level app's generated
// tables (apps/level/cpp/app_gen.cpp), which list `attitude` version 1: an
// extension binds by name and version, a different file (digest) is reported
// stale, an unbound one is reported missing and its source answers none, and
// its ops reach it only when bound. Prints each failure; exit 0 when all pass.
#include <cstdio>
#include <string>
#include <vector>

#include "contract_rt.h"
#include "ext_attitude.h"

namespace app {
extern const rt::App kApp;
}

namespace {

class Clock : public rt::Board {
 public:
  int64_t micros() const override { return 0; }
};

// The attitude extension with a fixed answer, and a version and digest a case
// may change.
class Fake : public ext::AttitudeExtension {
 public:
  uint16_t version_ = AttitudeExtension::version();
  std::string digest_ = AttitudeExtension::digest();
  bool answers_ = true;
  int tilts_ = 0;

  uint16_t version() const override { return version_; }
  const char* digest() const override { return digest_.c_str(); }
  bool attitude(Attitude& out) override {
    out.pitch = 12;
    out.roll = -3;
    return answers_;
  }
  std::string tilt(rt::Runtime&, double pitch, double roll) override {
    ++tilts_;
    return "{\"pitch\":" + rt::number_text(pitch) + ",\"roll\":" + rt::number_text(roll) + "}";
  }
};

int failures = 0;

void expect(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

bool has(const std::vector<std::string>& items, const std::string& item) {
  for (const std::string& i : items) {
    if (i == item) return true;
  }
  return false;
}

// The app's `tilt` resource as the agent prints it.
std::string tilt(rt::Runtime& runtime) {
  const std::string state = runtime.agent("{\"op\":\"state\"}");
  const size_t at = state.find("\"tilt\":");
  if (at == std::string::npos) return "";
  const std::string value = state.substr(at + 7);
  return value.compare(0, 4, "null") == 0 ? "null" : value.substr(0, value.find('}') + 1);
}

void check(const char* name, Fake* fake, bool bound, bool stale, const std::string& want_tilt) {
  Clock board;
  std::vector<rt::Extension*> extensions;
  if (fake) extensions.push_back(fake);
  rt::Runtime runtime(app::kApp, board, extensions);
  expect(runtime.boot(), std::string(name) + ": boot");
  expect(has(runtime.missing(), "extension:attitude") == !bound, std::string(name) + ": missing");
  expect(has(runtime.stale(), "attitude") == stale, std::string(name) + ": stale");
  expect(tilt(runtime) == want_tilt, std::string(name) + ": tilt is " + tilt(runtime) + ", expected " + want_tilt);
  const std::string answer = runtime.agent("{\"op\":\"tilt\",\"pitch\":2.5,\"roll\":-1}");
  const std::string want = bound ? "{\"pitch\":2.5,\"roll\":-1}" : "{\"error\":\"unknown op\"}";
  expect(answer == want, std::string(name) + ": tilt op answered " + answer);
  const std::string listed = runtime.agent("{\"op\":\"extensions\"}");
  expect(listed.find(bound ? "\"bound\":true" : "\"bound\":false") != std::string::npos,
         std::string(name) + ": extensions " + listed);
  expect(listed.find(bound && !stale ? "\"same_file\":true" : "\"same_file\":false") != std::string::npos,
         std::string(name) + ": extensions " + listed);
}

}  // namespace

int main() {
  const std::string level = "{\"pitch\":12,\"roll\":-3}";
  Fake same;
  check("same file", &same, true, false, level);
  Fake other_version;
  other_version.version_ = 2;
  check("other version", &other_version, false, false, "null");
  Fake other_file;
  other_file.digest_ = std::string(64, '0');
  check("other file", &other_file, true, true, level);
  Fake silent;
  silent.answers_ = false;
  check("no answer", &silent, true, false, "null");
  check("no extension", nullptr, false, false, "null");
  std::printf("ext_check: %s\n", failures ? "failures" : "all checks pass");
  return failures ? 1 : 0;
}
