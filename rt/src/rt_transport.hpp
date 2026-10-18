// The transport (exact2 LLP 1125 D2–D4, `runner/src/transport.rs`): a gated `sequence`'s
// `play(tracks, bpm=)` expanded into the app's sounds on the transport's own clock, up to a
// second ahead, so no action runs per beat. The board's speaker starts each voice at its
// time. The subset is plan2cpp's (`sound.rs`): one gated, unkeyed sequence with no count-in,
// no MIDI clock and no `ended`; a step's choke group, playback rate and pan are journaled
// once and not applied.
#pragma once

#include <deque>
#include <limits>
#include <string>
#include <vector>

#include "contract_rt.h"

namespace rt {

class Transport {
 public:
  explicit Transport(Runtime& runtime) : rt_(runtime) {}
  // After a commit at `t` (ms since boot): read the gate and `play`'s arguments, then arm,
  // edit or stop.
  void stood(double t);
  // The work due by `t`: a refill and the haptics whose time has come.
  void wake(double t);
  // When `wake` next has work, or infinity.
  double due() const;
  // The agent's `sounds`: the state and the voice record, oldest first.
  std::string json() const;

 private:
  struct Segment {
    double at, beat, bpm;
    double time(double b) const { return at + (b - beat) * 60000.0 / bpm; }
    double beat_at(double t) const { return beat + (t - at) * bpm / 60000.0; }
  };
  struct Step {
    double beat;
    int sound;  // an index into App::sounds, or -1
    double gain;
    std::string haptic;
    bool operator==(const Step& o) const {
      return beat == o.beat && sound == o.sound && gain == o.gain && haptic == o.haptic;
    }
  };
  struct Track {
    double length, loops, offset;
    std::vector<Step> steps;
    bool operator==(const Track& o) const {
      return length == o.length && loops == o.loops && offset == o.offset && steps == o.steps;
    }
  };
  struct Hit {
    double at;
    size_t track, step;
  };
  // One voice as the record keeps it, as the runner's `state.sounds` does.
  struct Voice {
    uint32_t id;
    uint16_t sound;
    double at, ends, gain;
    const char* by;  // "end", "stop" (stopped while sounding), "edit" or "cancelled"
  };
  enum class Fill { Arm, Edit, Wake };

  bool read(const V& tracks, const V& bpm, std::vector<Track>& out, double& tempo);
  void arm(double t);
  void edit(double t);
  void stop(double t);
  void retract(double t, const char* by);
  void fill(double t, Fill how);
  std::vector<Hit> hits(double from, double to) const;
  void issue(const Step& step, double when, double t);
  void note(const std::string& line);

  Runtime& rt_;
  bool gate_ = false;
  bool playing_ = false;
  V raw_tracks_, raw_bpm_;
  std::vector<Track> tracks_;
  double tempo_ = 0;
  Segment seg_{0, 0, 120};
  double issued_to_ = 0;
  double refill_due_ = std::numeric_limits<double>::infinity();
  uint32_t next_id_ = 1;
  std::deque<Voice> voices_;
  std::vector<std::pair<double, std::string>> haptics_;
  std::vector<std::string> told_;  // journal lines said once
};

}  // namespace rt
