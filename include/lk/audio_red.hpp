#pragma once
#include <cstdint>
#include <optional>
#include <vector>
#include <deque>
#include <api/array_view.h>

namespace lk {
// RFC 2198 payload envelope only. The encoded Opus blocks remain untouched.
// Validate all lengths before exposing a primary block or changing any PT.
struct RedPayload {
  size_t primary_offset=0;
  std::vector<size_t> type_offsets;
};
inline std::optional<RedPayload> parse_audio_red(webrtc::ArrayView<const uint8_t> data,int opus_pt) {
  RedPayload result; size_t cursor=0,redundant=0;
  while (cursor<data.size()) {
    if ((data[cursor]&0x7f)!=opus_pt) return {};
    result.type_offsets.push_back(cursor);
    if (!(data[cursor]&0x80)) {
      result.primary_offset=cursor+1+redundant;
      if (result.primary_offset>=data.size()) return {};
      return result;
    }
    if (data.size()-cursor<4 || result.type_offsets.size()>32) return {};
    redundant+=((data[cursor+2]&3)<<8)|data[cursor+3]; cursor+=4;
    if (redundant>data.size()) return {};
  }
  return {};
}
class AudioRedEncoder {
  struct Block {uint32_t timestamp;std::vector<uint8_t> bytes;};
  std::deque<Block> history_;
 public:
  void clear(){history_.clear();}
  std::vector<uint8_t> encode(webrtc::ArrayView<const uint8_t> primary,uint32_t timestamp,int pt,size_t capacity) {
    // RFC 2198 uses a 14-bit timestamp offset and a 10-bit block length.
    // Drop old redundancy before exceeding the negotiated packet capacity.
    while(!history_.empty() && (uint32_t(timestamp-history_.front().timestamp)>=16384 ||
        timestamp==history_.front().timestamp))history_.pop_front();
    size_t size=primary.size()+1;
    for(const auto& block:history_)size+=4+block.bytes.size();
    while(!history_.empty() && size>capacity){size-=4+history_.front().bytes.size();history_.pop_front();}
    std::vector<uint8_t> result;result.reserve(size);
    for(const auto& block:history_){
      auto offset=uint32_t(timestamp-block.timestamp);auto n=block.bytes.size();
      result.push_back(uint8_t(0x80|pt));result.push_back(uint8_t(offset>>6));
      result.push_back(uint8_t(((offset&63)<<2)|(n>>8)));result.push_back(uint8_t(n));
    }
    result.push_back(uint8_t(pt));
    for(const auto& block:history_)result.insert(result.end(),block.bytes.begin(),block.bytes.end());
    result.insert(result.end(),primary.begin(),primary.end());
    if(primary.size()<=1023){history_.push_back({timestamp,{primary.begin(),primary.end()}});while(history_.size()>2)history_.pop_front();}
    return result;
  }
};
}
