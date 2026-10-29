#pragma once
#include "lk/common.hpp"
#include "lk/video_subscription.hpp"
#include <api/peer_connection_interface.h>
#include <rtc_base/thread.h>
#include <memory>
#include <map>
#include <set>

namespace lk {
struct MediaSubscriptionState {
  bool active = false, has_feedback = false;
  double loss = 0;
  int64_t rtt_ms = 0;
  std::string codec;
  double quality_score = 100;
};

struct MediaSourceState {
  double audio_level = 0;
  std::vector<VideoLayerChoice> layers;
  double quality_score = 100;
};

// Internal adapter pinned to the adjacent M137 native WebRTC checkout. All
// packet routing is confined to its network thread, never the room event loop.
class SfuRouter {
 public:
  SfuRouter(webrtc::Thread* signaling, webrtc::Thread* network, webrtc::Thread* worker);
  ~SfuRouter();
  Json metrics() const;
  void configure_congestion_control(bool enabled, bool allow_pause);
  std::map<const webrtc::MediaStreamTrackInterface*, MediaSourceState> source_states(
      const std::set<const webrtc::MediaStreamTrackInterface*>& muted = {}) const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  friend class SfuSession;
};

class SfuSession {
 public:
  SfuSession(SfuRouter&, bool subscriber, bool bidirectional = false);
  ~SfuSession();
  void add(
      const std::string& sid, const std::string& sender_id,
      webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> source,
      const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface>>& variants = {});
  void remove(const std::string& sid);
  void enable(const std::string& sid, bool enabled);
  void settings(const std::string& sid, const VideoSubscription&);
  void publication(const std::string& cid, std::optional<bool> encrypted);
  std::map<std::string, MediaSubscriptionState> states() const;
  void prepare_answer(webrtc::scoped_refptr<webrtc::PeerConnectionInterface>,
                      webrtc::SessionDescriptionInterface&);
  // Detach before any SDP mutation/Close can destroy or replace transports.
  void detach();
  // Rebind only after a local/remote description has completed.
  void refresh(webrtc::scoped_refptr<webrtc::PeerConnectionInterface>);

 private:
  struct State;
  std::shared_ptr<State> state_;
};
}  // namespace lk
