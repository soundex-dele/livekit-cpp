#pragma once
#include <api/rtp_parameters.h>
#include <api/video_codecs/h264_profile_level_id.h>
#include <pc/session_description.h>
#include <algorithm>
#include <cctype>
#include <optional>

namespace lk {
inline std::string codec_name(std::string name) {
  std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});
  if (auto slash=name.find('/'); slash!=std::string::npos) name.erase(0,slash+1);
  return name;
}
inline void negotiated_codecs(webrtc::RtpParameters& parameters,const webrtc::MediaContentDescription& media) {
  // M137 receiver parameters can contain the shared BUNDLE channel's codec
  // union. A route must use this MID's accepted codecs and preference order.
  std::vector<webrtc::RtpCodecParameters> codecs;
  for (const auto& accepted:media.codecs()) {
    webrtc::RtpCodecParameters codec;
    auto found=std::find_if(parameters.codecs.begin(),parameters.codecs.end(),[&](const auto& c){return c.payload_type==accepted.id;});
    if (found!=parameters.codecs.end()) codec=*found;
    codec.payload_type=accepted.id;codec.name=accepted.name;codec.clock_rate=accepted.clockrate;
    codec.parameters=accepted.params;codec.kind=media.type();
    if (media.type()==webrtc::MediaType::AUDIO) codec.num_channels=int(accepted.channels);
    else codec.num_channels.reset();
    codecs.push_back(std::move(codec));
  }
  parameters.codecs=std::move(codecs);
}
inline bool forwarding_codec(const webrtc::RtpCodec& codec) {
  const auto name=codec_name(codec.name);
  if (name=="opus" || name=="vp8") return true;
  if (name=="pcmu" || name=="pcma") return codec.clock_rate==8000 && codec.num_channels.value_or(1)==1;
  // Non-interleaved H.264 supports native STAP-A/FU-A packetization. Mode 0
  // needs a separate MTU contract and is not advertised until validated.
  if (name=="h264") {
    const auto mode=codec.parameters.find("packetization-mode");
    return mode!=codec.parameters.end() && mode->second=="1";
  }
  return false;
}
inline std::optional<webrtc::RtpCodecParameters> primary_media_codec(const webrtc::RtpParameters& parameters,bool audio) {
  for (const auto& codec:parameters.codecs) {
    const auto name=codec_name(codec.name);
    if (forwarding_codec(codec) && (name=="opus" || name=="pcmu" || name=="pcma")==audio) return codec;
  }
  return {};
}
inline bool same_forwarding_codec(const webrtc::RtpCodec& a,const webrtc::RtpCodec& b) {
  if (codec_name(a.name)!=codec_name(b.name) || a.clock_rate!=b.clock_rate || a.num_channels!=b.num_channels) return false;
  if (codec_name(a.name)!="h264") return true;
  const auto mode=[](const auto& c) {auto it=c.parameters.find("packetization-mode");return it==c.parameters.end()?std::string("0"):it->second;};
  return mode(a)==mode(b) && webrtc::H264IsSameProfile(a.parameters,b.parameters);
}
}
