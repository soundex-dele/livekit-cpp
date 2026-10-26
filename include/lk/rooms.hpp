#pragma once
#include "lk/auth.hpp"
#include "lk/rtc.hpp"
#pragma push_macro("PASSTHROUGH")
#undef PASSTHROUGH
#include "livekit_rtc.pb.h"
#include "livekit_room.pb.h"
#pragma pop_macro("PASSTHROUGH")
#include <boost/asio/steady_timer.hpp>
#include <set>
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
  bool subscribed_once = false;
};
struct Participant {
  Claims claims;
  livekit::ParticipantInfo info;
  std::weak_ptr<SignalSink> sink;
  std::shared_ptr<Peer> publisher, subscriber;
  std::map<std::string, PublishedTrack> tracks;
  std::map<std::string, bool> subscriptions;
  std::set<std::string> disabled_tracks;
  std::map<std::string, VideoSubscription> track_settings;
  bool auto_subscribe = true, closed = false;
  int64_t last_seen = now_ms();
  uint32_t data_sequence = 0;
  void send(const livekit::SignalResponse& response) { if (auto s = sink.lock()) s->send(response); }
};
struct Room {
  livekit::Room info;
  std::map<std::string, std::shared_ptr<Participant>> participants;
  int64_t empty_since = now_ms();
  bool used = false;
};
class Rooms {
 public:
  Rooms(boost::asio::io_context&, RtcRuntime&, const Config&);
  ~Rooms();
  std::shared_ptr<Participant> join(const Claims&, bool auto_subscribe, std::shared_ptr<SignalSink>);
  void signal(const std::shared_ptr<Participant>&, const livekit::SignalRequest&);
  void leave(const std::shared_ptr<Participant>&, livekit::DisconnectReason = livekit::CLIENT_INITIATED, bool reconnect = false);
  Json api(const std::string& method, const Json&, const Claims&);
  void shutdown();
  Json metrics() const;
 private:
  Room& create(const livekit::CreateRoomRequest&);
  Room& room(const std::string&);
  std::shared_ptr<Participant> participant(Room&, const std::string&);
  void broadcast(Room&, const livekit::SignalResponse&);
  void updated(const std::shared_ptr<Participant>&);
  void received_track(const std::shared_ptr<Participant>&, Track, const std::string& mid);
  void unpublish(const std::shared_ptr<Participant>&, const std::string& sid);
  void subscribe(const std::shared_ptr<Participant>&, const std::string& sid, bool);
  void subscriptions_ready(const std::shared_ptr<Participant>&);
  void route_data(Room&, const std::shared_ptr<Participant>& sender, livekit::DataPacket, bool);
  void mute(const std::shared_ptr<Participant>&, const std::string&, bool);
  void tick();
  boost::asio::io_context& io_;
  RtcRuntime& rtc_;
  const Config& config_;
  boost::asio::steady_timer timer_;
  std::map<std::string, Room> rooms_;
  bool stopping_ = false;
};
}
