#include "lk/native_rtp.hpp"
#include "lk/media_codec.hpp"
#include <api/environment/environment_factory.h>
#include <modules/rtp_rtcp/include/rtp_header_extension_map.h>
#include <modules/rtp_rtcp/source/rtp_header_extensions.h>
#include <modules/rtp_rtcp/source/rtcp_packet/common_header.h>
#include <modules/rtp_rtcp/source/rtcp_packet/nack.h>
#include <modules/rtp_rtcp/source/rtcp_packet/pli.h>
#include <modules/rtp_rtcp/source/rtcp_packet/transport_feedback.h>
#include <modules/rtp_rtcp/source/rtp_format_vp9.h>
#include <modules/rtp_rtcp/source/video_rtp_depacketizer_vp9.h>
#include <rtc_base/thread.h>
#include <system_wrappers/include/clock.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using Bytes = webrtc::ArrayView<const uint8_t>;

void check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

webrtc::RtpPacketReceived packet(uint16_t seq, uint32_t ssrc = 11, bool extensions = false) {
  webrtc::RtpPacketReceived p;
  p.SetSsrc(ssrc);
  p.SetPayloadType(96);
  p.SetSequenceNumber(seq);
  p.SetTimestamp(90000);
  p.SetMarker(true);
  if (extensions) {
    webrtc::RtpHeaderExtensionMap map;
    map.Register<webrtc::VideoOrientation>(2);
    map.Register<webrtc::TransportSequenceNumber>(3);
    p.IdentifyExtensions(map);
    check(p.SetExtension<webrtc::VideoOrientation>(webrtc::kVideoRotation_90),
          "Rotation input failed");
    check(p.SetExtension<webrtc::TransportSequenceNumber>(100), "Transport sequence input failed");
  }
  const uint8_t payload[] = {0x10, 0, 1, 2, 3, 4, 5, 6};
  std::memcpy(p.AllocatePayload(sizeof(payload)), payload, sizeof(payload));
  p.set_arrival_time(webrtc::Timestamp::Millis(1000));
  return p;
}

Bytes bytes(const webrtc::Buffer& buffer) {
  return {buffer.data(), buffer.size()};
}

webrtc::RtpPacketReceived received(const webrtc::RtpPacketToSend& sent) {
  webrtc::RtpPacketReceived p(&sent.extension_manager());
  check(p.Parse(sent.data(), sent.size()), "Native RTP parse failed");
  p.set_arrival_time(webrtc::Timestamp::Millis(1000));
  return p;
}

webrtc::EncodedFrame encoded(int64_t id, uint32_t timestamp, bool key = true, size_t size = 4200) {
  // Synthetic encoded bytes with a valid VP8 uncompressed header. Native RTP
  // machinery must preserve these bytes; actual decoding is covered in browsers.
  std::vector<uint8_t> data(size, 0x55);
  const uint8_t header[] = {0x10, 0, 0, 0x9d, 0x01, 0x2a, 0x40, 0x01, 0xf0, 0};
  std::copy(std::begin(header), std::end(header), data.begin());
  if (!key)
    data[0] |= 1;
  webrtc::EncodedFrame frame;
  frame.SetId(id);
  frame.SetRtpTimestamp(timestamp);
  frame.SetEncodedData(webrtc::EncodedImageBuffer::Create(data.data(), data.size()));
  frame.rotation_ = webrtc::kVideoRotation_90;
  frame.num_references = key ? 0 : 1;
  if (!key)
    frame.references[0] = id - 1;
  return frame;
}

void run_sender_reconfiguration() {
  webrtc::SimulatedClock clock(1000000);
  auto env = webrtc::CreateEnvironment(&clock);
  lk::NativeSendConfig config;
  config.ssrc = 111;
  config.rtx_ssrc = 112;
  config.payload_type = 100;
  config.rtx_payload_type = 101;
  std::vector<webrtc::RtpPacketToSend> output;
  lk::NativeRtpSender::Continuity previous;
  {
    lk::NativeRtpSender sender(
        env, config,
        [&](const auto& p) {
          output.push_back(p);
          return true;
        },
        [] {});
    check(sender.forward_frame(encoded(1, 90000), 11), "Initial reconfiguration frame rejected");
    previous = sender.continuity();
    check(previous.packets > 0 && previous.octets > 0,
          "Missing sender accounting before reconfiguration");
  }
  const auto sequence = output.back().SequenceNumber();
  const auto timestamp = output.back().Timestamp();
  output.clear();
  clock.AdvanceTimeMilliseconds(40);
  // A codec renegotiation replaces the sender while retaining the outgoing
  // SSRC. Its source timestamp and RTP payload type need not remain the same.
  config.payload_type = 102;
  lk::NativeRtpSender replacement(
      env, config,
      [&](const auto& p) {
        output.push_back(p);
        return true;
      },
      [] {});
  replacement.restore_continuity(previous);
  check(!replacement.forward_frame(encoded(2, 1000, false), 22),
        "Replacement accepted a delta before its first keyframe");
  check(replacement.forward_frame(encoded(3, 4000), 22), "Replacement keyframe rejected");
  check(output.front().SequenceNumber() == uint16_t(sequence + 1),
        "Reconfigured sender reset RTP sequence");
  check(int32_t(output.front().Timestamp() - timestamp) > 0,
        "Reconfigured sender regressed RTP timestamp");
  check(output.front().PayloadType() == 102, "Reconfigured sender retained obsolete payload type");
  auto resumed = replacement.continuity();
  check(resumed.packets == previous.packets + output.size() && resumed.octets > previous.octets,
        "Reconfigured sender reset RTCP accounting");
  check(resumed.rtx_sequence == previous.rtx_sequence, "Reconfigured sender reset RTX sequence");
  config.ssrc = 211;
  lk::NativeRtpSender unrelated(env, config, [](const auto&) { return true; }, [] {});
  unrelated.restore_continuity(previous);
  check(unrelated.continuity().packets == 0 && !unrelated.continuity().timestamp,
        "Continuity leaked to a different SSRC");
}

void run_frames() {
  webrtc::SimulatedClock clock(1000000);
  auto env = webrtc::CreateEnvironment(&clock);
  lk::NativeSendConfig config;
  config.ssrc = 111;
  config.rtx_ssrc = 112;
  config.payload_type = 100;
  config.rtx_payload_type = 101;
  config.extensions = {{webrtc::RtpExtension::kVideoRotationUri, 8}};
  std::vector<webrtc::RtpPacketToSend> upstream, downstream;
  int requests = 0;
  lk::NativeRtpSender publisher(
      env, config,
      [&](const auto& p) {
        upstream.push_back(p);
        return true;
      },
      [] {});
  auto output_config = config;
  output_config.ssrc = 211;
  output_config.rtx_ssrc = 212;
  lk::NativeRtpSender subscriber(
      env, output_config,
      [&](const auto& p) {
        downstream.push_back(p);
        return true;
      },
      [&] { ++requests; });
  std::vector<webrtc::EncodedFrame> frames;
  lk::NativeVideoAssembler assembler([&](const auto& frame) {
    frames.push_back(frame);
    check(subscriber.forward_frame(frame, 111), "Complete upstream frame rejected");
  });
  std::vector<webrtc::Buffer> feedback;
  lk::NativeReceiveConfig rc;
  rc.ssrc = 111;
  rc.payload_type = 100;
  rc.rtx_payload_type = 101;
  rc.extensions = config.extensions;
  lk::NativeRtpReceiver receiver(
      env, rc, [&](const auto& p) { assembler.receive(p); },
      [&](Bytes p) {
        feedback.emplace_back(p.data(), p.size());
        return true;
      });
  auto key = encoded(1, 90000);
  check(publisher.forward_frame(key, 11), "Initial keyframe rejected");
  check(upstream.size() > 2, "Large VP8 frame was not natively fragmented");
  auto count = upstream.size();
  receiver.receive(received(upstream.front()));
  for (size_t i = 2; i < count; ++i)
    receiver.receive(received(upstream[i]));
  check(frames.empty() && downstream.empty(), "Incomplete VP8 frame escaped assembly");
  clock.AdvanceTimeMilliseconds(200);
  for (const auto& bytes : feedback)
    publisher.feedback({bytes.data(), bytes.size()});
  check(upstream.size() > count && upstream.back().Ssrc() == 112,
        "Lost frame fragment was not recovered with native RTX");
  receiver.receive(received(upstream.back()));
  check(frames.size() == 1 && frames[0].is_keyframe(), "Native frame assembly after RTX failed");
  check(frames[0].size() == key.size() &&
            std::equal(key.data(), key.data() + key.size(), frames[0].data()),
        "Native assembly changed VP8 bitstream");
  check(frames[0]._encodedWidth == 320 && frames[0]._encodedHeight == 240,
        "Native VP8 dimensions lost");
  check(frames[0].rotation() == webrtc::kVideoRotation_90,
        "Orientation lost across frame assembly");
  receiver.receive(received(upstream.back()));
  check(frames.size() == 1, "Duplicate RTX produced another frame");
  auto delta = encoded(2, 93000, false);
  count = upstream.size();
  check(publisher.forward_frame(delta, 11), "Dependent upstream frame rejected");
  // Last packet first: metadata and native assembly must tolerate reordering.
  for (size_t i = upstream.size(); i > count; --i)
    receiver.receive(received(upstream[i - 1]));
  check(frames.size() == 2 && !frames[1].is_keyframe(),
        "Native reference finder lost delta dependency");

  auto before = downstream.size();
  auto next = encoded(3, 96000, false);
  check(!subscriber.forward_frame(next, 222) && requests == 1,
        "Source switch accepted a delta frame");
  // A rejected target must not corrupt the current source's dependency state.
  next.SetId(frames.back().Id() + 1);
  next.references[0] = frames.back().Id();
  check(subscriber.forward_frame(next, 111), "Rejected switch damaged active source");
  auto previous_timestamp = downstream.back().Timestamp();
  auto missing = encoded(next.Id() + 2, 99000, false);
  check(!subscriber.forward_frame(missing, 111) && requests == 2,
        "Missing dependency did not request a keyframe");
  clock.AdvanceTimeMilliseconds(33);
  auto new_key = encoded(10, 3000);
  check(subscriber.forward_frame(new_key, 222), "Target keyframe rejected");
  auto switched_timestamp = downstream.back().Timestamp();
  check(switched_timestamp > previous_timestamp, "Source switch regressed output RTP clock");
  webrtc::rtcp::SenderReport sr;
  sr.SetSenderSsrc(222);
  sr.SetNtp(webrtc::NtpTime(12, 34));
  sr.SetRtpTimestamp(3000);
  auto translated = subscriber.translate_report(sr);
  check(translated.rtp_timestamp() == switched_timestamp && translated.ntp() == sr.ntp(),
        "Switched SR clock does not match RTP mapping");
  auto new_delta = encoded(11, 6000, false);
  check(subscriber.forward_frame(new_delta, 222), "New layer delta rejected");
  check(downstream.back().Timestamp() == switched_timestamp + 3000, "New layer clock rate changed");
  check(!subscriber.forward_frame(new_delta, 222), "Duplicate frame forwarded twice");
  subscriber.set_enabled(false);
  check(!subscriber.forward_frame(encoded(12, 9000, false), 222), "Disabled frame forwarded");
  subscriber.set_enabled(true);
  check(!subscriber.forward_frame(encoded(12, 9000, false), 222),
        "Resume did not require keyframe");
  check(subscriber.forward_frame(encoded(13, 12000), 222), "Resume keyframe rejected");
  subscriber.set_media_paused(true);
  check(!subscriber.forward_frame(encoded(14, 15000), 222), "Budget-paused video admitted a frame");
  subscriber.set_media_paused(false);
  check(!subscriber.forward_frame(encoded(14, 15000, false), 222),
        "Budget resume must require a fresh keyframe");
  check(subscriber.forward_frame(encoded(15, 18000), 222), "Budget resume rejected fresh keyframe");
  check(downstream.size() > before, "Frame switch produced no packets");
  for (size_t i = 1; i < downstream.size(); ++i)
    check(downstream[i].SequenceNumber() == uint16_t(downstream[i - 1].SequenceNumber() + 1),
          "Frame switch/mute introduced RTP sequence gaps");
  std::vector<webrtc::EncodedFrame> decoded_headers;
  lk::NativeVideoAssembler roundtrip([&](const auto& frame) { decoded_headers.push_back(frame); });
  for (const auto& p : downstream)
    roundtrip.receive(received(p));
  check(decoded_headers.size() == 7,
        "Repacketized frame picture IDs or dependencies are discontinuous");
  for (const auto& frame : decoded_headers) {
    check(frame.size() == key.size(), "Repacketized encoded size changed");
    check(std::equal(key.data() + 1, key.data() + key.size(), frame.data() + 1),
          "Repacketized VP8 bytes changed");
    check(frame.rotation() == key.rotation(), "Repacketization lost orientation");
  }
  // Verify native RTP timestamp wrap handling on a separate subscriber.
  lk::NativeRtpSender wrap(env, output_config, [](const auto&) { return true; }, [] {});
  check(wrap.forward_frame(encoded(1, 0xfffff000), 11) &&
            wrap.forward_frame(encoded(2, 0x1000, false), 11),
        "RTP timestamp wrap rejected");

  auto bounded = std::make_shared<lk::NativeSendTransport>(env, 6000);
  std::vector<webrtc::RtpPacketToSend> admitted;
  int overflow_plis = 0;
  lk::NativeRtpSender congested(
      env, output_config,
      [&](const auto& p) {
        if (p.packet_type() == webrtc::RtpPacketMediaType::kVideo)
          admitted.push_back(p);
        return true;
      },
      [&] { ++overflow_plis; }, bounded);
  check(congested.forward_frame(encoded(1, 90000), 11), "First queued frame rejected");
  auto queued_bytes = bounded->stats().queue_bytes;
  check(!congested.forward_frame(encoded(2, 93000, false), 11), "Oversized frame batch admitted");
  check(bounded->stats().queue_bytes == queued_bytes && bounded->stats().queue_dropped > 1,
        "Queue overflow admitted only part of a video frame");
  check(!congested.forward_frame(encoded(3, 96000, false, 1000), 11) && overflow_plis == 2,
        "Congestion drop did not gate dependent frames and request recovery");
  auto drain = [&] {
    for (int i = 0; i < 500; ++i) {
      bounded->process();
      clock.AdvanceTimeMilliseconds(1);
    }
  };
  bounded->set_available(true);
  drain();
  check(bounded->stats().queue_bytes == 0 && !admitted.empty(),
        "Native pacer did not drain accepted frame");
  check(congested.forward_frame(encoded(4, 99000, true, 1000), 11),
        "Recovery keyframe not admitted");
  drain();
  for (size_t i = 1; i < admitted.size(); ++i)
    check(admitted[i].SequenceNumber() == uint16_t(admitted[i - 1].SequenceNumber() + 1),
          "Rejected frame consumed RTP sequence numbers");
  int recovered_frames = 0;
  lk::NativeVideoAssembler after_overflow([&](const auto& frame) {
    check(frame.is_keyframe(), "Partial frame survived overflow");
    ++recovered_frames;
  });
  for (const auto& p : admitted)
    after_overflow.receive(received(p));
  check(recovered_frames == 2, "Overflow recovery did not produce two complete keyframes");
  auto tight = std::make_shared<lk::NativeSendTransport>(env, 2000);
  lk::NativeRtpSender tight_sender(
      env, output_config, [](const auto&) { return true; }, [] {}, tight);
  check(!tight_sender.forward_frame(encoded(1, 90000, true, 1900), 11) &&
            tight->stats().queue_bytes == 0,
        "Frame admission ignored native transport overhead at queue boundary");
}

void run_audio_codecs() {
  webrtc::SimulatedClock clock(1000000);
  auto env = webrtc::CreateEnvironment(&clock);
  for (auto [name, pt] : {std::pair{"PCMU", 0}, std::pair{"PCMA", 8}, std::pair{"opus", 111}}) {
    const int rate = pt == 111 ? 48000 : 8000;
    webrtc::RtpCodecParameters codec;
    codec.name = name;
    codec.payload_type = pt;
    codec.clock_rate = rate;
    codec.num_channels = pt == 111 ? 2 : 1;
    webrtc::RtpParameters params;
    params.codecs.push_back(codec);
    check(lk::primary_media_codec(params, true).has_value(), "Supported audio codec was excluded");
    check(!lk::primary_media_codec(params, false), "Audio codec was classified as video");
    if (pt != 111) {
      auto invalid = codec;
      invalid.clock_rate = 48000;
      check(!lk::forwarding_codec(invalid), "G711 accepted a nonstandard RTP clock");
      invalid = codec;
      invalid.num_channels = 2;
      check(!lk::forwarding_codec(invalid), "G711 accepted unsupported stereo RTP");
    }
    lk::NativeSendConfig send;
    send.audio = true;
    send.ssrc = 22;
    send.payload_type = pt;
    std::vector<webrtc::RtpPacketToSend> packets;
    lk::NativeRtpSender sender(
        env, send,
        [&](const auto& p) {
          packets.push_back(p);
          return true;
        },
        [] {});
    lk::NativeReceiveConfig receive;
    receive.audio = true;
    receive.ssrc = 11;
    receive.payload_type = pt;
    receive.clock_rate_hz = rate;
    lk::NativeRtpReceiver receiver(
        env, receive,
        [&](const auto& p) {
          check(p.payload_type_frequency() == rate,
                "Native receive statistics used the wrong audio clock");
          sender.forward(p);
        },
        [](Bytes) { return true; });
    for (int i = 0; i < 3; ++i) {
      auto input = packet(uint16_t(65534 + i));
      input.SetPayloadType(pt);
      input.SetTimestamp(uint32_t(0xfffffff0u + i * (rate / 50)));
      receiver.receive(input);
      const auto& output = packets.back();
      check(output.packet_type() == webrtc::RtpPacketMediaType::kAudio && output.Ssrc() == 22 &&
                output.PayloadType() == pt,
            "Audio packet metadata mapping failed");
      check(output.Timestamp() == input.Timestamp(),
            "Forwarding altered the codec's RTP clock, including wrap");
      check(
          output.payload_size() == input.payload_size() &&
              std::equal(input.payload().begin(), input.payload().end(), output.payload().begin()),
          "Forwarding changed encoded audio bytes");
    }
    check(packets.size() == 3, "Audio forwarding lost a packet");
  }
}

void run_vp9_frames() {
  webrtc::SimulatedClock clock(1000000);
  auto env = webrtc::CreateEnvironment(&clock);
  webrtc::RtpCodecParameters codec;
  codec.name = "VP9";
  codec.clock_rate = 90000;
  check(lk::forwarding_codec(codec), "VP9 default profile zero not supported");
  auto explicit_zero = codec;
  explicit_zero.parameters["profile-id"] = "0";
  check(lk::same_forwarding_codec(codec, explicit_zero), "VP9 default/explicit profile mismatch");
  codec.parameters["profile-id"] = "2";
  check(lk::forwarding_codec(codec) && !lk::same_forwarding_codec(codec, explicit_zero),
        "VP9 profile matching ignored");
  lk::NativeSendConfig config;
  config.ssrc = 111;
  config.rtx_ssrc = 112;
  config.payload_type = 100;
  config.rtx_payload_type = 101;
  config.video_codec = webrtc::kVideoCodecVP9;
  std::vector<webrtc::RtpPacketToSend> upstream, high, low;
  lk::NativeRtpSender publisher(
      env, config,
      [&](const auto& p) {
        upstream.push_back(p);
        return true;
      },
      [] {});
  auto output = config;
  output.ssrc = 211;
  output.rtx_ssrc = 212;
  output.payload_type = 102;
  lk::NativeRtpSender subscriber(
      env, output,
      [&](const auto& p) {
        high.push_back(p);
        return true;
      },
      [] {});
  output.ssrc = 311;
  output.rtx_ssrc = 312;
  lk::NativeRtpSender base(
      env, output,
      [&](const auto& p) {
        low.push_back(p);
        return true;
      },
      [] {});
  std::vector<uint8_t> payload(4200,
                               0x55);  // Codec bytes are audited, not decoded by this native test.
  auto bounded = std::make_shared<lk::NativeSendTransport>(env, 6000);
  std::vector<webrtc::RtpPacketToSend> recovered;
  lk::NativeRtpSender congested(
      env, output,
      [&](const auto& p) {
        if (p.packet_type() == webrtc::RtpPacketMediaType::kVideo)
          recovered.push_back(p);
        return true;
      },
      [] {}, bounded);
  auto drain = [&] {
    bounded->set_available(true);
    for (int i = 0; i < 500; ++i) {
      bounded->process();
      clock.AdvanceTimeMilliseconds(1);
    }
  };
  int assembled = 0;
  lk::NativeVideoAssembler assembler(
      [&](const auto& frame) {
        ++assembled;
        if (assembled == 1) {
          check(congested.forward_frame(frame, 111), "VP9 queue rejected initial source");
          check(!congested.forward_frame(frame, 222), "VP9 source switch ignored queue overflow");
          drain();
        } else if (assembled == 2) {
          check(congested.forward_frame(frame, 111),
                "Rejected VP9 switch corrupted current dependencies");
          drain();
          webrtc::VideoRtpDepacketizerVp9 parser;
          for (const auto& p : recovered) {
            auto parsed = parser.Parse(received(p).PayloadBuffer());
            check(parsed.has_value(), "VP9 queued packet did not parse");
            const auto& h =
                std::get<webrtc::RTPVideoHeaderVP9>(parsed->video_header.video_type_header);
            check(h.picture_id == 32767, "Rejected VP9 switch changed active picture ID offset");
          }
        }
        check(frame.size() == payload.size() &&
                  std::equal(payload.begin(), payload.end(), frame.data()),
              "VP9 encoded bytes changed");
        check(subscriber.forward_frame(frame, 111),
              "VP9 same-picture spatial frame or temporal dependency rejected");
        const auto* header = lk::native_vp9_header(frame);
        check(header, "VP9 native RTP metadata lost");
        if (header->spatial_idx == 0)
          check(base.forward_frame(frame, 111, 0),
                "VP9 independent base-layer subscription rejected");
      },
      webrtc::kVideoCodecVP9);
  lk::NativeReceiveConfig receive;
  receive.ssrc = 111;
  receive.payload_type = 100;
  receive.rtx_payload_type = 101;
  receive.video_codec = webrtc::kVideoCodecVP9;
  lk::NativeRtpReceiver receiver(
      env, receive, [&](const auto& p) { assembler.receive(p); }, [](Bytes) { return true; });
  uint16_t seq = 65530;
  for (int picture = 0; picture < 2; ++picture)
    for (int spatial = 0; spatial < 3; ++spatial) {
      webrtc::RTPVideoHeaderVP9 h{};
      h.InitRTPVideoHeaderVP9();
      h.flexible_mode = true;
      h.picture_id = int16_t(32767 + picture) & 0x7fff;
      h.temporal_idx = 0;
      h.spatial_idx = uint8_t(spatial);
      h.inter_pic_predicted = picture != 0;
      h.inter_layer_predicted = spatial != 0;
      h.num_ref_pics = picture ? 1 : 0;
      h.pid_diff[0] = 1;
      h.num_spatial_layers = 3;
      h.end_of_picture = spatial == 2;
      h.ss_data_available = picture == 0 && spatial == 0;
      h.spatial_layer_resolution_present = true;
      for (int i = 0; i < 3; ++i) {
        h.width[i] = uint16_t(320 << i);
        h.height[i] = uint16_t(180 << i);
      }
      webrtc::RtpPacketizer::PayloadSizeLimits limits;
      limits.max_payload_len = 1000;
      webrtc::RtpPacketizerVp9 packetizer(payload, limits, h);
      const auto begin = upstream.size();
      while (packetizer.NumPackets()) {
        webrtc::RtpPacketToSend packet(nullptr);
        packet.SetSsrc(11);
        packet.SetPayloadType(100);
        packet.SetSequenceNumber(seq++);
        packet.SetTimestamp(90000 + 3000 * picture);
        check(packetizer.NextPacket(&packet), "Native VP9 fixture packetization failed");
        publisher.forward(received(packet));
      }
      const auto end = upstream.size();
      for (size_t i = begin; i < end; ++i)
        if (i != begin + 1)
          receiver.receive(received(upstream[i]));
      check(assembled == picture * 3 + spatial, "Incomplete VP9 spatial frame escaped");
      webrtc::rtcp::Nack nack;
      nack.SetSenderSsrc(999);
      nack.SetMediaSsrc(111);
      nack.SetPacketIds({upstream[begin + 1].SequenceNumber()});
      clock.AdvanceTimeMilliseconds(200);
      publisher.feedback(bytes(nack.Build()));
      check(upstream.size() == end + 1 && upstream.back().Ssrc() == 112,
            "VP9 RTX did not repair a missing fragment");
      receiver.receive(received(upstream.back()));
      check(assembled == picture * 3 + spatial + 1, "VP9 repaired frame was not forwarded");
    }
  auto verify = [&](const auto& packets, int expected, int spatial_layers) {
    int count = 0, markers = 0;
    lk::NativeVideoAssembler downstream(
        [&](const auto& frame) {
          ++count;
          check(frame.size() == payload.size() &&
                    std::equal(payload.begin(), payload.end(), frame.data()),
                "VP9 repacketization changed bitstream");
          const auto* h = lk::native_vp9_header(frame);
          if (h->ss_data_available)
            check(h->num_spatial_layers == spatial_layers,
                  "VP9 outgoing scalability structure not clipped");
        },
        webrtc::kVideoCodecVP9);
    for (size_t i = 0; i < packets.size(); ++i) {
      if (i)
        check(packets[i].SequenceNumber() == uint16_t(packets[i - 1].SequenceNumber() + 1),
              "VP9 spatial filtering introduced sequence gaps");
      if (packets[i].Marker())
        ++markers;
      downstream.receive(received(packets[i]));
    }
    check(count == expected && markers == 2,
          "VP9 reference/picture wrap or end-of-picture markers changed");
  };
  verify(high, 6, 3);
  verify(low, 2, 1);
}

void run_h264_frames() {
  webrtc::SimulatedClock clock(1000000);
  auto env = webrtc::CreateEnvironment(&clock);
  lk::NativeSendConfig config;
  config.ssrc = 111;
  config.rtx_ssrc = 112;
  config.payload_type = 100;
  config.rtx_payload_type = 101;
  config.video_codec = webrtc::kVideoCodecH264;
  std::vector<webrtc::RtpPacketToSend> upstream, downstream;
  lk::NativeRtpSender publisher(
      env, config,
      [&](const auto& p) {
        upstream.push_back(p);
        return true;
      },
      [] {});
  auto out = config;
  out.ssrc = 211;
  out.rtx_ssrc = 212;
  out.payload_type = 102;
  out.rtx_payload_type = 103;
  lk::NativeRtpSender subscriber(
      env, out,
      [&](const auto& p) {
        downstream.push_back(p);
        return true;
      },
      [] {});
  // Annex-B IDR payload: exercise native fragmentation and reassembly without
  // pretending these synthetic slice bytes are decodable camera media.
  // Real SPS/PPS syntax (320x240), followed by synthetic slice content. The
  // parameter sets and IDR must survive STAP-A/FU-A byte for byte.
  std::vector<uint8_t> payload{0,    0,    0,    1,    0x67, 0x7a, 0x00, 0x0d, 0xbc, 0xd9, 0x41,
                               0x41, 0xfa, 0x10, 0,    0,    3,    0,    0x10, 0,    0,    3,
                               3,    0xc0, 0xf1, 0x42, 0x99, 0x60, 0,    0,    0,    1,    0x68,
                               0xeb, 0xe3, 0xcb, 0x22, 0xc0, 0,    0,    0,    1,    0x65, 0xff};
  payload.resize(payload.size() + 4200, 0x55);
  auto frame = encoded(1, 90000);
  frame.SetEncodedData(webrtc::EncodedImageBuffer::Create(payload.data(), payload.size()));
  check(publisher.forward_frame(frame, 11), "H264 keyframe was rejected");
  check(upstream.size() > 3, "H264 large IDR was not fragmented into native FU-A packets");
  int assembled = 0, decoded_payloads = 0;
  lk::NativeVideoAssembler input(
      [&](const auto& f) {
        ++assembled;
        check(f.is_keyframe(), "Native H264 IDR classification was lost");
        check(f.size() == payload.size() && std::equal(payload.begin(), payload.end(), f.data()),
              "H264 upstream assembly changed Annex-B bytes");
        check(subscriber.forward_frame(f, 111), "H264 subscriber refused complete recovered frame");
      },
      webrtc::kVideoCodecH264);
  lk::NativeReceiveConfig receive;
  receive.ssrc = 111;
  receive.payload_type = 100;
  receive.rtx_payload_type = 101;
  receive.video_codec = webrtc::kVideoCodecH264;
  lk::NativeRtpReceiver receiver(
      env, receive, [&](const auto& p) { input.receive(p); }, [](Bytes) { return true; });
  for (size_t i = 0; i < upstream.size(); ++i)
    if (i != 1)
      receiver.receive(received(upstream[i]));
  check(assembled == 0 && downstream.empty(),
        "Incomplete H264 frame was forwarded before loss recovery");
  webrtc::rtcp::Nack nack;
  nack.SetSenderSsrc(999);
  nack.SetMediaSsrc(111);
  nack.SetPacketIds({upstream[1].SequenceNumber()});
  clock.AdvanceTimeMilliseconds(200);
  publisher.feedback(bytes(nack.Build()));
  check(upstream.back().Ssrc() == 112, "H264 NACK did not produce RTX");
  receiver.receive(received(upstream.back()));
  check(assembled == 1 && !downstream.empty(), "Recovered H264 frame did not traverse SFU");
  lk::NativeVideoAssembler output(
      [&](const auto& f) {
        ++decoded_payloads;
        check(f.size() == payload.size() && std::equal(payload.begin(), payload.end(), f.data()),
              "H264 downstream repacketization changed encoded content");
      },
      webrtc::kVideoCodecH264);
  for (const auto& p : downstream) {
    check(p.PayloadType() == 102 && p.Ssrc() == 211, "H264 downstream payload/SSRC mapping failed");
    output.receive(received(p));
  }
  check(decoded_payloads == 1, "H264 downstream assembler did not emit one complete frame");
}

template <class F>
void blocks(const webrtc::Buffer& buffer, F callback) {
  auto* cursor = buffer.data();
  const auto* end = cursor + buffer.size();
  while (cursor < end) {
    webrtc::rtcp::CommonHeader header;
    check(header.Parse(cursor, size_t(end - cursor)), "Native RTCP parse failed");
    callback(header);
    cursor = header.NextPacket();
  }
}

void run() {
  webrtc::SimulatedClock clock(1000000);
  auto env = webrtc::CreateEnvironment(&clock);
  lk::NativeSendConfig config;
  config.ssrc = 111;
  config.rtx_ssrc = 222;
  config.payload_type = 100;
  config.rtx_payload_type = 101;
  config.mid = "video";
  config.extensions = {{webrtc::RtpExtension::kMidUri, 9},
                       {webrtc::RtpExtension::kVideoRotationUri, 8}};
  std::vector<webrtc::RtpPacketToSend> sent;
  int plis = 0;
  lk::NativeRtpSender sender(
      env, config,
      [&](const auto& p) {
        sent.push_back(p);
        return true;
      },
      [&] { ++plis; });
  auto first = packet(65534, 11, true);
  webrtc::RtpHeaderExtensionMap extension_map;
  extension_map.Register<webrtc::VideoOrientation>(2);
  extension_map.Register<webrtc::TransportSequenceNumber>(3);
  sender.forward(first);
  sender.forward(packet(0));
  check(sent.size() == 2, "Two native packets expected");
  const uint16_t base = sent[0].SequenceNumber();
  check(sent[1].SequenceNumber() == uint16_t(base + 2), "Publisher gap/wrap must be preserved");
  check(sent[0].Ssrc() == 111 && sent[0].PayloadType() == 100, "Subscriber SSRC/PT mapping failed");
  check(sent[0].Timestamp() == first.Timestamp() && sent[0].Marker(),
        "Timestamp/marker must survive forwarding");
  check(sent[0].payload_size() == first.payload_size() &&
            std::equal(first.payload().begin(), first.payload().end(), sent[0].payload().begin()),
        "Encoded payload changed");
  std::string mid;
  check(sent[0].GetExtension<webrtc::RtpMid>(&mid) && mid == "video", "Subscriber MID missing");
  webrtc::VideoRotation rotation;
  check(sent[0].GetExtension<webrtc::VideoOrientation>(&rotation) &&
            rotation == webrtc::kVideoRotation_90,
        "Native extension ID remapping failed");
  check(!sent[0].HasExtension<webrtc::TransportSequenceNumber>(),
        "Publisher congestion sequence leaked downstream");
  sender.forward(packet(65535));
  check(sent.back().SequenceNumber() == uint16_t(base + 1), "Reordered packet mapped incorrectly");

  webrtc::rtcp::Nack nack;
  nack.SetSenderSsrc(333);
  nack.SetMediaSsrc(111);
  nack.SetPacketIds({base});
  clock.AdvanceTimeMilliseconds(200);
  sender.feedback(bytes(nack.Build()));
  check(sent.size() == 4, "Native NACK did not retransmit cached packet");
  check(sent.back().Ssrc() == 222 && sent.back().PayloadType() == 101,
        "Native RTX SSRC/PT missing");
  check(sent.back().packet_type() == webrtc::RtpPacketMediaType::kRetransmission,
        "Native retransmission classification missing");
  std::vector<webrtc::RtpPacketReceived> recovered;
  lk::NativeReceiveConfig recv;
  recv.ssrc = 111;
  recv.payload_type = 100;
  recv.rtx_payload_type = 101;
  {
    lk::NativeRtpReceiver receiver(
        env, recv, [&](const auto& p) { recovered.push_back(p); }, [](Bytes) { return true; });
    receiver.receive(received(sent.back()));
    check(recovered.size() == 1 && recovered[0].SequenceNumber() == base &&
              recovered[0].Ssrc() == 111,
          "Native RTX decapsulation failed");
    check(recovered[0].payload_size() == first.payload_size() &&
              std::equal(first.payload().begin(), first.payload().end(),
                         recovered[0].payload().begin()),
          "RTX payload changed");
  }
  webrtc::rtcp::Pli pli;
  pli.SetSenderSsrc(333);
  pli.SetMediaSsrc(999);
  sender.feedback(bytes(pli.Build()));
  check(plis == 0, "PLI for another SSRC leaked into route");
  pli.SetMediaSsrc(111);
  sender.feedback(bytes(pli.Build()));
  check(plis == 1, "Native PLI callback missing");
  nack.SetMediaSsrc(999);
  clock.AdvanceTimeMilliseconds(200);
  sender.feedback(bytes(nack.Build()));
  check(sent.size() == 4, "NACK for another SSRC leaked into cache");
  sender.set_enabled(false);
  sender.forward(packet(1));
  pli.SetMediaSsrc(111);
  sender.feedback(bytes(pli.Build()));
  check(sent.size() == 4 && plis == 1, "Disabled subscription emitted media/feedback");
  sender.set_enabled(true);
  nack.SetMediaSsrc(111);
  sender.feedback(bytes(nack.Build()));
  check(sent.size() == 4, "Mute must clear native retransmission cache");
  sender.forward(packet(100));
  check(sent.back().SequenceNumber() == uint16_t(base + 3), "Resume sequence is not continuous");

  webrtc::rtcp::SenderReport sr;
  sr.SetSenderSsrc(11);
  sr.SetNtp(webrtc::NtpTime(123, 456));
  sr.SetRtpTimestamp(90000);
  auto mapped = sender.translate_report(sr);
  check(mapped.sender_ssrc() == 111 && mapped.ntp() == sr.ntp() && mapped.rtp_timestamp() == 90000,
        "Native SR clock mapping changed");
  check(
      mapped.sender_packet_count() == 4 && mapped.sender_octet_count() == 4 * first.payload_size(),
      "SR subscriber counters incorrect");

  // Simulate a dropped upstream packet AFTER native sender transmission. The
  // receive-side native gap detector must request it; native RTX restores it.
  std::vector<webrtc::RtpPacketToSend> upstream;
  std::vector<webrtc::Buffer> feedback;
  std::vector<uint16_t> delivered;
  lk::NativeRtpSender publisher(
      env, config,
      [&](const auto& p) {
        upstream.push_back(p);
        return true;
      },
      [] {});
  publisher.forward(packet(10));
  publisher.forward(packet(11));
  publisher.forward(packet(12));
  {
    lk::NativeRtpReceiver receiver(
        env, recv, [&](const auto& p) { delivered.push_back(p.SequenceNumber()); },
        [&](Bytes p) {
          feedback.emplace_back(p.data(), p.size());
          return true;
        });
    receiver.receive(received(upstream[0]));
    receiver.receive(received(upstream[2]));
    bool requested = false;
    for (const auto& rtcp : feedback)
      blocks(rtcp, [&](const auto& header) {
        if (header.type() == webrtc::rtcp::Nack::kPacketType &&
            header.fmt() == webrtc::rtcp::Nack::kFeedbackMessageType) {
          webrtc::rtcp::Nack missing;
          check(missing.Parse(header), "Native NACK parse failed");
          requested |= std::find(missing.packet_ids().begin(), missing.packet_ids().end(),
                                 upstream[1].SequenceNumber()) != missing.packet_ids().end();
        }
      });
    check(requested, "Native gap detector did not request missing packet");
    clock.AdvanceTimeMilliseconds(200);
    for (const auto& rtcp : feedback)
      publisher.feedback(bytes(rtcp));
    check(upstream.size() == 4 && upstream.back().Ssrc() == 222,
          "Upstream native retransmission missing");
    receiver.receive(received(upstream.back()));
    check(delivered.size() == 3 && delivered.back() == upstream[1].SequenceNumber(),
          "Missing packet not recovered through native modules");
    feedback.clear();
    receiver.request_keyframe();
    receiver.request_keyframe();
    int upstream_plis = 0;
    for (const auto& rtcp : feedback)
      blocks(rtcp, [&](const auto& header) {
        if (header.type() == webrtc::rtcp::Pli::kPacketType &&
            header.fmt() == webrtc::rtcp::Pli::kFeedbackMessageType)
          ++upstream_plis;
      });
    check(upstream_plis == 1, "Native upstream PLI or routing throttle failed");
  }  // Teardown cancels native periodic feedback tasks on their owning queue.
  feedback.clear();
  {
    lk::NativeReceiveTransport transport(env, [&](Bytes p) {
      feedback.emplace_back(p.data(), p.size());
      return true;
    });
    for (uint16_t seq : {100, 101, 103}) {
      auto p = packet(seq, seq == 101 ? 22 : 11, true);
      p.IdentifyExtensions(extension_map);
      p.SetExtension<webrtc::TransportSequenceNumber>(seq);
      p.set_arrival_time(clock.CurrentTime());
      transport.receive(p, seq == 101);
      clock.AdvanceTimeMilliseconds(5);
    }
    clock.AdvanceTimeMilliseconds(300);
    transport.process();
    bool reported = false;
    for (const auto& rtcp : feedback)
      blocks(rtcp, [&](const auto& header) {
        if (header.type() == webrtc::rtcp::TransportFeedback::kPacketType &&
            header.fmt() == webrtc::rtcp::TransportFeedback::kFeedbackMessageType) {
          webrtc::rtcp::TransportFeedback report;
          check(report.Parse(header), "Native transport feedback parse failed");
          reported |= report.GetBaseSequence() == 100 && report.GetPacketStatusCount() == 4 &&
                      report.GetReceivedPackets().size() == 3;
        }
      });
    check(reported, "Transport feedback must share audio/video sequence space and report loss");
  }
}
}  // namespace

int main() {
  auto thread = webrtc::Thread::Create();
  thread->SetName("native-rtp-test", nullptr);
  if (!thread->Start())
    return 1;
  std::string error;
  thread->BlockingCall([&] {
    try {
      run();
      run_frames();
      run_sender_reconfiguration();
      run_h264_frames();
      run_vp9_frames();
      run_audio_codecs();
    } catch (const std::exception& e) {
      error = e.what();
    }
  });
  thread->Stop();
  if (!error.empty()) {
    std::cerr << error << '\n';
    return 1;
  }
  std::cout
      << "PASS native RTP/RTCP, loss recovery, encoded VP8/H264/VP9 assembly/repacketization, "
         "frame switching and PCMU/PCMA/Opus clocks/payload preservation\n";
}
