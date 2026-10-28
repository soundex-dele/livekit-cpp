#pragma once
#include <api/array_view.h>
#include <api/video/video_codec_type.h>
#include <vector>
#include <optional>
#include <algorithm>

namespace lk {
// Only the clear RTP payload descriptor is changed. Bytes after it (including
// E2EE ciphertext, authentication tags and codec headers) stay byte-identical.
inline std::optional<std::vector<uint8_t>> rewrite_video_descriptor(
    webrtc::ArrayView<const uint8_t> input,webrtc::VideoCodecType codec,
    int picture,int tl0,int spatial_cap) {
  std::vector<uint8_t> p(input.begin(),input.end());
  if(p.empty())return {};
  size_t pos=1;
  auto picture_id=[&]() {
    if(pos>=p.size())return false;
    bool wide=(p[pos]&0x80)!=0;
    if(wide && pos+1>=p.size())return false;
    if(picture>=0) {
      if(!wide)p.insert(p.begin()+pos+1,0);
      p[pos]=uint8_t(0x80|((picture>>8)&0x7f));p[pos+1]=uint8_t(picture);pos+=2;
    }else pos+=wide?2:1;
    return true;
  };
  if(codec==webrtc::kVideoCodecVP8) {
    if(!(p[0]&0x80))return p;
    if(pos>=p.size())return {};
    auto ext=p[pos++];
    if((ext&0x80) && !picture_id())return {};
    if(ext&0x40){if(pos>=p.size())return {};if(tl0>=0)p[pos]=uint8_t(tl0);++pos;}
    if(ext&0x30)++pos;
  }else if(codec==webrtc::kVideoCodecVP9) {
    const auto flags=p[0];
    if((flags&0x80) && !picture_id())return {};
    if(flags&0x20){
      if(pos>=p.size())return {};++pos;
      if(!(flags&0x10)){if(pos>=p.size())return {};if(tl0>=0)p[pos]=uint8_t(tl0);++pos;}
    }
    if((flags&0x50)==0x50){
      int count=0;bool more;
      do{if(pos>=p.size() || ++count>3)return {};more=p[pos++]&1;}while(more);
    }
    if(flags&2){
      if(pos>=p.size())return {};
      auto ss=pos++;int layers=(p[ss]>>5)+1;
      int keep=std::clamp(spatial_cap+1,1,layers);
      if(p[ss]&0x10){
        if(pos+4*layers>p.size())return {};
        p.erase(p.begin()+pos+4*keep,p.begin()+pos+4*layers);pos+=4*keep;
      }
      p[ss]=uint8_t((p[ss]&31)|((keep-1)<<5));
      if(p[ss]&8){
        if(pos>=p.size())return {};auto count=p[pos++];
        for(int i=0;i<count;++i){if(pos>=p.size())return {};auto refs=(p[pos++]>>2)&3;pos+=refs;if(pos>p.size())return {};}
      }
    }
  }else return p;
  if(pos>=p.size())return {};
  return p;
}
}
