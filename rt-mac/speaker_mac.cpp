// The Mac speaker: an AudioQueue whose callback mixes the voices due in each buffer. A voice is
// placed by its board time: the buffer's first sample is `next_us_`, the board time the
// output reaches it, about three buffers after the callback.
#include "speaker_mac.hpp"

#include <algorithm>
#include <cmath>

namespace rt_mac {

namespace {

constexpr double kRate = 44100.0;
constexpr UInt32 kFrames = 441;  // 10 ms a buffer
constexpr int kBuffers = 3;
constexpr int64_t kLatencyUs = kBuffers * 10000;
constexpr int64_t kDriftUs = 50000;  // past this the next buffer is placed again

}  // namespace

Speaker::~Speaker() {
  if (queue_) {
    AudioQueueStop(queue_, true);
    AudioQueueDispose(queue_, true);
  }
}

bool Speaker::start(std::string& error) {
  AudioStreamBasicDescription f{};
  f.mSampleRate = kRate;
  f.mFormatID = kAudioFormatLinearPCM;
  f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
  f.mChannelsPerFrame = 1;
  f.mBitsPerChannel = 16;
  f.mBytesPerFrame = 2;
  f.mFramesPerPacket = 1;
  f.mBytesPerPacket = 2;
  OSStatus s = AudioQueueNewOutput(&f, callback, this, nullptr, nullptr, 0, &queue_);
  if (s != noErr) {
    error = "AudioQueueNewOutput failed (" + std::to_string(s) + ")";
    return false;
  }
  for (int i = 0; i < kBuffers; ++i) {
    AudioQueueBufferRef b = nullptr;
    s = AudioQueueAllocateBuffer(queue_, kFrames * 2, &b);
    if (s != noErr) {
      error = "AudioQueueAllocateBuffer failed (" + std::to_string(s) + ")";
      return false;
    }
    callback(this, queue_, b);
  }
  s = AudioQueueStart(queue_, nullptr);
  if (s != noErr) {
    error = "AudioQueueStart failed (" + std::to_string(s) + ")";
    return false;
  }
  return true;
}

void Speaker::play(uint32_t /*id*/, uint16_t sound, int64_t at_us, float gain) {
  std::lock_guard<std::mutex> hold(lock_);
  voices_.push_back({at_us, sound, gain});
}

void Speaker::cancel_from(int64_t at_us) {
  std::lock_guard<std::mutex> hold(lock_);
  voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [at_us](const Voice& v) { return v.at_us >= at_us; }),
                voices_.end());
}

void Speaker::callback(void* self, AudioQueueRef queue, AudioQueueBufferRef buffer) {
  auto* sp = static_cast<Speaker*>(self);
  sp->render(static_cast<int16_t*>(buffer->mAudioData), kFrames);
  buffer->mAudioDataByteSize = kFrames * 2;
  AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
}

void Speaker::render(int16_t* out, size_t frames) {
  const int64_t due = board_.micros() + kLatencyUs;
  if (next_us_ < 0 || std::llabs(next_us_ - due) > kDriftUs) next_us_ = due;
  std::vector<float> mix(frames, 0.0f);
  const double end_us = next_us_ + frames * 1e6 / kRate;
  {
    std::lock_guard<std::mutex> hold(lock_);
    for (const Voice& v : voices_) {
      const rt::Sound& s = app_.sounds[v.sound];
      for (size_t i = 0; i < frames; ++i) {
        const double t_us = next_us_ + i * 1e6 / kRate;
        if (t_us < v.at_us) continue;
        const auto k = static_cast<size_t>((t_us - v.at_us) * s.rate / 1e6);
        if (k >= s.frames) break;
        mix[i] += s.samples[k] * v.gain;
      }
    }
    // A voice whose last sample is before the next buffer is done.
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [&](const Voice& v) {
                                   const rt::Sound& s = app_.sounds[v.sound];
                                   return v.at_us + s.frames * 1e6 / s.rate < end_us;
                                 }),
                  voices_.end());
  }
  for (size_t i = 0; i < frames; ++i) {
    out[i] = static_cast<int16_t>(std::clamp(std::lround(mix[i]), -32768L, 32767L));
  }
  next_us_ += static_cast<int64_t>(frames * 1e6 / kRate);
}

}  // namespace rt_mac
