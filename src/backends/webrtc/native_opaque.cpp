#include "lk/native_opaque.hpp"
#include <modules/rtp_rtcp/source/rtp_dependency_descriptor_extension.h>
#include <modules/rtp_rtcp/source/rtp_generic_frame_descriptor_extension.h>
#include <modules/rtp_rtcp/source/video_rtp_depacketizer_vp8.h>
#include <modules/rtp_rtcp/source/video_rtp_depacketizer_vp9.h>
#include <modules/rtp_rtcp/source/video_rtp_depacketizer_av1.h>
#include <modules/rtp_rtcp/source/rtp_header_extensions.h>
#include <modules/video_coding/rtp_frame_reference_finder.h>
#include <rtc_base/numerics/sequence_number_unwrapper.h>
#include <map>
#include <algorithm>
#include <cstring>

namespace lk {
struct NativeOpaqueAssembler::Impl {
  webrtc::VideoCodecType codec;
  std::function<void(const webrtc::EncodedFrame&)> callback;
  webrtc::SeqNumUnwrapper<uint16_t> sequence, frame_numbers;
  webrtc::RtpFrameReferenceFinder references;
  std::shared_ptr<webrtc::FrameDependencyStructure> structure;

  struct Group {
    std::map<int64_t, webrtc::RtpPacketReceived> packets;
    std::optional<int64_t> first, last;
    webrtc::RTPVideoHeader header;
    std::shared_ptr<const webrtc::FrameDependencyStructure> structure;
    size_t bytes = 0;
  };

  std::map<std::pair<uint32_t, int>, Group> groups;
  // Legacy generic descriptors carry the spatial id only on the first packet.
  // Keep continuation packets independently until that packet establishes the
  // range, including when the marker arrives before the first packet.
  std::map<int64_t, webrtc::RtpPacketReceived> legacy_packets;
  std::map<int64_t, webrtc::RtpPacketReceived> pending_structure;
  size_t pending_structure_bytes = 0;
  std::map<uint16_t, ForwardingFrame> waiting;
  std::optional<int64_t> previous_end;
  int64_t newest_sequence = -1, structure_frame = -1;

  explicit Impl(webrtc::VideoCodecType c, std::function<void(const webrtc::EncodedFrame&)> cb)
      : codec(c), callback(std::move(cb)) {}

  void ready(webrtc::RtpFrameReferenceFinder::ReturnVector frames) {
    for (auto& frame : frames) {
      auto found = waiting.find(frame->first_seq_num());
      if (found == waiting.end())
        continue;
      static_cast<webrtc::EncodedFrame&>(found->second) = *frame;
      callback(found->second);
      waiting.erase(found);
    }
  }

  void receive(const webrtc::RtpPacketReceived& packet) {
    auto seq = sequence.Unwrap(packet.SequenceNumber());
    newest_sequence = std::max(newest_sequence, seq);
    if (seq < newest_sequence - 2048)
      return;
    references.ClearTo(uint16_t(newest_sequence - 2048));
    if (!packet.payload_size()) {
      ready(references.PaddingReceived(packet.SequenceNumber()));
      if (previous_end && seq == *previous_end + 1)
        previous_end = seq;
      return;
    }
    webrtc::RTPVideoHeader h;
    h.codec = codec;
    bool first = false, last = packet.Marker(), legacy = false, new_structure = false;
    if (codec == webrtc::kVideoCodecVP8 || codec == webrtc::kVideoCodecVP9) {
      std::unique_ptr<webrtc::VideoRtpDepacketizer> depacketizer;
      if (codec == webrtc::kVideoCodecVP8)
        depacketizer = std::make_unique<webrtc::VideoRtpDepacketizerVp8>();
      else
        depacketizer = std::make_unique<webrtc::VideoRtpDepacketizerVp9>();
      auto parsed = depacketizer->Parse(packet.PayloadBuffer());
      if (!parsed)
        return;
      h = parsed->video_header;
    }
    if (packet.HasExtension<webrtc::RtpDependencyDescriptorExtension>()) {
      webrtc::DependencyDescriptor d;
      if (!packet.GetExtension<webrtc::RtpDependencyDescriptorExtension>(structure.get(), &d)) {
        if (pending_structure.emplace(seq, packet).second)
          pending_structure_bytes += packet.size();
        while (!pending_structure.empty() &&
               (pending_structure.size() > 2048 || pending_structure_bytes > 2 * 1024 * 1024 ||
                pending_structure.begin()->first < newest_sequence - 2048)) {
          pending_structure_bytes -= pending_structure.begin()->second.size();
          pending_structure.erase(pending_structure.begin());
        }
        return;
      }
      auto id = frame_numbers.Unwrap(uint16_t(d.frame_number));
      if (d.attached_structure) {
        // Never let delayed packets roll back a newer template generation.
        if (id < structure_frame)
          return;
        structure = std::make_shared<webrtc::FrameDependencyStructure>(*d.attached_structure);
        structure_frame = id;
        new_structure = true;
      }
      first = d.first_packet_in_frame;
      last = d.last_packet_in_frame;
      h.generic.emplace();
      h.generic->frame_id = id;
      h.generic->spatial_index = d.frame_dependencies.spatial_id;
      h.generic->temporal_index = d.frame_dependencies.temporal_id;
      h.generic->decode_target_indications = d.frame_dependencies.decode_target_indications;
      h.generic->chain_diffs = d.frame_dependencies.chain_diffs;
      if (d.active_decode_targets_bitmask)
        h.generic->active_decode_targets = *d.active_decode_targets_bitmask;
      for (int diff : d.frame_dependencies.frame_diffs)
        h.generic->dependencies.push_back(id - diff);
      h.frame_type = h.generic->dependencies.empty() ? webrtc::VideoFrameType::kVideoFrameKey
                                                     : webrtc::VideoFrameType::kVideoFrameDelta;
      if (d.resolution) {
        h.width = d.resolution->Width();
        h.height = d.resolution->Height();
      }
    } else if (packet.HasExtension<webrtc::RtpGenericFrameDescriptorExtension00>()) {
      webrtc::RtpGenericFrameDescriptor d;
      if (!packet.GetExtension<webrtc::RtpGenericFrameDescriptorExtension00>(&d))
        return;
      first = d.FirstPacketInSubFrame();
      last = d.LastPacketInSubFrame();
      legacy = true;
      if (first) {
        h.generic.emplace();
        h.generic->frame_id = frame_numbers.Unwrap(d.FrameId());
        h.generic->spatial_index = d.SpatialLayer();
        h.generic->temporal_index = d.TemporalLayer();
        for (auto diff : d.FrameDependenciesDiffs())
          h.generic->dependencies.push_back(h.generic->frame_id - diff);
        h.width = d.Width();
        h.height = d.Height();
        h.frame_type = h.generic->dependencies.empty() ? webrtc::VideoFrameType::kVideoFrameKey
                                                       : webrtc::VideoFrameType::kVideoFrameDelta;
      }
    } else if (codec == webrtc::kVideoCodecVP8 || codec == webrtc::kVideoCodecVP9 ||
               codec == webrtc::kVideoCodecAV1) {
      std::unique_ptr<webrtc::VideoRtpDepacketizer> depacketizer;
      if (codec == webrtc::kVideoCodecVP8)
        depacketizer = std::make_unique<webrtc::VideoRtpDepacketizerVp8>();
      else if (codec == webrtc::kVideoCodecVP9)
        depacketizer = std::make_unique<webrtc::VideoRtpDepacketizerVp9>();
      else
        depacketizer = std::make_unique<webrtc::VideoRtpDepacketizerAv1>();
      auto parsed = depacketizer->Parse(packet.PayloadBuffer());
      if (!parsed)
        return;
      h = parsed->video_header;
      first = h.is_first_packet_in_frame;
      last |= h.is_last_packet_in_frame;
      if (codec == webrtc::kVideoCodecAV1) {
        // An OBU boundary is not a frame boundary without DD. Use the RTP
        // picture boundary and start a new stream only at a coded sequence.
        first = (previous_end && seq == *previous_end + 1) || (packet.payload()[0] & 8);
        last = packet.Marker();
      }
    } else {
      auto p = packet.payload();
      bool key = false;
      if (codec == webrtc::kVideoCodecH264) {
        int type = p[0] & 31;
        if (!type || type > 28 || (type >= 25 && type <= 27) || (type == 28 && p.size() < 3))
          return;
        first = type != 28 || (p.size() > 1 && (p[1] & 0x80));
        key = type == 5 || type == 7 || (type == 28 && p.size() > 1 && (p[1] & 31) == 5);
        if (type == 24)
          for (size_t i = 1; i + 2 < p.size();) {
            size_t n = (size_t(p[i]) << 8) | p[i + 1];
            i += 2;
            if (!n || i + n > p.size())
              return;
            key |= (p[i] & 31) == 5 || (p[i] & 31) == 7;
            i += n;
          }
      } else if (codec == webrtc::kVideoCodecH265) {
        if (p.size() < 2)
          return;
        int type = (p[0] >> 1) & 63;
        if (type > 49 || (p[1] & 7) == 0)
          return;
        first = type != 49 || (p.size() > 2 && (p[2] & 0x80));
        if (type == 49) {
          if (p.size() < 3)
            return;
          type = p[2] & 63;
        }
        key = (type >= 16 && type <= 21) || type == 32 || type == 33;
        if (type == 48)
          for (size_t i = 2; i + 2 < p.size();) {
            size_t n = (size_t(p[i]) << 8) | p[i + 1];
            i += 2;
            if (n < 2 || i + n > p.size())
              return;
            int nal = (p[i] >> 1) & 63;
            key |= (nal >= 16 && nal <= 21) || nal == 32 || nal == 33;
            i += n;
          }
      } else
        return;
      h.frame_type =
          key ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
      // An independently decodable picture also restarts after a sequence gap.
      // Requiring adjacency here would make loss permanent even after a PLI.
      first = (previous_end && seq == *previous_end + 1) || (key && first);
    }
    int spatial = h.generic ? h.generic->spatial_index : 0;
    if (auto* vp9 = std::get_if<webrtc::RTPVideoHeaderVP9>(&h.video_type_header);
        vp9 && vp9->spatial_idx != webrtc::kNoSpatialIdx)
      spatial = vp9->spatial_idx;
    if (legacy) {
      legacy_packets.emplace(seq, packet);
      while (!legacy_packets.empty() && (legacy_packets.size() > 2048 ||
                                         legacy_packets.begin()->first < newest_sequence - 2048))
        legacy_packets.erase(legacy_packets.begin());
      if (!first) {
        std::optional<int64_t> nearest;
        for (const auto& [key, g] : groups)
          if (key.first == packet.Timestamp() && g.first && *g.first <= seq &&
              (!g.last || seq <= *g.last) && (!nearest || *g.first > *nearest)) {
            nearest = g.first;
            spatial = key.second;
          }
        if (!nearest)
          return;
      }
    }
    auto key = std::pair{packet.Timestamp(), spatial};
    auto& group = groups[key];
    if (group.packets.contains(seq))
      return;
    if (group.packets.empty())
      group.header = h;
    if (first && (!group.first || seq < *group.first)) {
      group.first = seq;
      group.header = h;
      group.structure = structure;
    }
    if (h.frame_type == webrtc::VideoFrameType::kVideoFrameKey)
      group.header.frame_type = h.frame_type;
    if (last)
      group.last = seq;
    group.bytes += packet.payload_size();
    group.packets.emplace(seq, packet);
    if (legacy && group.first) {
      // Find the first explicit end after this subframe's start, never consume
      // packets belonging to the next spatial subframe.
      for (auto it = legacy_packets.lower_bound(*group.first); it != legacy_packets.end(); ++it) {
        if (it->second.Timestamp() != packet.Timestamp())
          break;
        webrtc::RtpGenericFrameDescriptor d;
        if (!it->second.GetExtension<webrtc::RtpGenericFrameDescriptorExtension00>(&d))
          break;
        if (it->first != *group.first && d.FirstPacketInSubFrame())
          break;
        if (!group.packets.contains(it->first)) {
          group.bytes += it->second.payload_size();
          group.packets.emplace(*it);
        }
        if (d.LastPacketInSubFrame()) {
          group.last = it->first;
          break;
        }
      }
    }
    if (group.bytes > 2 * 1024 * 1024 || group.packets.size() > 2048) {
      groups.erase(key);
      return;
    }
    if (group.first && group.last && *group.last >= *group.first &&
        *group.last - *group.first < 2048) {
      bool complete = true;
      for (auto s = *group.first; s <= *group.last; ++s)
        if (!group.packets.contains(s)) {
          complete = false;
          break;
        }
      if (complete) {
        ForwardingFrame saved;
        saved.header = group.header;
        saved.structure = group.structure;
        size_t bytes = 0;
        for (auto s = *group.first; s <= *group.last; ++s)
          bytes += group.packets.at(s).payload_size();
        auto buffer = webrtc::EncodedImageBuffer::Create(bytes);
        size_t position = 0;
        for (auto s = *group.first; s <= *group.last; ++s) {
          const auto& p = group.packets.at(s);
          std::memcpy(buffer->data() + position, p.payload().data(), p.payload_size());
          position += p.payload_size();
          saved.packets.push_back(p);
        }
        auto& tail = saved.packets.back();
        auto rotation = webrtc::kVideoRotation_0;
        tail.GetExtension<webrtc::VideoOrientation>(&rotation);
        auto native = std::make_unique<webrtc::RtpFrameObject>(
            uint16_t(*group.first), uint16_t(*group.last), tail.Marker(), 0, 0, 0,
            packet.Timestamp(), 0, webrtc::VideoSendTiming(), packet.PayloadType(), codec, rotation,
            webrtc::VideoContentType::UNSPECIFIED, saved.header, std::nullopt, std::nullopt,
            webrtc::RtpPacketInfos(), buffer);
        auto start = native->first_seq_num();
        previous_end = std::max(previous_end.value_or(*group.last), *group.last);
        saved.SetEncodedData(buffer);
        if (legacy)
          legacy_packets.erase(legacy_packets.lower_bound(*group.first),
                               legacy_packets.upper_bound(*group.last));
        waiting[start] = std::move(saved);
        groups.erase(key);
        ready(references.ManageFrame(std::move(native)));
        // A reordered next picture may already be complete except for its
        // inferred first-packet boundary. Revisit it when this picture closes.
        if (codec == webrtc::kVideoCodecH264 || codec == webrtc::kVideoCodecH265 ||
            codec == webrtc::kVideoCodecAV1) {
          std::optional<webrtc::RtpPacketReceived> next;
          for (auto& [_, pending] : groups)
            if (!pending.first && previous_end) {
              auto found = pending.packets.find(*previous_end + 1);
              if (found != pending.packets.end()) {
                next = found->second;
                pending.bytes -= found->second.payload_size();
                pending.packets.erase(found);
                break;
              }
            }
          if (next)
            receive(*next);
        }
      }
    }
    for (auto it = groups.begin(); it != groups.end();) {
      if (!it->second.packets.empty() &&
          it->second.packets.rbegin()->first < newest_sequence - 2048)
        it = groups.erase(it);
      else
        ++it;
    }
    while (groups.size() > 16) {
      auto oldest =
          std::min_element(groups.begin(), groups.end(), [](const auto& a, const auto& b) {
            return a.second.packets.begin()->first < b.second.packets.begin()->first;
          });
      groups.erase(oldest);
    }
    for (auto it = waiting.begin(); it != waiting.end();)
      if (uint16_t(newest_sequence - it->first) > 2048)
        it = waiting.erase(it);
      else
        ++it;
    size_t retained = 0;
    for (const auto& [_, frame] : waiting)
      retained += frame.size() * 2;
    while (!waiting.empty() && (waiting.size() > 128 || retained > 8 * 1024 * 1024)) {
      auto oldest =
          std::max_element(waiting.begin(), waiting.end(), [&](const auto& a, const auto& b) {
            return uint16_t(newest_sequence - a.first) < uint16_t(newest_sequence - b.first);
          });
      retained -= oldest->second.size() * 2;
      waiting.erase(oldest);
    }
    if (new_structure && !pending_structure.empty()) {
      auto pending = std::move(pending_structure);
      pending_structure.clear();
      pending_structure_bytes = 0;
      for (const auto& [_, saved] : pending)
        receive(saved);
    }
  }
};

NativeOpaqueAssembler::NativeOpaqueAssembler(webrtc::VideoCodecType codec,
                                             std::function<void(const webrtc::EncodedFrame&)> cb)
    : impl_(std::make_unique<Impl>(codec, std::move(cb))) {}

NativeOpaqueAssembler::~NativeOpaqueAssembler() = default;

void NativeOpaqueAssembler::receive(const webrtc::RtpPacketReceived& p) {
  impl_->receive(p);
}
}  // namespace lk
