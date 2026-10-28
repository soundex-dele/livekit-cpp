#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace lk {
// Default activity window from the pinned Go audio/audiolevel.go: -35 dBov,
// 40% active time, 400 ms observation and EMA over two intervals.
class AudioActivity {
  std::optional<uint32_t> timestamp_;
  uint64_t duration_=0,active_=0;
  int loudest_=127;
  int64_t observed_at_=0;
  double level_=0;
 public:
  void observe(int dbov,uint32_t timestamp,int clock_rate,int64_t now_ms) {
    if(!timestamp_)timestamp_=timestamp;
    auto elapsed=uint32_t(timestamp-*timestamp_);
    if(elapsed>=0x80000000u || clock_rate<=0)return;
    timestamp_=timestamp;observed_at_=now_ms;
    auto duration=uint64_t(elapsed)*1000/clock_rate;
    duration_+=duration;
    if(dbov<=35){active_+=duration;loudest_=std::min(loudest_,dbov);}
    if(duration_>=400){
      double next=active_>=160 ? std::pow(10.0,-loudest_/20.0)*(double(active_)/400) : 0;
      level_=active_>=160 ? level_+(next-level_)*(2.0/3) : 0;
      duration_=active_=0;loudest_=127;
    }
  }
  double level(int64_t now_ms) {
    if(now_ms-observed_at_>=800){level_=0;duration_=active_=0;loudest_=127;}
    return level_>=std::pow(10.0,-35.0/20) ? level_ : 0;
  }
};
}
