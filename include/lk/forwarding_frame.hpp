#pragma once
#include <api/video/encoded_frame.h>
#include <api/transport/rtp/dependency_descriptor.h>
#include <modules/rtp_rtcp/source/frame_object.h>
#include <modules/rtp_rtcp/source/rtp_packet_received.h>
#include <memory>
#include <vector>

namespace lk {
// Routing metadata is separate from encoded bytes. In particular an encrypted
// frame keeps its original RTP payloads; an SFU must not inspect its bitstream.
struct ForwardingFrame : webrtc::EncodedFrame {
  ForwardingFrame() = default;

  explicit ForwardingFrame(const webrtc::EncodedFrame& f) : webrtc::EncodedFrame(f) {}

  webrtc::RTPVideoHeader header;
  std::shared_ptr<const webrtc::FrameDependencyStructure> structure;
  std::vector<webrtc::RtpPacketReceived> packets;
};

inline const webrtc::RTPVideoHeader* forwarding_header(const webrtc::EncodedFrame& frame) {
  if (auto* f = dynamic_cast<const ForwardingFrame*>(&frame))
    return &f->header;
  if (auto* f = dynamic_cast<const webrtc::RtpFrameObject*>(&frame))
    return &f->GetRtpVideoHeader();
  return nullptr;
}

inline int frame_spatial(const webrtc::EncodedFrame& frame) {
  const auto* h = forwarding_header(frame);
  if (!h)
    return 0;
  if (h->generic)
    return h->generic->spatial_index;
  if (auto* v = std::get_if<webrtc::RTPVideoHeaderVP9>(&h->video_type_header))
    return v->spatial_idx == webrtc::kNoSpatialIdx ? 0 : v->spatial_idx;
  return 0;
}

inline int frame_temporal(const webrtc::EncodedFrame& frame) {
  const auto* h = forwarding_header(frame);
  if (!h)
    return 0;
  if (h->generic)
    return h->generic->temporal_index;
  if (auto* v = std::get_if<webrtc::RTPVideoHeaderVP9>(&h->video_type_header))
    return v->temporal_idx == webrtc::kNoTemporalIdx ? 0 : v->temporal_idx;
  if (auto* v = std::get_if<webrtc::RTPVideoHeaderVP8>(&h->video_type_header))
    return v->temporalIdx == webrtc::kNoTemporalIdx ? 0 : v->temporalIdx;
  return 0;
}
}  // namespace lk
