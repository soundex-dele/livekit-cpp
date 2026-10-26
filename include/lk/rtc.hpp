#pragma once
#include "lk/video_subscription.hpp"
#include "lk/config.hpp"
#include <boost/asio/io_context.hpp>
#include <api/peer_connection_interface.h>
#include <rtc_base/thread.h>
#include <functional>
#include <deque>
#include <map>
#include <memory>

namespace lk {
class SfuRouter;
class SfuSession;
struct CodecAudit;
class RtcRuntime {
 public:
  RtcRuntime();
  ~RtcRuntime();
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
  std::unique_ptr<SfuRouter> sfu;
  std::shared_ptr<CodecAudit> codec_audit;
  Json media_metrics() const;
 private:
  std::unique_ptr<webrtc::Thread> network_, worker_, signaling_;
  bool ssl_ = false;
};
using Track = webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface>;
struct PeerEvents {
  std::function<void(std::string, std::string)> description;
  std::function<void(std::string)> candidate;
  std::function<void(Track, std::string)> track;
  std::function<void(std::string)> remove_track;
  std::function<void(std::string, bool)> data;
  std::function<void()> connected;
  std::function<void()> negotiated;
  std::function<void(std::string)> error;
};
class Peer : public webrtc::PeerConnectionObserver, public std::enable_shared_from_this<Peer> {
 public:
  static std::shared_ptr<Peer> create(boost::asio::io_context&, RtcRuntime&, const Config&, bool subscriber, PeerEvents);
  ~Peer();
  void close();
  void remote_description(const std::string& type, const std::string& sdp);
  void ice_candidate(const std::string& json);
  void negotiate();
  void add_track(const std::string& sid, const std::string& participant_sid, Track track);
  void remove_track(const std::string& sid);
  void enable_track(const std::string& sid, bool enabled);
  void track_settings(const std::string& sid, const VideoSubscription&);
  bool send_data(const std::string& bytes, bool reliable);
  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override;
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
  void OnIceCandidate(const webrtc::IceCandidateInterface*) override;
  void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState) override;
  void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>) override;
  void OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface>) override;
 private:
  struct Channel;
  Peer(boost::asio::io_context&, RtcRuntime&, bool, PeerEvents);
  void initialize(const Config&);
  void create_description(bool offer);
  void observe_channel(webrtc::scoped_refptr<webrtc::DataChannelInterface>);
  void flush_data();
  void fail(const std::string&);
  boost::asio::io_context& io_;
  RtcRuntime& runtime_;
  bool subscriber_, closed_ = false, negotiating_ = false, dirty_ = false, remote_ready_ = false;
  PeerEvents events_;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
  std::unique_ptr<SfuSession> sfu_;
  std::vector<std::unique_ptr<Channel>> channels_;
  std::vector<std::string> candidates_;
  std::deque<std::string> reliable_queue_;
  size_t reliable_bytes_ = 0;
  std::map<std::string, webrtc::scoped_refptr<webrtc::RtpSenderInterface>> senders_;
};
}
