#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace lk {
struct MediaQualitySample {
  uint64_t packets=0,bytes=0;
  int64_t lost=0,rtt_ms=0,last_rtcp_ms=0;
  double jitter_ms=0;
};
// R-factor and MOS rules from the fixed Go connectionquality/scorer.go.
// Statistics are supplied by the native RTP/RTCP adapter, never by decoding.
class MediaQuality {
  std::optional<int64_t> at_;
  MediaQualitySample previous_;
  double score_=100;
  std::array<unsigned,250> pps_{};
  unsigned readings_=0,mode_=0;
  int64_t mode_at_=0;
  bool muted_=false,paused_=false;
 public:
  double score()const{return score_;}
  static float mos(double score){
    if(score<=0)return 1;if(score>=100)return 4.5f;
    return float(1+0.035*score+0.000007*score*(score-60)*(100-score));
  }
  void update(int64_t now,const MediaQualitySample& sample,double loss_weight,bool muted,bool paused,
              double distance=0,bool include_rtt=false,bool include_jitter=false) {
    if(muted && !muted_ && score_!=20)score_=100;
    if(paused && !paused_)score_=30;
    bool resumed=(muted_ && !muted)||(paused_ && !paused);
    muted_=muted;paused_=paused;
    if(!at_ || resumed || sample.packets<previous_.packets){at_=now;previous_=sample;return;}
    if(now-*at_<5000)return;
    const auto duration=now-*at_;auto packets=sample.packets-previous_.packets;
    auto lost=std::max<int64_t>(0,sample.lost-previous_.lost);
    previous_=sample;at_=now;
    if(muted || paused || sample.packets==0)return;
    double pps=double(packets)*1000/duration;
    auto bin=std::min(249,int(pps/2+0.5));++pps_[bin];++readings_;
    if(readings_>10 && (!mode_at_ || now-mode_at_>120000)){
      unsigned maximum=0;
      for(unsigned i=5;i<pps_.size();++i)if(pps_[i]>maximum){maximum=pps_[i];mode_=i;}
      for(auto& count:pps_)count>>=1;mode_at_=now;
    }
    if(mode_>0 && mode_<249)loss_weight*=std::sqrt(std::min(1.0,pps/(mode_*2)));
    if(!packets){score_=sample.last_rtcp_ms && now-sample.last_rtcp_ms>duration ? 20 : 40;return;}
    double delay=(include_rtt ? sample.rtt_ms/2.0 : 0)+(include_jitter ? sample.jitter_ms*2 : 0);
    double penalty=delay>160 ? (delay-120)/10 : delay/40;
    double packet_score=std::max(0.0,100-penalty-double(lost)*100/packets*loss_weight);
    double next=std::min(packet_score,std::clamp(100-distance*35,0.0,100.0));
    double factor=next<score_ ? 0.8 : 0.4;
    score_=std::max(30.0,factor*next+(1-factor)*score_);
  }
};
}
