#pragma once
#include "lk/auth.hpp"
#include "lk/rtc.hpp"
#include "lk/rpc.hpp"
#include "lk/subscription_permissions.hpp"
#include "lk/data_packets.hpp"
#pragma push_macro("PASSTHROUGH")
#undef PASSTHROUGH
#include "livekit_rtc.pb.h"
#include "livekit_room.pb.h"
#pragma pop_macro("PASSTHROUGH")
#include <boost/asio/steady_timer.hpp>
#include <set>
#include <algorithm>
namespace lk {
struct SignalSink {
  virtual ~SignalSink() = default;
  virtual void send(const livekit::SignalResponse&) = 0;
  virtual void close() = 0;
};
struct PublishedTrack {
  std::string cid, mid;
  livekit::TrackInfo info;
  Track media;
  std::optional<webrtc::RtpCodecParameters> codec;
  bool subscribed_once = false;
};
struct Participant {
  Claims claims;
  livekit::ParticipantInfo info;
  std::weak_ptr<SignalSink> sink;
  std::shared_ptr<Peer> publisher, subscriber;
  std::map<std::string, PublishedTrack> tracks;
  std::map<std::string, bool> subscriptions;
  SubscriptionPermissions subscription_permissions;
  std::map<std::string, bool> subscription_allowed;
  std::set<std::string> disabled_tracks;
  std::map<std::string, VideoSubscription> track_settings;
  bool auto_subscribe = true, closed = false, ice_connected = false;
  int single_pc_protocol = -1;
  livekit::ClientInfo client_info;
  struct CachedUpdate { livekit::ParticipantInfo info; int64_t at; uint64_t order; };
  std::map<std::string, CachedUpdate> update_cache;
  uint64_t update_order = 0;
  int64_t last_seen = now_ms();
  std::chrono::steady_clock::time_point next_token_refresh = std::chrono::steady_clock::now() + std::chrono::minutes(5);
  uint32_t last_published_data_sequence = 0;
  uint32_t last_rtt_ms = 0;
  void send(const livekit::SignalResponse& response) {
    // Old Rust clients cannot decode these variants; pre-v15 clients expect
    // only error RequestResponses. Keep capability gating in one place.
    if ((response.has_request_response() || response.has_track_subscribed()) &&
        client_info.sdk() == livekit::ClientInfo::RUST && client_info.protocol() < 10) return;
    if (response.has_request_response() && response.request_response().reason() == livekit::RequestResponse::OK &&
        client_info.protocol() <= 14) return;
    if (response.has_update()) for (const auto& info : response.update().participants()) {
      auto& cached = update_cache[info.sid()];
      // Resume only needs tombstone identity/version. Do not retain copies of
      // every peer's metadata/track list in every participant's bounded cache.
      cached.info.Clear(); cached.info.set_sid(info.sid()); cached.info.set_identity(info.identity());
      cached.info.set_state(info.state()); cached.info.set_version(info.version());
      cached.at = now_ms(); cached.order = ++update_order;
      if (update_cache.size() > 128) {
        auto oldest = std::min_element(update_cache.begin(), update_cache.end(),
          [](const auto& a, const auto& b) { return a.second.order < b.second.order; });
        update_cache.erase(oldest);
      }
    }
    if (auto s = sink.lock()) s->send(response);
  }
};
struct Room {
  livekit::Room info;
  std::map<std::string, std::shared_ptr<Participant>> participants;
  int64_t created_at_ms = now_ms(), last_left_ms = 0;
  bool used = false;
  UserPacketDeduper user_packet_deduper;
};
class Rooms {
 public:
  Rooms(boost::asio::io_context&, RtcRuntime&, const Config&);
  ~Rooms();
  std::shared_ptr<Participant> join(const Claims&, bool auto_subscribe, std::shared_ptr<SignalSink>, int single_pc_protocol = -1,
                                    const livekit::ClientInfo& = {});
  std::shared_ptr<Participant> resume(const Claims&, std::shared_ptr<SignalSink>);
  void signal_closed(const std::shared_ptr<Participant>&, const SignalSink*);
  void signal(const std::shared_ptr<Participant>&, const livekit::SignalRequest&);
  void leave(const std::shared_ptr<Participant>&, livekit::DisconnectReason = livekit::CLIENT_INITIATED, bool reconnect = false);
  Json api(const std::string& method, const Json&, const Claims&);
  RpcRequests::Cancel participant_api(const std::string& method, const Json&, const Claims&, RpcRequests::Complete);
  void shutdown();
  Json metrics() const;
 private:
  Room& create(const livekit::CreateRoomRequest&);
  Room& room(const std::string&);
  std::shared_ptr<Participant> participant(Room&, const std::string&);
  void broadcast(Room&, const livekit::SignalResponse&);
  void refresh_room(Room&, std::optional<bool> recording = {}, bool notify = true);
  void updated(const std::shared_ptr<Participant>&);
  void refresh_token(const std::shared_ptr<Participant>&);
  void update_metadata(const std::shared_ptr<Participant>&, const std::string& name, const std::string& metadata,
                       const google::protobuf::Map<std::string, std::string>&);
  void received_track(const std::shared_ptr<Participant>&, Track, const std::string& mid, const webrtc::RtpParameters&);
  void unpublish(const std::shared_ptr<Participant>&, const std::string& sid);
  void subscribe(const std::shared_ptr<Participant>&, const std::string& sid, bool);
  void update_subscription_permissions(const std::shared_ptr<Participant>&, const livekit::SubscriptionPermission&);
  void subscriptions_ready(const std::shared_ptr<Participant>&);
  void route_data(Room&, const std::shared_ptr<Participant>& sender, livekit::DataPacket, bool);
  void mute(const std::shared_ptr<Participant>&, const std::string&, bool);
  struct ApiRpc;
  void attempt_rpc(const std::shared_ptr<ApiRpc>&);
  void finish_rpc(uint64_t, std::optional<RpcRequests::Result>);
  void tick();
  boost::asio::io_context& io_;
  RtcRuntime& rtc_;
  const Config& config_;
  boost::asio::steady_timer timer_;
  RpcRequests rpc_;
  std::map<uint64_t, std::shared_ptr<ApiRpc>> api_rpcs_;
  uint64_t next_api_rpc_ = 0;
  std::shared_ptr<int> rpc_lifetime_ = std::make_shared<int>(0);
  std::map<std::string, Room> rooms_;
  bool stopping_ = false;
};
}
