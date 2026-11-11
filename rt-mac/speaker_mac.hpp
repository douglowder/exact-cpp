// A Mac speaker for the transport's voices (rt_transport.cpp), through CoreAudio's AudioQueue:
// 16-bit mono at 44.1 kHz in three 10 ms buffers. The render callback mixes each voice at its
// board time and gain, so a voice starts on time whatever the main loop is doing, as the
// Stopwatch's voice task does. It runs on the real clock only: a manual clock (the checks)
// does not move with the audio.
#pragma once

#include <AudioToolbox/AudioToolbox.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "exact_rt.h"

namespace rt_mac {

class Speaker final : public rt::Speaker {
 public:
  // `app`'s sounds, timed by `board`'s clock.
  Speaker(const rt::App& app, const rt::Board& board) : app_(app), board_(board) {}
  ~Speaker() override;
  Speaker(const Speaker&) = delete;
  Speaker& operator=(const Speaker&) = delete;
  // Opens and starts the output; false with a message in `error` if CoreAudio refuses.
  bool start(std::string& error);
  void play(uint32_t id, uint16_t sound, int64_t at_us, float gain) override;
  void cancel_from(int64_t at_us) override;

 private:
  struct Voice {
    int64_t at_us;
    uint16_t sound;
    float gain;
  };
  static void callback(void* self, AudioQueueRef queue, AudioQueueBufferRef buffer);
  // Mixes `frames` samples from board time `next_us_` into `out`.
  void render(int16_t* out, size_t frames);

  const rt::App& app_;
  const rt::Board& board_;
  AudioQueueRef queue_ = nullptr;
  std::mutex lock_;
  std::vector<Voice> voices_;
  int64_t next_us_ = -1;  // the board time of the next buffer's first sample
};

}  // namespace rt_mac
