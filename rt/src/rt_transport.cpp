// The transport: exact2's `runner/src/transport.rs` for the C++ target's subset. A fill
// issues every hit in [issued_to, t + 1 s), and the next is due 500 ms before its end. An
// edit (arguments that differ by value) starts a new segment at the beat reached when the
// tempo changed, ends every voice not started by its time, and fills again from it. Only the
// last segment is kept: a fill never starts before the last edit, which set `issued_to` to
// its time. A wake does not skip a late step (the runner does, on a live clock): on the Mac's
// manual clock no wake is late, and on the board a late step starts at once.
#include "rt_transport.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "rt_view.h"

namespace rt {

namespace {

constexpr double kLookahead = 1000.0;
constexpr double kRefill = 500.0;
constexpr double kLoopsPerSecond = 1000.0;
constexpr double kOffsetLimit = 500.0;
constexpr double kShortest = 1.0 / 1048576.0;
constexpr size_t kRecord = 256;

const char* const kHaptics[] = {"selection",     "impact",      "impact-light", "impact-medium", "impact-heavy",
                                "impact-soft",   "impact-rigid", "success",     "warning",       "error"};

const V* item(const V& list, size_t i) {
  if (list.k != V::K::List || !list.rec || i >= list.rec->size()) return nullptr;
  return &(*list.rec)[i];
}

size_t count(const V& list) { return list.k == V::K::List && list.rec ? list.rec->size() : 0; }

double number(const V& v) { return v.k == V::K::Num ? v.n : NAN; }

bool whole(double n, double low, double high) { return std::floor(n) == n && n >= low && n <= high; }

bool haptic_kind(const std::string& kind) {
  for (const char* k : kHaptics) {
    if (kind == k) return true;
  }
  return false;
}

}  // namespace

void Transport::note(const std::string& line) {
  if (std::find(told_.begin(), told_.end(), line) != told_.end()) return;
  told_.push_back(line);
  rt_.log(line);
}

bool Transport::read(const V& tracks, const V& bpm, std::vector<Track>& out, double& tempo) {
  const App& app = rt_.app();
  const std::string name = app.sequences[0].name;
  tempo = number(bpm);
  if (!(std::isfinite(tempo) && tempo > 0)) {
    rt_.log("sequence " + name + ": bpm " + number_text(tempo) + " is not a tempo");
    return false;
  }
  out.clear();
  for (size_t i = 0; i < count(tracks); ++i) {
    const V& t = *item(tracks, i);
    const double length = number(t.field(0)), loops = number(t.field(1));
    const double offset = t.field(2).k == V::K::None ? 0 : number(t.field(2));
    const std::string label = "sequence " + name + ": track " + std::to_string(i + 1);
    if (!(std::isfinite(length) && length >= kShortest) || !whole(loops, 0, INFINITY) ||
        tempo / 60.0 / length > kLoopsPerSecond || !(std::isfinite(offset) && std::fabs(offset) <= kOffsetLimit)) {
      rt_.log(label + " dropped");
      continue;
    }
    const bool sends = !t.field(3).s.empty();
    Track track{length, loops, offset, {}};
    const V steps = t.field(4);
    for (size_t j = 0; j < count(steps); ++j) {
      const V& s = *item(steps, j);
      const double beat = number(s.field(0)), gain = number(s.field(2)), midi = s.field(4).k == V::K::None ? -1 : number(s.field(4));
      const std::string src = s.field(1).s, haptic = s.field(8).s;
      int sound = -1;
      for (uint16_t k = 0; k < app.sound_count; ++k) {
        if (src == app.sounds[k].src) sound = k;
      }
      const bool midi_ok = midi == -1 || (whole(midi, 0, 127) && whole(number(s.field(5)), 1, 127) &&
                                          whole(number(s.field(6)), 0, 15) && number(s.field(7)) > 0);
      if (!(std::isfinite(beat) && beat >= 0 && beat < length) || (!src.empty() && sound < 0) ||
          (!haptic.empty() && !haptic_kind(haptic)) || (!src.empty() && !std::isfinite(gain)) || !midi_ok) {
        rt_.log(label + " step " + std::to_string(j + 1) + " dropped");
        continue;
      }
      if (src.empty() && haptic.empty() && (midi == -1 || !sends)) continue;
      if (!s.field(3).s.empty()) note("sequence " + name + ": a step's choke group is not applied on this board");
      if (sends) note("sequence " + name + ": MIDI notes are not sent on this board");
      const V rate = s.field(9), pan = s.field(10);
      if ((rate.k == V::K::Num && rate.n != 1) || (pan.k == V::K::Num && pan.n != 0)) {
        note("sequence " + name + ": a step's playbackRate and pan are not applied on this board");
      }
      track.steps.push_back({beat, sound, gain, haptic});
    }
    out.push_back(track);
  }
  return true;
}

std::vector<Transport::Hit> Transport::hits(double from, double to) const {
  std::vector<Hit> out;
  const double b0 = seg_.beat_at(from), b1 = seg_.beat_at(to);
  for (size_t i = 0; i < tracks_.size(); ++i) {
    const Track& t = tracks_[i];
    // A loop either side, so a time the division rounds is still seen.
    double k = std::max(0.0, std::floor(b0 / t.length) - 1.0);
    double last = std::ceil(b1 / t.length) + 1.0;
    if (t.loops > 0) last = std::min(last, t.loops - 1.0);
    for (; k <= last; k += 1.0) {
      for (size_t j = 0; j < t.steps.size(); ++j) {
        const double at = seg_.time(k * t.length + t.steps[j].beat);
        if (from <= at && at < to) out.push_back({at, i, j});
      }
    }
  }
  std::sort(out.begin(), out.end(), [](const Hit& a, const Hit& b) {
    if (a.at != b.at) return a.at < b.at;
    if (a.track != b.track) return a.track < b.track;
    return a.step < b.step;
  });
  return out;
}

void Transport::issue(const Step& step, double when, double t) {
  if (step.sound >= 0) {
    const Sound& sound = rt_.app().sounds[step.sound];
    const double at = std::max(when, t);
    const double gain = std::min(1.0, std::max(0.0, step.gain));
    const double ends = at + 1000.0 * sound.frames / sound.rate;
    const uint32_t id = next_id_++;
    voices_.push_back({id, static_cast<uint16_t>(step.sound), at, ends, gain, "end"});
    if (voices_.size() > kRecord) voices_.pop_front();
    if (Speaker* sp = rt_.board().speaker()) {
      sp->play(id, static_cast<uint16_t>(step.sound), rt_.board_us(at), static_cast<float>(gain));
    }
  }
  if (!step.haptic.empty()) haptics_.push_back({std::max(when, t), step.haptic});
}

void Transport::fill(double t, Fill how) {
  const double from = issued_to_, to = t + kLookahead;
  for (const Hit& h : hits(from, to)) {
    const Track& track = tracks_[h.track];
    const double when = h.at + track.offset;
    // Before `t`: played now at the arming, skipped at an edit (it was played already).
    if (when < t && how == Fill::Edit) continue;
    issue(track.steps[h.step], when, t);
  }
  issued_to_ = to;
  refill_due_ = to - kRefill;
}

void Transport::retract(double t, const char* by) {
  for (Voice& v : voices_) {
    if (std::strcmp(v.by, "end") != 0) continue;
    if (v.at >= t) {
      v.by = by;
      v.ends = v.at;
    } else if (std::strcmp(by, "cancelled") == 0 && v.ends > t) {
      v.by = "stop";
      v.ends = t;
    }
  }
  haptics_.erase(std::remove_if(haptics_.begin(), haptics_.end(), [t](const auto& h) { return h.first >= t; }),
                 haptics_.end());
  if (Speaker* sp = rt_.board().speaker()) sp->cancel_from(rt_.board_us(t));
}

void Transport::arm(double t) {
  if (!read(raw_tracks_, raw_bpm_, tracks_, tempo_)) return;
  playing_ = true;
  seg_ = {t, 0, tempo_};
  issued_to_ = t;
  rt_.log(std::string("sequence ") + rt_.app().sequences[0].name + " starts at beat 0, bpm " + number_text(tempo_));
  fill(t, Fill::Arm);
}

void Transport::edit(double t) {
  std::vector<Track> tracks;
  double tempo = 0;
  if (!read(raw_tracks_, raw_bpm_, tracks, tempo)) return stop(t);
  if (tracks == tracks_ && tempo == tempo_) return;
  if (tempo != seg_.bpm) seg_ = {t, seg_.beat_at(t), tempo};
  retract(t, "edit");
  tracks_ = std::move(tracks);
  tempo_ = tempo;
  issued_to_ = std::min(issued_to_, t);
  fill(t, Fill::Edit);
}

void Transport::stop(double t) {
  retract(t, "cancelled");
  playing_ = false;
  refill_due_ = std::numeric_limits<double>::infinity();
  rt_.log(std::string("sequence ") + rt_.app().sequences[0].name + " stopped (gate)");
}

void Transport::stood(double t) {
  const App& app = rt_.app();
  if (app.sequence_count == 0) return;
  const Sequence& q = app.sequences[0];
  Ctx c{&rt_, nullptr, false, &rt_.view_->root_scope()};
  const bool on = q.gate(c).b;
  const V tracks = q.tracks(c), bpm = q.bpm(c);
  const bool was = gate_;
  gate_ = on;
  if (playing_) {
    if (!on) {
      stop(t);
    } else if (!equal(tracks, raw_tracks_) || !equal(bpm, raw_bpm_)) {
      raw_tracks_ = tracks;
      raw_bpm_ = bpm;
      edit(t);
    }
  } else if (on && !was) {
    raw_tracks_ = tracks;
    raw_bpm_ = bpm;
    arm(t);
  }
}

void Transport::wake(double t) {
  for (size_t i = 0; i < haptics_.size();) {
    if (haptics_[i].first <= t) {
      rt_.board().command("haptic", {V::str(haptics_[i].second)});
      haptics_.erase(haptics_.begin() + static_cast<long>(i));
    } else {
      ++i;
    }
  }
  if (playing_ && t >= refill_due_) fill(t, Fill::Wake);
}

double Transport::due() const {
  double next = playing_ ? refill_due_ : std::numeric_limits<double>::infinity();
  for (const auto& h : haptics_) next = std::min(next, h.first);
  return next;
}

std::string Transport::json() const {
  const App& app = rt_.app();
  std::string out = std::string("{\"state\":\"") + (playing_ ? "playing" : "stopped") + "\",\"voices\":[";
  for (size_t i = 0; i < voices_.size(); ++i) {
    const Voice& v = voices_[i];
    if (i) out += ',';
    out += "{\"id\":" + std::to_string(v.id) + ",\"sound\":" + std::to_string(v.sound) + ",\"src\":";
    quote(app.sounds[v.sound].src, out);
    out += ",\"at\":" + number_text(v.at) + ",\"ends\":" + number_text(v.ends) + ",\"gain\":" + number_text(v.gain) +
           ",\"by\":\"" + v.by + "\"}";
  }
  return out + "]}";
}

}  // namespace rt
