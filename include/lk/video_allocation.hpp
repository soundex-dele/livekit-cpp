#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace lk {
// Application policy over GoogCC's target; this does not estimate bandwidth.
struct VideoLayerRate { uint32_t ssrc=0; int64_t bps=0; };
struct VideoAllocationState {
  uint32_t selected=0,pending=0;
  int64_t pending_since_ms=0;
  bool initialized=false;
  bool pending_fast_resume=false;
};
struct VideoBandwidthRequest {
  std::string id;
  uint32_t priority=0;
  // Low-to-high, already capped by the user's quality/viewport request.
  std::vector<VideoLayerRate> layers;
  VideoAllocationState state;
  uint64_t subscription_order=0;
};
struct MediaAllocation {
  int64_t media_budget_bps=0,audio_reserved_bps=0,video_allocated_bps=0;
  size_t paused=0;
};
// Leave spare capacity visible to native ALR detection. Keep feedback alive
// during a full pause; fill near-target traffic only while more media is wanted.
// GoogCC still caps actual padding rate.
inline int64_t recovery_padding_demand(int64_t target_bps,int64_t admitted_bps,int64_t requested_bps) {
  if (requested_bps<=0) return 0;
  // Keep feedback flowing while all video is paused, but leave enough spare
  // capacity for native ALR detection and its periodic recovery probes.
  if (admitted_bps<=0)
    return std::min(requested_bps,std::max<int64_t>(0,target_bps)/2);
  return admitted_bps>=std::max<int64_t>(0,target_bps)*60/100 ? requested_bps : 0;
}
inline MediaAllocation allocate_video_bandwidth(int64_t target_bps,int64_t audio_demand_bps,
    int64_t now_ms,std::vector<VideoBandwidthRequest>& requests) {
  MediaAllocation result;
  // Keep 15% for retransmission/control traffic. Audio takes precedence even
  // when its demand exceeds the target; no video is then admitted.
  result.media_budget_bps=std::max<int64_t>(0,target_bps)/100*85;
  result.audio_reserved_bps=std::min(result.media_budget_bps,std::max<int64_t>(0,audio_demand_bps));
  int64_t budget=result.media_budget_bps-result.audio_reserved_bps;
  std::vector<size_t> order(requests.size()); std::iota(order.begin(),order.end(),0);
  auto priority=[](uint32_t p){return p ? uint64_t(p) : uint64_t(std::numeric_limits<uint32_t>::max())+1;};
  std::sort(order.begin(),order.end(),[&](size_t a,size_t b) {
    auto pa=priority(requests[a].priority),pb=priority(requests[b].priority);
    if (pa!=pb) return pa<pb;
    if (requests[a].subscription_order!=requests[b].subscription_order)
      return requests[a].subscription_order<requests[b].subscription_order;
    return requests[a].id<requests[b].id;
  });
  for (auto& request:requests) {
    int64_t cost=1;
    for (auto& layer:request.layers) { cost=std::max(cost,layer.bps); layer.bps=cost; }
  }
  auto distribute=[&](int64_t remaining) {
    std::vector<int> chosen(requests.size(),-1);
    // Admit base layers before upgrading any video; priority resolves shortage.
    for (auto i:order) if (!requests[i].layers.empty() && requests[i].layers[0].bps<=remaining) {
      chosen[i]=0; remaining-=requests[i].layers[0].bps;
    }
    for (auto i:order) if (chosen[i]>=0) {
      const auto& layers=requests[i].layers;
      for (size_t j=1;j<layers.size();++j) {
        auto extra=layers[j].bps-layers[size_t(chosen[i])].bps;
        if (extra>remaining) break;
        remaining-=extra; chosen[i]=int(j);
      }
    }
    return chosen;
  };
  auto desired=distribute(budget),guarded=distribute(budget/115*100),ample=distribute(budget/2);
  for (size_t i=0;i<requests.size();++i) {
    auto& request=requests[i]; auto& state=request.state;
    int current=-1;
    for (size_t j=0;j<request.layers.size();++j) if (request.layers[j].ssrc==state.selected) current=int(j);
    int selected=current;
    if (!state.initialized) selected=desired[i]>=0 ? 0 : -1;
    // A user cap or inactive source can remove the current layer. Fall back
    // directly to the best affordable remaining layer, not to base followed
    // by a delayed upgrade. There is no higher active layer to preserve here.
    else if (state.selected && current<0) selected=desired[i];
    else if (desired[i]<current) selected=desired[i]; // Downshift without delay.
    int upgrade=std::min(desired[i],guarded[i]);
    if (upgrade>selected) {
      auto candidate=request.layers[size_t(upgrade)].ssrc;
      // Native initial probing can grow the budget several-fold after an
      // early keyframe temporarily paused video. With 100% spare capacity,
      // require one full allocation interval instead of a one-second freeze.
      // Reset the timer if either the candidate or headroom class changes.
      bool fast_resume=selected<0 && state.initialized && ample[i]>=upgrade;
      if (state.pending!=candidate || state.pending_fast_resume!=fast_resume) {
        state.pending=candidate; state.pending_since_ms=now_ms;
        state.pending_fast_resume=fast_resume;
      }
      if (now_ms-state.pending_since_ms>=(fast_resume ? 200 : 1000)) { selected=upgrade; state.pending=0; }
    } else state.pending=0;
    // Waiting for source discovery or the first affordable base layer is not
    // a congestion pause. Start that base layer immediately when it first fits;
    // hysteresis applies to upgrades and to resuming previously allocated video.
    state.initialized|=selected>=0;
    state.selected=selected>=0 ? request.layers[size_t(selected)].ssrc : 0;
    if (selected>=0) result.video_allocated_bps+=request.layers[size_t(selected)].bps;
    else ++result.paused;
  }
  return result;
}
}
