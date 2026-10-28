#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace lk {
// Application subscription policy; no RTP/codec parsing belongs here.
struct VideoSubscription {
  int quality=2; // LOW=0, MEDIUM=1, HIGH=2; dimensions take precedence.
  uint32_t width=0,height=0;
  uint32_t priority=0; // 1 is highest; 0 uses the default priority (last).
  uint32_t fps=0; // Temporal-layer cap; zero leaves the source frame rate intact.
  bool operator==(const VideoSubscription&) const = default;
};
struct VideoLayerChoice {
  uint32_t ssrc=0,width=0,height=0;
  int quality=2;
};
inline uint32_t select_video_layer(std::vector<VideoLayerChoice> layers,const VideoSubscription& wanted) {
  if (layers.empty()) return 0;
  std::sort(layers.begin(),layers.end(),[](const auto& a,const auto& b) {
    if (a.quality!=b.quality) return a.quality<b.quality;
    return a.ssrc<b.ssrc;
  });
  if (wanted.width || wanted.height) {
    for (const auto& layer:layers)
      if ((!wanted.width || layer.width>=wanted.width) && (!wanted.height || layer.height>=wanted.height)) return layer.ssrc;
    return layers.back().ssrc;
  }
  uint32_t selected=layers.front().ssrc;
  for (const auto& layer:layers) if (layer.quality<=wanted.quality) selected=layer.ssrc;
  return selected;
}
}
