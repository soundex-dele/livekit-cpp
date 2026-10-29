#include "lk/native_rtp.hpp"
#include "lk/native_opaque.hpp"
#include "lk/audio_red.hpp"
#include "lk/rtp_payload_rewrite.hpp"
#include <modules/rtp_rtcp/source/rtp_dependency_descriptor_extension.h>
#include <modules/rtp_rtcp/source/rtp_generic_frame_descriptor_extension.h>
#include <api/call/transport.h>
#include <api/video/rtp_video_frame_assembler.h>
#include <call/rtx_receive_stream.h>
#include <modules/rtp_rtcp/include/receive_statistics.h>
#include <modules/rtp_rtcp/source/rtp_header_extensions.h>
#include <modules/rtp_rtcp/source/rtp_packet_history.h>
#include <modules/rtp_rtcp/source/rtp_sender.h>
#include <modules/rtp_rtcp/source/rtcp_receiver.h>
#include <modules/rtp_rtcp/source/rtp_rtcp_impl2.h>
#include <modules/rtp_rtcp/source/packet_sequencer.h>
#include <modules/rtp_rtcp/source/rtp_format_vp8.h>
#include <modules/rtp_rtcp/source/rtp_format_vp9.h>
#include <modules/rtp_rtcp/source/rtp_format_h264.h>
#include <modules/rtp_rtcp/source/video_rtp_depacketizer_h264.h>
#include <modules/rtp_rtcp/source/frame_object.h>
#include <modules/video_coding/h26x_packet_buffer.h>
#include <modules/video_coding/rtp_frame_reference_finder.h>
#include <rtc_base/numerics/sequence_number_unwrapper.h>
#include <modules/video_coding/nack_requester.h>
#include <modules/congestion_controller/include/receive_side_congestion_controller.h>
#include <modules/rtp_rtcp/source/rtcp_packet/remb.h>
#include <rtc_base/task_utils/repeating_task.h>
#include <rtc_base/rate_limiter.h>
#include <rtc_base/crypto_random.h>
#include <rtc_base/logging.h>
#include <rtc_base/numerics/sequence_number_util.h>
#include <modules/include/module_common_types_public.h>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <deque>
#include <set>

namespace lk {
const webrtc::RTPVideoHeaderVP9* native_vp9_header(const webrtc::EncodedFrame& frame) {
  const auto* header = forwarding_header(frame);
  return header ? std::get_if<webrtc::RTPVideoHeaderVP9>(&header->video_type_header) : nullptr;
}

struct NativeReceiveTransport::Impl {
  Rtcp send;
  uint32_t feedback_ssrc = webrtc::CreateRandomNonZeroId();
  webrtc::ReceiveSideCongestionController controller;
  webrtc::RepeatingTaskHandle task;

  Impl(const webrtc::Environment& env, Rtcp callback)
      : send(std::move(callback)),
        controller(
            env,
            [this](std::vector<std::unique_ptr<webrtc::rtcp::RtcpPacket>> packets) {
              for (const auto& packet : packets) {
                auto b = packet->Build();
                send({b.data(), b.size()});
              }
            },
            [this](int64_t bitrate, std::vector<uint32_t> ssrcs) {
              webrtc::rtcp::Remb remb;
              remb.SetSenderSsrc(feedback_ssrc);
              remb.SetBitrateBps(bitrate);
              remb.SetSsrcs(std::move(ssrcs));
              auto b = remb.Build();
              send({b.data(), b.size()});
            }) {
    task = webrtc::RepeatingTaskHandle::Start(
        webrtc::TaskQueueBase::Current(), [this] { return controller.MaybeProcess(); },
        webrtc::TaskQueueBase::DelayPrecision::kLow, &env.clock());
  }

  ~Impl() {
    task.Stop();
  }
};

NativeReceiveTransport::NativeReceiveTransport(const webrtc::Environment& env, Rtcp send)
    : impl_(std::make_unique<Impl>(env, std::move(send))) {}

NativeReceiveTransport::~NativeReceiveTransport() = default;

void NativeReceiveTransport::receive(const webrtc::RtpPacketReceived& packet, bool audio) {
  impl_->controller.OnReceivedPacket(packet,
                                     audio ? webrtc::MediaType::AUDIO : webrtc::MediaType::VIDEO);
}

webrtc::TimeDelta NativeReceiveTransport::process() {
  return impl_->controller.MaybeProcess();
}

struct NativeRtpSender::Impl : webrtc::RtpPacketSender,
                               webrtc::RTCPReceiver::ModuleRtpRtcp,
                               webrtc::RtcpIntraFrameObserver {
  webrtc::Environment env;
  NativeSendConfig config;
  Send send;
  std::function<void()> keyframe;
  std::shared_ptr<NativeSendTransport> transport;
  webrtc::RtpPacketHistory history;
  webrtc::RateLimiter limiter;
  webrtc::PacketSequencer sequencer;
  std::unique_ptr<webrtc::RTPSender> sender;
  std::unique_ptr<webrtc::RTCPReceiver> receiver;
  bool enabled = true, media_paused = false;
  std::optional<uint16_t> offset;
  uint16_t next_sequence;
  uint32_t packets = 0, octets = 0;
  uint32_t frame_source = 0, frame_timestamp_offset = 0;
  std::optional<uint32_t> last_frame_timestamp;
  webrtc::Timestamp last_frame_time = webrtc::Timestamp::MinusInfinity();
  uint16_t picture_id = 0;
  int64_t descriptor_offset = 0;
  std::optional<int64_t> last_descriptor_id;
  std::shared_ptr<const webrtc::FrameDependencyStructure> last_structure;
  int vp9_spatial_layers = 1, last_spatial_layer = -1;
  int vp9_picture_offset = 0, vp9_tl0_offset = 0;
  std::optional<int> last_vp9_picture, last_vp9_tl0;
  std::optional<int64_t> source_picture;
  std::set<int64_t> admitted_frames;
  bool needs_keyframe = true;
  bool batch_admitted = true;
  int frame_diagnostics = 0;
  NativeRtpSender::Quality quality;
  AudioRedEncoder red_encoder;

  Impl(const webrtc::Environment& e, NativeSendConfig c, Send s, std::function<void()> k,
       std::shared_ptr<NativeSendTransport> t)
      : env(e),
        config(std::move(c)),
        send(std::move(s)),
        keyframe(std::move(k)),
        transport(std::move(t)),
        history(env, webrtc::RtpPacketHistory::PaddingMode::kDefault),
        limiter(&env.clock(), 1000),
        sequencer(config.ssrc, config.rtx_ssrc, !config.audio, &env.clock()),
        next_sequence(sequencer.media_sequence_number()) {
    if (!config.ssrc || config.payload_type < 0 || config.payload_type > 127)
      throw std::invalid_argument("Invalid SFU sender configuration");
    history.SetStorePacketsStatus(webrtc::RtpPacketHistory::StorageMode::kStoreAndCull, 512);
    limiter.SetMaxRate(2000000);
    webrtc::RtpRtcpInterface::Configuration native;
    native.audio = config.audio;
    native.local_media_ssrc = config.ssrc;
    native.rtx_send_ssrc = config.rtx_ssrc;
    native.extmap_allow_mixed = config.extmap_allow_mixed;
    native.always_send_mid_and_rid = true;
    native.intra_frame_callback = this;
    native.retransmission_rate_limiter = &limiter;
    sender = std::make_unique<webrtc::RTPSender>(env, native, &history, this);
    receiver = std::make_unique<webrtc::RTCPReceiver>(env, native, this);
    sender->SetMaxRtpPacketSize(2000);
    for (const auto& ext : config.extensions) {
      // Hop-specific identifiers are populated by the shared send transport.
      if (ext.uri == webrtc::RtpExtension::kMidUri ||
          ext.uri == webrtc::RtpExtension::kAudioLevelUri ||
          ext.uri == webrtc::RtpExtension::kVideoRotationUri ||
          ext.uri == webrtc::RtpExtension::kDependencyDescriptorUri ||
          ext.uri == webrtc::RtpExtension::kGenericFrameDescriptorUri00 ||
          ext.uri == webrtc::RtpExtension::kAbsoluteCaptureTimeUri ||
          (transport && (ext.uri == webrtc::RtpExtension::kTransportSequenceNumberUri ||
                         ext.uri == webrtc::RtpExtension::kAbsSendTimeUri)))
        sender->RegisterRtpHeaderExtension(ext.uri, ext.id);
    }
    sender->SetMid(config.mid);
    if (config.rtx_ssrc && config.rtx_payload_type >= 0) {
      sender->SetRtxPayloadType(config.rtx_payload_type, config.payload_type);
      sender->SetRtxStatus(webrtc::kRtxRetransmitted);
    }
    if (transport)
      transport->attach(
          config.ssrc, config.rtx_ssrc,
          {[this](auto& packet) {
             if (packet.packet_type() == webrtc::RtpPacketMediaType::kRetransmission ||
                 packet.packet_type() == webrtc::RtpPacketMediaType::kPadding)
               sequencer.Sequence(packet);
           },
           [this](const auto& packet) {
             return enabled &&
                    (!media_paused ||
                     packet.packet_type() == webrtc::RtpPacketMediaType::kPadding) &&
                    send(packet);
           },
           [this](auto packet, bool sent) { complete(std::move(packet), sent); },
           [this](size_t size) {
             std::vector<std::unique_ptr<webrtc::RtpPacketToSend>> padding;
             if (enabled && !config.audio && config.rtx_ssrc)
               padding = sender->GeneratePadding(size, packets > 0, false);
             return padding;
           },
           [this](webrtc::ArrayView<const uint16_t> seq) {
             for (auto number : seq)
               history.MarkPacketAsSent(number);
           }});
  }

  ~Impl() {
    if (transport)
      transport->detach(config.ssrc, config.rtx_ssrc);
  }

  void complete(std::unique_ptr<webrtc::RtpPacketToSend> packet, bool sent) {
    if (packet->packet_type() == webrtc::RtpPacketMediaType::kRetransmission)
      history.MarkPacketAsSent(*packet->retransmitted_sequence_number());
    else if (packet->packet_type() != webrtc::RtpPacketMediaType::kPadding) {
      if (sent) {
        ++packets;
        octets += uint32_t(packet->payload_size());
      }
      history.PutRtpPacket(std::move(packet), env.clock().CurrentTime());
    }
  }

  void EnqueuePackets(std::vector<std::unique_ptr<webrtc::RtpPacketToSend>> batch) override {
    for (auto& packet : batch) {
      bool retransmit = packet->packet_type() == webrtc::RtpPacketMediaType::kRetransmission;
      if (retransmit && !transport)
        sequencer.Sequence(*packet);
      if (!transport) {
        bool sent = enabled && send(*packet);
        complete(std::move(packet), sent);
      }
    }
    if (transport)
      batch_admitted = transport->enqueue(std::move(batch));
  }

  void SetTmmbn(std::vector<webrtc::rtcp::TmmbItem>) override {}

  void OnRequestSendReport() override {}

  void OnReceivedNack(const std::vector<uint16_t>& sequences) override {
    if (transport)
      limiter.SetMaxRate(uint32_t(std::max<int64_t>(30000, transport->stats().target_bps)));
    if (enabled && !media_paused)
      sender->OnReceivedNack(sequences,
                             receiver->AverageRtt().value_or(webrtc::TimeDelta::Millis(100)).ms());
  }

  void OnReceivedRtcpReportBlocks(
      webrtc::ArrayView<const webrtc::ReportBlockData> reports) override {
    for (const auto& report : reports)
      if (report.source_ssrc() == config.ssrc) {
        quality.loss = report.fraction_lost();
        quality.has_feedback = true;
        quality.lost = report.cumulative_lost();
        quality.last_rtcp_ms = env.clock().CurrentTime().ms();
      }
    quality.rtt_ms = receiver->AverageRtt().value_or(webrtc::TimeDelta::Zero()).ms();
  }

  void OnReceivedIntraFrameRequest(uint32_t ssrc) override {
    if (enabled && ssrc == config.ssrc && keyframe)
      keyframe();
  }

  void forward(const webrtc::RtpPacketReceived& received, int source_red_pt, int source_primary_pt,
               bool encrypted) {
    if (!enabled || media_paused || received.payload_size() == 0)
      return;
    if (!offset)
      offset = uint16_t(next_sequence - received.SequenceNumber());
    auto packet = sender->AllocatePacket(received.Csrcs());
    auto payload = received.payload();
    std::vector<uint8_t> red;
    int payload_type = config.payload_type;
    if (config.audio && encrypted) {
      // Insertable-stream encryption authenticates the encoded audio frame.
      // Adding/removing RED changes that frame before the receiver decrypts
      // it. Preserve its outer format and every authenticated byte.
      if (received.PayloadType() == source_red_pt) {
        if (config.red_payload_type < 0)
          return;
        payload_type = config.red_payload_type;
      }
    } else if (config.audio && received.PayloadType() == source_red_pt) {
      auto envelope = parse_audio_red(payload, source_primary_pt);
      if (!envelope)
        return;
      if (config.red_payload_type >= 0) {
        red.assign(payload.begin(), payload.end());
        for (auto offset : envelope->type_offsets)
          red[offset] = (red[offset] & 0x80) | uint8_t(config.payload_type);
        payload = red;
        payload_type = config.red_payload_type;
      } else
        payload = payload.subview(envelope->primary_offset);
    } else if (config.audio && config.red_payload_type >= 0) {
      red = red_encoder.encode(payload, received.Timestamp(), config.payload_type,
                               packet->FreeCapacity());
      payload = red;
      payload_type = config.red_payload_type;
    }
    packet->SetPayloadType(uint8_t(payload_type));
    packet->SetSequenceNumber(uint16_t(received.SequenceNumber() + *offset));
    packet->SetTimestamp(received.Timestamp());
    packet->SetMarker(received.Marker());
    packet->set_packet_type(config.audio ? webrtc::RtpPacketMediaType::kAudio
                                         : webrtc::RtpPacketMediaType::kVideo);
    packet->set_allow_retransmission(true);
    packet->set_capture_time(env.clock().CurrentTime());
    if (auto extension = received.GetRawExtension<webrtc::AudioLevelExtension>();
        !extension.empty())
      packet->SetRawExtension<webrtc::AudioLevelExtension>(extension);
    if (auto extension = received.GetRawExtension<webrtc::VideoOrientation>(); !extension.empty())
      packet->SetRawExtension<webrtc::VideoOrientation>(extension);
    if (payload.size() > packet->FreeCapacity())
      return;
    std::memcpy(packet->AllocatePayload(payload.size()), payload.data(), payload.size());
    if (packet->SequenceNumber() == next_sequence ||
        webrtc::IsNewerSequenceNumber(packet->SequenceNumber(), next_sequence))
      next_sequence = uint16_t(packet->SequenceNumber() + 1);
    std::vector<std::unique_ptr<webrtc::RtpPacketToSend>> batch;
    batch.push_back(std::move(packet));
    sender->EnqueuePackets(std::move(batch));
  }

  bool forward_frame(const webrtc::EncodedFrame& frame, uint32_t source_ssrc, int max_spatial_layer,
                     int max_temporal_layer) {
    if (!enabled || media_paused || config.audio || !frame.size() || !source_ssrc)
      return false;
    const auto* vp9 =
        config.video_codec == webrtc::kVideoCodecVP9 ? native_vp9_header(frame) : nullptr;
    if (config.video_codec == webrtc::kVideoCodecVP9 && !vp9)
      return false;
    const int spatial = frame_spatial(frame);
    if (frame_temporal(frame) > max_temporal_layer)
      return false;
    const auto* forwarded = dynamic_cast<const ForwardingFrame*>(&frame);
    const auto* metadata = forwarding_header(frame);
    if (spatial > max_spatial_layer)
      return false;
    bool switching = frame_source != source_ssrc;
    if (!frame.is_keyframe()) {
      bool missing = needs_keyframe || switching ||
                     frame.num_references > webrtc::EncodedFrame::kMaxFrameReferences;
      for (size_t i = 0; !missing && i < frame.num_references; ++i)
        missing = !admitted_frames.contains(frame.references[i]);
      if (missing) {
        if (frame_diagnostics < 8) {
          ++frame_diagnostics;
          RTC_LOG(LS_INFO) << "SFU frame gate missing dependency source=" << source_ssrc
                           << " id=" << frame.Id() << " switching=" << switching;
        }
        if (keyframe)
          keyframe();
        return false;
      }
    }
    if (!switching && admitted_frames.contains(frame.Id())) {
      if (frame_diagnostics < 8) {
        ++frame_diagnostics;
        RTC_LOG(LS_INFO) << "SFU frame gate duplicate source=" << source_ssrc
                         << " id=" << frame.Id();
      }
      return false;
    }
    auto now = env.clock().CurrentTime();
    uint32_t timestamp_offset = frame_timestamp_offset;
    if (switching) {
      timestamp_offset = 0;
      if (last_frame_timestamp &&
          (!webrtc::IsNewerTimestamp(frame.RtpTimestamp(), *last_frame_timestamp) ||
           uint32_t(frame.RtpTimestamp() - *last_frame_timestamp) > 450000)) {
        auto elapsed = std::clamp<int64_t>((now - last_frame_time).us() * 90 / 1000, 1, 90000);
        timestamp_offset = uint32_t(*last_frame_timestamp + elapsed - frame.RtpTimestamp());
      }
    }
    uint32_t timestamp = frame.RtpTimestamp() + timestamp_offset;
    // VP9 emits one encoded frame per spatial layer at the same RTP timestamp.
    // Keep decode order within a picture while still rejecting old pictures.
    const bool same_picture = last_frame_timestamp && timestamp == *last_frame_timestamp;
    if (last_frame_timestamp && !webrtc::IsNewerTimestamp(timestamp, *last_frame_timestamp) &&
        !((vp9 || (metadata && metadata->generic)) && !switching && same_picture &&
          spatial > last_spatial_layer)) {
      if (frame_diagnostics < 8) {
        ++frame_diagnostics;
        RTC_LOG(LS_INFO) << "SFU frame gate timestamp source=" << source_ssrc
                         << " timestamp=" << timestamp << " last=" << *last_frame_timestamp
                         << " offset=" << timestamp_offset;
      }
      return false;
    }
    webrtc::RTPVideoHeaderVP8 header;
    header.InitRTPVideoHeaderVP8();
    const auto* vp8 =
        metadata ? std::get_if<webrtc::RTPVideoHeaderVP8>(&metadata->video_type_header) : nullptr;
    if (vp8)
      header = *vp8;
    webrtc::RtpPacketizer::PayloadSizeLimits limits;
    limits.max_payload_len = 1100;
    std::unique_ptr<webrtc::RtpPacketizer> packetizer;
    auto spatial_layers = vp9_spatial_layers;
    auto picture_offset = vp9_picture_offset, tl0_offset = vp9_tl0_offset;
    const bool opaque = forwarded && !forwarded->packets.empty();
    int input_picture = vp9 ? vp9->picture_id : vp8 ? vp8->pictureId : -1;
    int input_tl0 = vp9 ? vp9->tl0_pic_idx : vp8 ? vp8->tl0PicIdx : -1;
    int mask = vp9 ? vp9->max_picture_id : 0x7fff;
    if (vp8 && opaque) {
      const auto p = forwarded->packets.front().payload();
      if (p.size() > 2 && (p[0] & 0x80) && (p[1] & 0x80) && !(p[2] & 0x80))
        mask = 0x7f;
    }
    auto extended_picture = source_picture;
    int output_picture = -1, output_tl0 = -1;
    if (input_picture >= 0) {
      int64_t value = input_picture;
      if (!switching && source_picture) {
        value = (*source_picture & ~int64_t(mask)) | input_picture;
        if (value - *source_picture > mask / 2)
          value -= mask + 1;
        else if (*source_picture - value > mask / 2)
          value += mask + 1;
      }
      extended_picture = value;
      if (switching)
        picture_offset = last_vp9_picture ? *last_vp9_picture + 1 - int(value) : 0;
      output_picture = int(value + picture_offset) & 0x7fff;
    }
    if (input_tl0 >= 0) {
      if (switching)
        tl0_offset = last_vp9_tl0 ? *last_vp9_tl0 + 1 - input_tl0 : 0;
      output_tl0 = (input_tl0 + tl0_offset) & 0xff;
    }
    header.pictureId = output_picture >= 0 ? int16_t(output_picture) : int16_t(picture_id & 0x7fff);
    if (output_tl0 >= 0)
      header.tl0PicIdx = output_tl0;
    auto dd_offset = descriptor_offset;
    if (switching)
      dd_offset = last_descriptor_id ? *last_descriptor_id + 1 - frame.Id() : 0;
    if (opaque) {
    } else if (config.video_codec == webrtc::kVideoCodecH264)
      packetizer = std::make_unique<webrtc::RtpPacketizerH264>(
          webrtc::ArrayView<const uint8_t>(frame.data(), frame.size()), limits,
          config.h264_packetization_mode ? webrtc::H264PacketizationMode::NonInterleaved
                                         : webrtc::H264PacketizationMode::SingleNalUnit);
    else if (config.video_codec == webrtc::kVideoCodecVP8)
      packetizer = std::make_unique<webrtc::RtpPacketizerVp8>(
          webrtc::ArrayView<const uint8_t>(frame.data(), frame.size()), limits, header);
    else if (vp9) {
      auto header = *vp9;
      if (switching)
        spatial_layers = 1;
      if (header.ss_data_available)
        spatial_layers = int(header.num_spatial_layers);
      spatial_layers =
          std::max(spatial_layers, spatial + 1 + (frame.is_last_spatial_layer ? 0 : 1));
      header.num_spatial_layers = std::min(spatial_layers, max_spatial_layer + 1);
      header.end_of_picture = frame.is_last_spatial_layer || spatial == max_spatial_layer;
      // Keep the source's temporal dependencies/GOF. A constant per-source
      // offset makes picture/TL0 identifiers continuous across source switches.
      if (header.picture_id != webrtc::kNoPictureId) {
        header.picture_id = int16_t(output_picture);
        header.max_picture_id = webrtc::kMaxTwoBytePictureId;
      }
      if (header.tl0_pic_idx != webrtc::kNoTl0PicIdx) {
        header.tl0_pic_idx = int16_t(output_tl0);
      }
      packetizer = std::make_unique<webrtc::RtpPacketizerVp9>(
          webrtc::ArrayView<const uint8_t>(frame.data(), frame.size()), limits, header);
    } else
      return false;
    std::vector<std::unique_ptr<webrtc::RtpPacketToSend>> batch;
    auto sequence = next_sequence;
    size_t opaque_index = 0;
    while (opaque ? opaque_index < forwarded->packets.size() : packetizer->NumPackets() != 0) {
      auto packet = sender->AllocatePacket({});
      packet->SetPayloadType(uint8_t(config.payload_type));
      packet->SetTimestamp(timestamp);
      packet->SetSequenceNumber(sequence++);
      packet->set_packet_type(webrtc::RtpPacketMediaType::kVideo);
      packet->set_allow_retransmission(true);
      packet->set_capture_time(now);
      packet->set_is_key_frame(frame.is_keyframe());
      packet->SetExtension<webrtc::VideoOrientation>(frame.rotation());
      const bool first_packet = batch.empty();
      const bool last_packet =
          opaque ? opaque_index + 1 == forwarded->packets.size() : packetizer->NumPackets() == 1;
      if (metadata && metadata->generic) {
        const auto& generic = *metadata->generic;
        if (forwarded && forwarded->structure &&
            sender->IsRtpHeaderExtensionRegistered(webrtc::kRtpExtensionDependencyDescriptor)) {
          const auto& structure = *forwarded->structure;
          webrtc::DependencyDescriptor d;
          d.first_packet_in_frame = first_packet;
          d.last_packet_in_frame = last_packet;
          d.frame_number = uint16_t(frame.Id() + dd_offset);
          d.frame_dependencies.spatial_id = generic.spatial_index;
          d.frame_dependencies.temporal_id = generic.temporal_index;
          d.frame_dependencies.decode_target_indications = generic.decode_target_indications;
          d.frame_dependencies.chain_diffs = generic.chain_diffs;
          for (auto dependency : generic.dependencies)
            d.frame_dependencies.frame_diffs.push_back(int(generic.frame_id - dependency));
          uint32_t active = 0;
          for (int target = 0; target < structure.num_decode_targets; ++target) {
            int s = 0, t = 0;
            for (const auto& item : structure.templates)
              if (item.decode_target_indications[target] !=
                  webrtc::DecodeTargetIndication::kNotPresent) {
                s = std::max(s, item.spatial_id);
                t = std::max(t, item.temporal_id);
              }
            if (s <= max_spatial_layer && t <= max_temporal_layer)
              active |= uint32_t{1} << target;
          }
          d.active_decode_targets_bitmask =
              active & uint32_t(generic.active_decode_targets.to_ulong());
          if (first_packet && (frame.is_keyframe() || forwarded->structure != last_structure))
            d.attached_structure = std::make_unique<webrtc::FrameDependencyStructure>(structure);
          if (!config.extmap_allow_mixed &&
              webrtc::RtpDependencyDescriptorExtension::ValueSize(structure, d) > 16)
            return false;
          if (!packet->SetExtension<webrtc::RtpDependencyDescriptorExtension>(structure, d))
            return false;
        } else if (sender->IsRtpHeaderExtensionRegistered(
                       webrtc::kRtpExtensionGenericFrameDescriptor)) {
          webrtc::RtpGenericFrameDescriptor d;
          d.SetFirstPacketInSubFrame(first_packet);
          d.SetLastPacketInSubFrame(last_packet);
          d.SetFrameId(uint16_t(frame.Id() + dd_offset));
          d.SetSpatialLayersBitmask(uint8_t(1 << spatial));
          d.SetTemporalLayer(generic.temporal_index);
          if (frame.is_keyframe())
            d.SetResolution(frame._encodedWidth, frame._encodedHeight);
          for (auto dependency : generic.dependencies)
            if (!d.AddFrameDependencyDiff(uint16_t(generic.frame_id - dependency)))
              return false;
          if (!packet->SetExtension<webrtc::RtpGenericFrameDescriptorExtension00>(d))
            return false;
        }
      }
      if (opaque) {
        const auto& input = forwarded->packets[opaque_index++];
        if (auto ext = input.GetRawExtension<webrtc::AbsoluteCaptureTimeExtension>(); !ext.empty())
          packet->SetRawExtension<webrtc::AbsoluteCaptureTimeExtension>(ext);
        auto payload = rewrite_video_descriptor(input.payload(), config.video_codec, output_picture,
                                                output_tl0, max_spatial_layer);
        if (!payload || payload->size() > packet->FreeCapacity())
          return false;
        std::memcpy(packet->AllocatePayload(payload->size()), payload->data(), payload->size());
        packet->SetMarker(last_packet &&
                          (frame.is_last_spatial_layer || spatial == max_spatial_layer));
      } else {
        if (!packetizer->NextPacket(packet.get()))
          return false;
      }
      batch.push_back(std::move(packet));
    }
    if (batch.empty())
      return false;
    batch_admitted = true;
    sender->EnqueuePackets(std::move(batch));
    if (!batch_admitted) {
      if (!switching)
        needs_keyframe = true;
      if (keyframe)
        keyframe();
      return false;
    }
    next_sequence = sequence;
    ++picture_id;
    if (switching || (frame.is_keyframe() && !same_picture))
      admitted_frames.clear();
    admitted_frames.insert(frame.Id());
    while (admitted_frames.size() > 512)
      admitted_frames.erase(admitted_frames.begin());
    needs_keyframe = false;
    frame_source = source_ssrc;
    frame_timestamp_offset = timestamp_offset;
    last_frame_timestamp = timestamp;
    last_frame_time = now;
    last_spatial_layer = spatial;
    if (metadata && metadata->generic) {
      descriptor_offset = dd_offset;
      last_descriptor_id = frame.Id() + dd_offset;
    }
    if (forwarded)
      last_structure = forwarded->structure;
    // Commit codec state only after the entire picture fragment batch is admitted.
    vp9_spatial_layers = spatial_layers;
    vp9_picture_offset = picture_offset;
    vp9_tl0_offset = tl0_offset;
    source_picture = extended_picture;
    if (output_picture >= 0)
      last_vp9_picture = output_picture;
    if (output_tl0 >= 0)
      last_vp9_tl0 = output_tl0;
    return true;
  }
};

NativeRtpSender::NativeRtpSender(const webrtc::Environment& env, NativeSendConfig config, Send send,
                                 std::function<void()> keyframe,
                                 std::shared_ptr<NativeSendTransport> transport)
    : impl_(std::make_unique<Impl>(env, std::move(config), std::move(send), std::move(keyframe),
                                   std::move(transport))) {}

NativeRtpSender::~NativeRtpSender() = default;

void NativeRtpSender::forward(const webrtc::RtpPacketReceived& packet, int red, int primary,
                              bool encrypted) {
  impl_->forward(packet, red, primary, encrypted);
}

bool NativeRtpSender::forward_frame(const webrtc::EncodedFrame& frame, uint32_t source_ssrc,
                                    int max_spatial_layer, int max_temporal_layer) {
  return impl_->forward_frame(frame, source_ssrc, max_spatial_layer, max_temporal_layer);
}

void NativeRtpSender::feedback(webrtc::ArrayView<const uint8_t> packet) {
  impl_->receiver->IncomingPacket(packet);
}

NativeRtpSender::Quality NativeRtpSender::quality() const {
  auto q = impl_->quality;
  q.packets = impl_->packets;
  q.bytes = impl_->octets;
  return q;
}

void NativeRtpSender::set_enabled(bool enabled) {
  if (impl_->enabled == enabled)
    return;
  impl_->enabled = enabled;
  impl_->offset.reset();
  impl_->history.Clear();
  impl_->red_encoder.clear();
  impl_->needs_keyframe = true;
  impl_->admitted_frames.clear();
  if (impl_->transport)
    impl_->transport->discard(impl_->config.ssrc, impl_->config.rtx_ssrc);
  impl_->sender->SetSendingMediaStatus(enabled);
}

void NativeRtpSender::set_media_paused(bool paused) {
  if (impl_->media_paused == paused)
    return;
  impl_->media_paused = paused;
  // Pause already cleared media state. On resume retain padding currently in
  // the native pacer so opening a video does not truncate an active probe.
  if (!paused)
    return;
  impl_->offset.reset();
  impl_->history.Clear();
  impl_->red_encoder.clear();
  impl_->needs_keyframe = true;
  impl_->admitted_frames.clear();
  if (impl_->transport)
    impl_->transport->discard(impl_->config.ssrc, impl_->config.rtx_ssrc);
}

NativeRtpSender::Continuity NativeRtpSender::continuity() const {
  return {impl_->config.ssrc,
          impl_->packets,
          impl_->octets,
          impl_->config.rtx_ssrc,
          impl_->last_frame_timestamp,
          impl_->next_sequence,
          impl_->sequencer.rtx_sequence_number(),
          impl_->last_frame_time};
}

void NativeRtpSender::restore_continuity(const Continuity& previous) {
  if (previous.ssrc != impl_->config.ssrc)
    return;
  impl_->next_sequence = previous.sequence;
  impl_->sequencer.set_media_sequence_number(previous.sequence);
  if (previous.rtx_ssrc == impl_->config.rtx_ssrc)
    impl_->sequencer.set_rtx_sequence_number(previous.rtx_sequence);
  impl_->packets = previous.packets;
  impl_->octets = previous.octets;
  impl_->last_frame_timestamp = previous.timestamp;
  impl_->last_frame_time = previous.at;
}

webrtc::rtcp::SenderReport NativeRtpSender::translate_report(
    const webrtc::rtcp::SenderReport& original) const {
  webrtc::rtcp::SenderReport report;
  report.SetSenderSsrc(impl_->config.ssrc);
  report.SetNtp(original.ntp());
  report.SetRtpTimestamp(original.rtp_timestamp() + impl_->frame_timestamp_offset);
  report.SetPacketCount(impl_->packets);
  report.SetOctetCount(impl_->octets);
  return report;
}

struct NativeVideoAssembler::Impl {
  Frame callback;
  webrtc::RtpVideoFrameAssembler assembler;

  struct H264 {
    webrtc::VideoRtpDepacketizerH264 depacketizer;
    webrtc::H26xPacketBuffer packets{true};
    webrtc::RtpFrameReferenceFinder references;
    webrtc::SeqNumUnwrapper<uint16_t> sequences;

    webrtc::RtpFrameReferenceFinder::ReturnVector receive(const webrtc::RtpPacketReceived& rtp) {
      webrtc::RtpFrameReferenceFinder::ReturnVector frames;
      references.ClearTo(uint16_t(rtp.SequenceNumber() - 2000));
      if (!rtp.payload_size()) {
        auto padding = packets.InsertPadding(rtp.SequenceNumber());
        return references.PaddingReceived(rtp.SequenceNumber());
      }
      auto parsed = depacketizer.Parse(rtp.PayloadBuffer());
      if (!parsed)
        return frames;
      // Native parsing may rewrite SPS VUI for a local decoder. An SFU must
      // preserve the publisher's NAL bytes; use parsed metadata but the original
      // STAP-A/single-NAL payload. FU-A's native header removal is required.
      if ((rtp.payload()[0] & 0x1f) != 28)
        parsed->video_payload = rtp.PayloadBuffer();
      parsed->video_header.is_last_packet_in_frame = rtp.Marker();
      auto packet = std::make_unique<webrtc::H26xPacketBuffer::Packet>(
          rtp, sequences.Unwrap(rtp.SequenceNumber()), parsed->video_header);
      packet->video_payload = std::move(parsed->video_payload);
      auto assembled = packets.InsertPacket(std::move(packet));
      webrtc::H26xPacketBuffer::Packet* first = nullptr;
      std::vector<webrtc::ArrayView<const uint8_t>> payloads;
      for (const auto& part : assembled.packets) {
        if (part->is_first_packet_in_frame()) {
          first = part.get();
          payloads.clear();
        }
        payloads.emplace_back(part->video_payload);
        if (!part->is_last_packet_in_frame() || !first)
          continue;
        auto data = depacketizer.AssembleFrame(payloads);
        if (!data)
          continue;
        auto frame = std::make_unique<webrtc::RtpFrameObject>(
            first->seq_num(), part->seq_num(), part->marker_bit, 0, 0, 0, first->timestamp, 0,
            webrtc::VideoSendTiming(), first->payload_type, webrtc::kVideoCodecH264,
            part->video_header.rotation, part->video_header.content_type, first->video_header,
            part->video_header.color_space, part->video_header.frame_instrumentation_data,
            webrtc::RtpPacketInfos(), std::move(data));
        for (auto& ready : references.ManageFrame(std::move(frame)))
          frames.push_back(std::move(ready));
      }
      return frames;
    }
  };

  std::unique_ptr<H264> h264;
  std::unique_ptr<NativeOpaqueAssembler> opaque;
  // M137's assembler does not extract the orientation extension itself.
  // Preserve it with native extension accessors, bounded by its packet window.
  std::deque<std::pair<uint32_t, webrtc::VideoRotation>> rotations;

  explicit Impl(Frame frame, webrtc::VideoCodecType codec, bool preserve)
      : callback(std::move(frame)),
        assembler(codec == webrtc::kVideoCodecH264  ? webrtc::RtpVideoFrameAssembler::kH264
                  : codec == webrtc::kVideoCodecVP9 ? webrtc::RtpVideoFrameAssembler::kVp9
                                                    : webrtc::RtpVideoFrameAssembler::kVp8) {
    if (preserve || codec == webrtc::kVideoCodecAV1 || codec == webrtc::kVideoCodecH265)
      opaque = std::make_unique<NativeOpaqueAssembler>(codec, callback);
    else if (codec == webrtc::kVideoCodecH264)
      h264 = std::make_unique<H264>();
  }

  void receive(const webrtc::RtpPacketReceived& packet) {
    if (opaque) {
      opaque->receive(packet);
      return;
    }
    if (packet.Marker()) {
      auto rotation = webrtc::kVideoRotation_0;
      packet.GetExtension<webrtc::VideoOrientation>(&rotation);
      rotations.emplace_back(packet.Timestamp(), rotation);
      if (rotations.size() > 2048)
        rotations.pop_front();
    }
    auto deliver = [&](auto frame) {
      auto found = std::find_if(rotations.rbegin(), rotations.rend(), [&](const auto& entry) {
        return entry.first == frame->RtpTimestamp();
      });
      if (found != rotations.rend())
        frame->rotation_ = found->second;
      callback(*frame);
    };
    if (h264) {
      for (auto& frame : h264->receive(packet))
        deliver(std::move(frame));
    } else {
      for (auto& frame : assembler.InsertPacket(packet))
        deliver(frame.ExtractFrame());
    }
  }
};

NativeVideoAssembler::NativeVideoAssembler(Frame frame, webrtc::VideoCodecType codec, bool opaque)
    : impl_(std::make_unique<Impl>(std::move(frame), codec, opaque)) {}

NativeVideoAssembler::~NativeVideoAssembler() = default;

void NativeVideoAssembler::receive(const webrtc::RtpPacketReceived& packet) {
  impl_->receive(packet);
}

struct NativeRtpReceiver::Impl : webrtc::RtpPacketSinkInterface,
                                 webrtc::Transport,
                                 webrtc::NackSender,
                                 webrtc::KeyFrameRequestSender,
                                 webrtc::RtcpRttStats {
  webrtc::Environment env;
  NativeReceiveConfig config;
  Media media;
  Rtcp rtcp;
  std::unique_ptr<webrtc::ReceiveStatistics> statistics;
  std::unique_ptr<webrtc::ModuleRtpRtcpImpl2> module;
  std::unique_ptr<webrtc::NackPeriodicProcessor> nack_processor;
  std::unique_ptr<webrtc::NackRequester> nack;
  std::unique_ptr<webrtc::RtxReceiveStream> rtx;
  int64_t rtt_ms = 100;
  int64_t last_rtcp_ms = 0;
  webrtc::Timestamp last_keyframe = webrtc::Timestamp::MinusInfinity();

  Impl(const webrtc::Environment& e, NativeReceiveConfig c, Media m, Rtcp r)
      : env(e), config(c), media(std::move(m)), rtcp(std::move(r)) {
    if (!config.ssrc || config.clock_rate_hz <= 0 || !webrtc::TaskQueueBase::Current())
      throw std::invalid_argument(
          "SFU receiver requires signaled SSRC, positive RTP clock and native task queue");
    statistics = webrtc::ReceiveStatistics::CreateThreadCompatible(&env.clock());
    webrtc::RtpRtcpInterface::Configuration native;
    native.audio = config.audio;
    native.receiver_only = true;
    native.local_media_ssrc = webrtc::CreateRandomNonZeroId();
    native.outgoing_transport = this;
    native.receive_statistics = statistics.get();
    native.rtt_stats = this;
    module = std::make_unique<webrtc::ModuleRtpRtcpImpl2>(env, native);
    module->SetRemoteSSRC(config.ssrc);
    module->SetRTCPStatus(webrtc::RtcpMode::kCompound);
    if (!config.audio) {
      nack_processor = std::make_unique<webrtc::NackPeriodicProcessor>();
      nack = std::make_unique<webrtc::NackRequester>(webrtc::TaskQueueBase::Current(),
                                                     nack_processor.get(), &env.clock(), this, this,
                                                     env.field_trials());
    }
    if (config.rtx_payload_type >= 0)
      rtx = std::make_unique<webrtc::RtxReceiveStream>(
          this, std::map<int, int>{{config.rtx_payload_type, config.payload_type}}, config.ssrc);
  }

  void OnRtpPacket(const webrtc::RtpPacketReceived& packet) override {
    if ((packet.PayloadType() != config.payload_type &&
         packet.PayloadType() != config.red_payload_type) ||
        packet.Ssrc() != config.ssrc)
      return;
    auto received = packet;
    // BUNDLE's transport map can reflect the last configured m-line. Identify
    // per-source extensions using the native map from this negotiated receiver.
    received.IdentifyExtensions(webrtc::RtpHeaderExtensionMap(config.extensions));
    received.set_payload_type_frequency(config.clock_rate_hz);
    statistics->OnRtpPacket(received);
    if (nack)
      nack->OnReceivedPacket(received.SequenceNumber(), received.recovered());
    if (received.payload_size() || !config.audio)
      media(received);
  }

  bool SendRtp(webrtc::ArrayView<const uint8_t>, const webrtc::PacketOptions&) override {
    return false;
  }

  bool SendRtcp(webrtc::ArrayView<const uint8_t> packet) override {
    return rtcp(packet);
  }

  void SendNack(const std::vector<uint16_t>& seq, bool) override {
    module->SendNack(seq);
  }

  void RequestKeyFrame() override {
    auto now = env.clock().CurrentTime();
    if (config.audio || now - last_keyframe < webrtc::TimeDelta::Millis(200))
      return;
    last_keyframe = now;
    module->SendPictureLossIndication();
  }

  void OnRttUpdate(int64_t rtt) override {
    rtt_ms = rtt;
    if (nack)
      nack->UpdateRtt(rtt);
  }

  int64_t LastProcessedRtt() const override {
    return rtt_ms;
  }
};

NativeRtpReceiver::NativeRtpReceiver(const webrtc::Environment& env, NativeReceiveConfig config,
                                     Media media, Rtcp rtcp)
    : impl_(std::make_unique<Impl>(env, config, std::move(media), std::move(rtcp))) {}

NativeRtpReceiver::~NativeRtpReceiver() = default;

void NativeRtpReceiver::receive(const webrtc::RtpPacketReceived& packet) {
  if (impl_->rtx && packet.PayloadType() == impl_->config.rtx_payload_type)
    impl_->rtx->OnRtpPacket(packet);
  else
    impl_->OnRtpPacket(packet);
}

void NativeRtpReceiver::feedback(webrtc::ArrayView<const uint8_t> packet) {
  impl_->module->IncomingRtcpPacket(packet);
  impl_->last_rtcp_ms = impl_->env.clock().CurrentTime().ms();
}

void NativeRtpReceiver::request_keyframe() {
  impl_->RequestKeyFrame();
}

MediaQualitySample NativeRtpReceiver::quality() const {
  MediaQualitySample result;
  result.rtt_ms = impl_->rtt_ms;
  result.last_rtcp_ms = impl_->last_rtcp_ms;
  if (auto* statistic = impl_->statistics->GetStatistician(impl_->config.ssrc)) {
    auto stats = statistic->GetStats();
    result.packets = stats.packet_counter.packets;
    result.bytes = stats.packet_counter.payload_bytes;
    result.lost = stats.packets_lost;
    result.jitter_ms = stats.interarrival_jitter.us() / 1000.0;
  }
  return result;
}
}  // namespace lk
