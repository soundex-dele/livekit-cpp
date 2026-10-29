#include "lk/native_rtp.hpp"
#include "lk/forwarding_frame.hpp"
#include "lk/audio_red.hpp"
#include "lk/audio_activity.hpp"
#include "lk/media_codec.hpp"
#include "lk/rtp_payload_rewrite.hpp"
#include <api/environment/environment_factory.h>
#include <modules/rtp_rtcp/source/rtp_dependency_descriptor_extension.h>
#include <modules/rtp_rtcp/source/rtp_generic_frame_descriptor_extension.h>
#include <modules/rtp_rtcp/source/rtp_header_extensions.h>
#include <rtc_base/thread.h>
#include <system_wrappers/include/clock.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

webrtc::RtpPacketReceived packet(uint16_t seq, uint32_t timestamp,
                                 const std::vector<uint8_t>& payload) {
  webrtc::RtpPacketReceived p;
  p.SetSsrc(11);
  p.SetPayloadType(96);
  p.SetSequenceNumber(seq);
  p.SetTimestamp(timestamp);
  p.SetMarker(true);
  std::memcpy(p.AllocatePayload(payload.size()), payload.data(), payload.size());
  return p;
}

webrtc::RtpPacketReceived received(const webrtc::RtpPacketToSend& p) {
  webrtc::RtpPacketReceived result(&p.extension_manager());
  check(result.Parse(p.data(), p.size()), "RTP roundtrip failed");
  return result;
}

void audio(const webrtc::Environment& env) {
  lk::NativeSendConfig c;
  c.audio = true;
  c.ssrc = 21;
  c.payload_type = 111;
  c.red_payload_type = 112;
  std::vector<webrtc::RtpPacketToSend> encoded;
  lk::NativeRtpSender sender(
      env, c,
      [&](const auto& p) {
        encoded.push_back(p);
        return true;
      },
      [] {});
  const std::vector<uint8_t> bytes{0xde, 0xad, 0xbe, 0xef};
  for (int i = 0; i < 3; ++i)
    sender.forward(packet(uint16_t(10 + i), uint32_t(1000 + 960 * i), bytes));
  check(encoded.size() == 3 && encoded.back().PayloadType() == 112, "Opus to RED not negotiated");
  auto parsed = lk::parse_audio_red(encoded.back().payload(), 111);
  check(parsed && parsed->type_offsets.size() == 3, "Two-packet RED history missing");
  check(std::equal(bytes.begin(), bytes.end(),
                   encoded.back().payload().begin() + parsed->primary_offset),
        "RED changed primary ciphertext");
  c.payload_type = 109;
  c.red_payload_type = -1;
  c.ssrc = 31;
  std::vector<webrtc::RtpPacketToSend> plain;
  lk::NativeRtpSender strip(
      env, c,
      [&](const auto& p) {
        plain.push_back(p);
        return true;
      },
      [] {});
  strip.forward(received(encoded.back()), 112, 111);
  check(plain.size() == 1 && plain[0].PayloadType() == 109 &&
            plain[0].payload_size() == bytes.size() &&
            std::equal(bytes.begin(), bytes.end(), plain[0].payload().begin()),
        "RED to Opus conversion corrupt");
  c.red_payload_type = 110;
  lk::NativeRtpSender remap(
      env, c,
      [&](const auto& p) {
        plain.push_back(p);
        return true;
      },
      [] {});
  remap.forward(received(encoded.back()), 112, 111);
  check(lk::parse_audio_red(plain.back().payload(), 109).has_value(),
        "RED inner payload types not remapped");
  auto encrypted = packet(17, 7000, bytes);
  remap.forward(encrypted, -1, 96, true);
  check(plain.back().PayloadType() == 109 && plain.back().payload_size() == bytes.size() &&
            std::equal(bytes.begin(), bytes.end(), plain.back().payload().begin()),
        "Encrypted Opus must not be wrapped in RED");
  encrypted.SetPayloadType(112);
  remap.forward(encrypted, 112, 111, true);
  check(plain.back().PayloadType() == 110 && plain.back().payload_size() == bytes.size() &&
            std::equal(bytes.begin(), bytes.end(), plain.back().payload().begin()),
        "Encrypted RED must be opaque");
  const auto before = plain.size();
  strip.forward(encrypted, 112, 111, true);
  check(plain.size() == before, "Encrypted RED cannot be converted to Opus");
  auto malformed = encoded.back().payload();
  std::vector<uint8_t> broken(malformed.begin(), malformed.end());
  broken[3] = 255;
  check(!lk::parse_audio_red(broken, 111), "Truncated RED block accepted");
  lk::NativeReceiveConfig rc;
  rc.ssrc = 21;
  rc.audio = true;
  rc.payload_type = 111;
  rc.red_payload_type = 112;
  rc.clock_rate_hz = 48000;
  int count = 0;
  lk::NativeRtpReceiver receiver(env, rc, [&](const auto&) { ++count; }, [](auto) { return true; });
  receiver.receive(received(encoded.back()));
  check(count == 1, "RED outer receive gate rejected packet");
}

void opaque(const webrtc::Environment& env) {
  for (auto codec : {webrtc::kVideoCodecAV1, webrtc::kVideoCodecH265, webrtc::kVideoCodecH264}) {
    lk::NativeSendConfig c;
    c.ssrc = 31;
    c.video_codec = codec;
    c.h264_packetization_mode = 0;
    c.extmap_allow_mixed = true;
    c.extensions = {{webrtc::RtpExtension::kDependencyDescriptorUri, 7}};
    std::vector<webrtc::RtpPacketToSend> output;
    lk::NativeRtpSender sender(
        env, c,
        [&](const auto& p) {
          output.push_back(p);
          return true;
        },
        [] {});
    int frames = 0;
    lk::NativeVideoAssembler assembler(
        [&](const auto& frame) {
          ++frames;
          check(sender.forward_frame(frame, 11, 0, 0), "Opaque DD frame rejected");
        },
        codec, true);
    webrtc::RtpHeaderExtensionMap map;
    map.SetExtmapAllowMixed(true);
    map.Register<webrtc::RtpDependencyDescriptorExtension>(7);
    webrtc::FrameDependencyStructure structure;
    structure.structure_id = 0;
    structure.num_decode_targets = 1;
    structure.num_chains = 0;
    webrtc::FrameDependencyTemplate key;
    key.spatial_id = 0;
    key.temporal_id = 0;
    key.decode_target_indications = {webrtc::DecodeTargetIndication::kSwitch};
    structure.templates = {key};
    structure.resolutions.emplace_back(320, 180);
    // Deliberately not a codec bitstream: the DD path must forward encrypted
    // content without parsing the AV1/H26x coded frame.
    const std::vector<uint8_t> cipher{0xff, 0, 0x83, 0x9c, 0xde, 0xad};
    std::vector<webrtc::RtpPacketReceived> input;
    for (int i = 0; i < 3; ++i) {
      auto p = packet(uint16_t(10 + i), 90000, {});
      p.SetMarker(i == 2);
      p.IdentifyExtensions(map);
      webrtc::DependencyDescriptor d;
      d.first_packet_in_frame = i == 0;
      d.last_packet_in_frame = i == 2;
      d.frame_number = 10;
      d.frame_dependencies = key;
      if (i == 0)
        d.attached_structure = std::make_unique<webrtc::FrameDependencyStructure>(structure);
      check(p.SetExtension<webrtc::RtpDependencyDescriptorExtension>(structure, d),
            "DD input serialization failed");
      std::memcpy(p.AllocatePayload(cipher.size()), cipher.data(), cipher.size());
      input.push_back(p);
    }
    assembler.receive(input[2]);
    assembler.receive(input[1]);
    check(frames == 0, "Missing DD structure escaped");
    assembler.receive(input[0]);
    check(frames == 1 && output.size() == 3, "Opaque DD assembly failed");
    for (size_t i = 0; i < output.size(); ++i) {
      check(output[i].payload_size() == cipher.size() &&
                std::equal(cipher.begin(), cipher.end(), output[i].payload().begin()),
            "Encrypted bytes changed");
      check(output[i].Marker() == (i == 2), "Opaque RTP marker misplaced");
      webrtc::DependencyDescriptor d;
      check(received(output[i]).GetExtension<webrtc::RtpDependencyDescriptorExtension>(&structure,
                                                                                       &d),
            "Rewritten DD does not parse");
      check(d.first_packet_in_frame == (i == 0) && d.last_packet_in_frame == (i == 2),
            "DD packet boundaries lost");
    }
  }
  // Header-only H26x forwarding also works when the endpoint omits DD.
  for (auto codec : {webrtc::kVideoCodecH264, webrtc::kVideoCodecH265}) {
    std::vector<uint8_t> payload = codec == webrtc::kVideoCodecH264
                                       ? std::vector<uint8_t>{0x65, 0x80, 0x42}
                                       : std::vector<uint8_t>{0x26, 1, 0x80, 0x42};
    int frames = 0;
    lk::NativeVideoAssembler assembler(
        [&](const auto& f) {
          ++frames;
          auto* opaque = dynamic_cast<const lk::ForwardingFrame*>(&f);
          check(opaque && opaque->packets.size() == 1, "H26x payload not preserved");
        },
        codec, true);
    assembler.receive(packet(100, 3000, payload));
    check(frames == 1, "H26x IDR without DD rejected");
  }
}

void descriptors() {
  const std::vector<uint8_t> vp9{0xac, 127, 0, 7, 0xde, 0xad, 0xbe, 0xef};
  auto result = lk::rewrite_video_descriptor(vp9, webrtc::kVideoCodecVP9, 128, 8, 0);
  check(result && (*result)[1] == 0x80 && (*result)[2] == 128 && (*result)[4] == 8,
        "7-bit VP9 ID was not promoted");
  check(std::equal(vp9.begin() + 4, vp9.end(), result->begin() + 5),
        "VP9 descriptor rewrite touched ciphertext");
  const std::vector<uint8_t> vp8{0x90, 0xe0, 127, 7, 0x20, 0xde, 0xad};
  result = lk::rewrite_video_descriptor(vp8, webrtc::kVideoCodecVP8, 128, 8, 0);
  check(result && (*result)[2] == 0x80 && (*result)[3] == 128 && (*result)[4] == 8 &&
            (*result)[5] == 0x20,
        "VP8 temporal metadata lost");
  check(std::equal(vp8.begin() + 5, vp8.end(), result->begin() + 6),
        "VP8 descriptor rewrite touched ciphertext");
  const std::vector<uint8_t> broken{0x80};
  check(!lk::rewrite_video_descriptor(broken, webrtc::kVideoCodecVP9, 1, 1, 0),
        "Truncated descriptor accepted");
  for (auto name : {"VP9", "AV1", "H264", "H265"}) {
    webrtc::RtpCodecParameters c;
    c.name = name;
    c.clock_rate = 90000;
    check(lk::forwarding_codec(c), "Default codec rejected");
  }
}

void events() {
  lk::AudioActivity activity;
  activity.observe(20, 0, 48000, 1000);
  activity.observe(20, 960, 48000, 1020);
  check(activity.level(1020) == 0, "Short transient incorrectly activated speaker");
  for (int i = 2; i <= 20; ++i)
    activity.observe(20, i * 960, 48000, 1000 + i * 20);
  check(activity.level(1400) > 0.06, "Active speaker window did not accumulate");
  check(activity.level(2200) == 0, "Stale speaker not cleared");
  lk::MediaQuality scorer;
  lk::MediaQualitySample s;
  s.packets = 100;
  s.last_rtcp_ms = 1000;
  scorer.update(1000, s, 10, false, false);
  s.packets = 200;
  s.lost = 10;
  s.last_rtcp_ms = 6000;
  scorer.update(6000, s, 10, false, false);
  check(scorer.score() == 30, "Loss score lower bound differs");
  s.packets = 300;
  s.last_rtcp_ms = 11000;
  scorer.update(11000, s, 10, false, false);
  check(scorer.score() == 58, "Recovery smoothing differs");
  scorer.update(16000, s, 10, true, false);
  check(scorer.score() == 100, "Muted track penalized quality");
  check(lk::MediaQuality::mos(100) == 4.5f && lk::MediaQuality::mos(0) == 1, "MOS range differs");
  lk::MediaQuality baseline;
  s.packets = 100;
  s.lost = 0;
  s.rtt_ms = 1000;
  s.jitter_ms = 200;
  baseline.update(1000, s, 10, false, false);
  s.packets = 200;
  baseline.update(6000, s, 10, false, false);
  check(baseline.score() == 100, "Default Go production scoring must exclude RTT and jitter");
}
}  // namespace

int main() {
  auto thread = webrtc::Thread::Create();
  thread->Start();
  std::string error;
  thread->BlockingCall([&] {
    try {
      webrtc::SimulatedClock clock(1000000);
      auto env = webrtc::CreateEnvironment(&clock);
      audio(env);
      opaque(env);
      descriptors();
      events();
    } catch (const std::exception& e) {
      error = e.what();
    }
  });
  thread->Stop();
  if (!error.empty()) {
    std::cerr << error << '\n';
    return 1;
  }
  std::cout << "PASS RED, opaque AV1/H264/H265, DD, encrypted payload preservation, VP8/VP9 "
               "descriptors and media events\n";
}
