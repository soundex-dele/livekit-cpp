#pragma once
#include "lk/forwarding_frame.hpp"
#include <functional>
#include <memory>

namespace lk {
// Bounded RTP-frame collection for E2EE and packet-preserving codecs. Only
// unencrypted RTP payload descriptors/header extensions are interpreted.
class NativeOpaqueAssembler {
 public:
  NativeOpaqueAssembler(webrtc::VideoCodecType, std::function<void(const webrtc::EncodedFrame&)>);
  ~NativeOpaqueAssembler();
  void receive(const webrtc::RtpPacketReceived&);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace lk
