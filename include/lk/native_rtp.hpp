#pragma once
#include <api/environment/environment.h>
#include <api/rtp_parameters.h>
#include <modules/rtp_rtcp/source/rtp_packet_received.h>
#include <modules/rtp_rtcp/source/rtp_packet_to_send.h>
#include <modules/rtp_rtcp/source/rtcp_packet/sender_report.h>
#include <functional>
#include <memory>
#include <rtc_base/network/sent_packet.h>
#include <rtc_base/network_route.h>
#include <api/video/encoded_frame.h>

namespace lk {
// Transport-level glue around native GoogCC, feedback history and pacing.
// All methods, callbacks and destruction run on one WebRTC network task queue.
class NativeSendTransport {
 public:
  struct Stats {
    uint64_t sent=0,padding=0,feedback=0,feedback_packets=0,dropped=0,queue_dropped=0;
    uint64_t probe_clusters=0,probe_packets=0,probe_feedback_packets=0;
    int64_t target_bps=0,pacing_bps=0,queue_bytes=0,queue_ms=0;
  };
  struct Stream {
    std::function<void(webrtc::RtpPacketToSend&)> prepare;
    std::function<bool(const webrtc::RtpPacketToSend&)> send;
    std::function<void(std::unique_ptr<webrtc::RtpPacketToSend>,bool)> complete;
    std::function<std::vector<std::unique_ptr<webrtc::RtpPacketToSend>>(size_t)> padding;
    std::function<void(webrtc::ArrayView<const uint16_t>)> abort;
  };
  // Optional shared native timer queue; media state stays on the owning queue.
  explicit NativeSendTransport(const webrtc::Environment&,size_t max_queue_bytes=4*1024*1024,
                               webrtc::TaskQueueBase* timer_queue=nullptr);
  ~NativeSendTransport();
  void attach(uint32_t,std::optional<uint32_t>,Stream);
  void detach(uint32_t,std::optional<uint32_t>);
  void discard(uint32_t,std::optional<uint32_t>);
  // Admit the entire batch or reject it, so a video frame is never split by
  // the application memory limit. Queue ordering and expiry remain native.
  bool enqueue(std::vector<std::unique_ptr<webrtc::RtpPacketToSend>>);
  void set_available(bool);
  void sent_packet(const webrtc::SentPacketInfo&);
  void network_route(const webrtc::NetworkRoute&);
  void feedback(webrtc::ArrayView<const uint8_t>);
  // Native StreamsConfig padding limit; never a bandwidth setter.
  void set_padding_demand(int64_t padding_bps);
  webrtc::TimeDelta process();
  Stats stats() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// One per publisher transport: transport-wide feedback spans audio/video/RTX.
class NativeReceiveTransport {
 public:
  using Rtcp = std::function<bool(webrtc::ArrayView<const uint8_t>)>;
  NativeReceiveTransport(const webrtc::Environment&,Rtcp);
  ~NativeReceiveTransport();
  void receive(const webrtc::RtpPacketReceived&,bool audio);
  webrtc::TimeDelta process();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// Thin routing adapters, not protocol implementations. Lifetime and every call
// belong to the same native WebRTC network task queue.
struct NativeSendConfig {
  uint32_t ssrc=0;
  std::optional<uint32_t> rtx_ssrc;
  int payload_type=96,rtx_payload_type=-1;
  bool audio=false,extmap_allow_mixed=false;
  std::string mid;
  std::vector<webrtc::RtpExtension> extensions;
  bool operator==(const NativeSendConfig&) const = default;
};
class NativeRtpSender {
 public:
  using Send = std::function<bool(const webrtc::RtpPacketToSend&)>;
  NativeRtpSender(const webrtc::Environment&,NativeSendConfig,Send,std::function<void()> keyframe,
                 std::shared_ptr<NativeSendTransport> transport = {});
  ~NativeRtpSender();
  void forward(const webrtc::RtpPacketReceived&);
  // Repacketize an already encoded VP8 frame with the native packetizer.
  // Each subscriber owns picture IDs, RTP sequence and source clock mapping.
  bool forward_frame(const webrtc::EncodedFrame&,uint32_t source_ssrc);
  void feedback(webrtc::ArrayView<const uint8_t>);
  void set_enabled(bool);
  // Budget pause clears media/RTX while keeping native padding probes alive.
  void set_media_paused(bool);
  webrtc::rtcp::SenderReport translate_report(const webrtc::rtcp::SenderReport&) const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// Native VP8 depacketization, complete-frame assembly and dependency discovery.
// Does not create a decoder. Include RTP padding so native gap tracking works.
class NativeVideoAssembler {
 public:
  using Frame = std::function<void(const webrtc::EncodedFrame&)>;
  explicit NativeVideoAssembler(Frame);
  ~NativeVideoAssembler();
  void receive(const webrtc::RtpPacketReceived&);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
struct NativeReceiveConfig {
  uint32_t ssrc=0;
  int payload_type=96,rtx_payload_type=-1;
  bool audio=false;
  std::vector<webrtc::RtpExtension> extensions;
  bool operator==(const NativeReceiveConfig&) const = default;
};
class NativeRtpReceiver {
 public:
  using Media = std::function<void(const webrtc::RtpPacketReceived&)>;
  using Rtcp = std::function<bool(webrtc::ArrayView<const uint8_t>)>;
  NativeRtpReceiver(const webrtc::Environment&,NativeReceiveConfig,Media,Rtcp);
  ~NativeRtpReceiver();
  void receive(const webrtc::RtpPacketReceived&);
  void feedback(webrtc::ArrayView<const uint8_t>);
  void request_keyframe();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
