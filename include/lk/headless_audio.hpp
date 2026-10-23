#pragma once
#include <modules/audio_device/include/audio_device_default.h>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>

namespace lk {
// Audio receive streams are pulled by ADM playout. A dummy ADM never pulls,
// so remote AudioTrack sinks would stay silent. Drive playout every 10 ms and
// discard the mix: each remote source delivers its own PCM to its relay sender.
class HeadlessAudioDevice : public webrtc::webrtc_impl::AudioDeviceModuleDefault<webrtc::AudioDeviceModule> {
 public:
  ~HeadlessAudioDevice() override { StopPlayout(); }
  int32_t RegisterAudioCallback(webrtc::AudioTransport* transport) override {
    StopPlayout(); transport_.store(transport); return 0;
  }
  int32_t ActiveAudioLayer(AudioLayer* layer) const override { *layer = kDummyAudio; return 0; }
  int32_t PlayoutIsAvailable(bool* available) override { *available = true; return 0; }
  int32_t RecordingIsAvailable(bool* available) override { *available = false; return 0; }
  int32_t StereoPlayout(bool* enabled) const override { *enabled = false; return 0; }
  int32_t StereoRecording(bool* enabled) const override { *enabled = false; return 0; }
  int32_t SpeakerVolumeIsAvailable(bool* available) override { *available = false; return 0; }
  int32_t MicrophoneVolumeIsAvailable(bool* available) override { *available = false; return 0; }
  int32_t SpeakerMuteIsAvailable(bool* available) override { *available = false; return 0; }
  int32_t MicrophoneMuteIsAvailable(bool* available) override { *available = false; return 0; }
  int32_t StartPlayout() override {
    if (playing_.exchange(true)) return 0;
    thread_ = std::thread([this] {
      auto next = std::chrono::steady_clock::now();
      while (playing_.load()) {
        if (auto* transport = transport_.load()) {
          std::array<int16_t, 480> samples{}; size_t count = 0; int64_t elapsed = 0, ntp = 0;
          transport->NeedMorePlayData(480, sizeof(int16_t), 1, 48000, samples.data(), count, &elapsed, &ntp);
        }
        next += std::chrono::milliseconds(10);
        if (next < std::chrono::steady_clock::now() - std::chrono::milliseconds(20)) next = std::chrono::steady_clock::now();
        std::this_thread::sleep_until(next);
      }
    });
    return 0;
  }
  int32_t StopPlayout() override {
    playing_.store(false);
    if (thread_.joinable()) thread_.join();
    return 0;
  }
  int32_t Terminate() override { StopPlayout(); transport_.store(nullptr); return 0; }
  bool Playing() const override { return playing_.load(); }
 private:
  std::atomic<webrtc::AudioTransport*> transport_{nullptr};
  std::atomic<bool> playing_{false};
  std::thread thread_;
};
}
