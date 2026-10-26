#include "lk/rtc.hpp"
#include "lk/headless_audio.hpp"
#include "lk/sfu.hpp"
#include "lk/codec_audit.hpp"
#include <boost/asio/post.hpp>
#include <api/audio/audio_device.h>
#include <api/create_peerconnection_factory.h>
#include <api/enable_media.h>
#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <api/make_ref_counted.h>
#include <api/task_queue/default_task_queue_factory.h>
#include <rtc_base/ssl_adapter.h>
#include <rtc_base/logging.h>
#ifdef _WIN32
#include <rtc_base/task_queue_win.h>
#endif
#include <media/sctp/dcsctp_transport.h>
#include <utility>
#include <cstdlib>

namespace lk {
namespace {
// The supplied checkout builds dcSCTP but its default factory can be compiled
// without WEBRTC_HAVE_DCSCTP. Select the native transport explicitly.
class NativeSctpFactory : public webrtc::SctpTransportFactoryInterface {
 public:
  explicit NativeSctpFactory(webrtc::Thread* network) : network_(network) {}
  std::unique_ptr<webrtc::SctpTransportInternal> CreateSctpTransport(const webrtc::Environment& env, webrtc::DtlsTransportInternal* transport) override {
    return std::make_unique<webrtc::DcSctpTransport>(env, network_, transport);
  }
 private: webrtc::Thread* network_;
};
class SetObserver : public webrtc::SetSessionDescriptionObserver {
 public:
  explicit SetObserver(std::function<void(std::string)> cb) : cb_(std::move(cb)) {}
  void OnSuccess() override { cb_(""); }
  void OnFailure(webrtc::RTCError e) override { cb_(std::string(e.message())); }
 private: std::function<void(std::string)> cb_;
};
class CreateObserver : public webrtc::CreateSessionDescriptionObserver {
 public:
  explicit CreateObserver(std::function<void(std::string, std::string)> cb) : cb_(std::move(cb)) {}
  void OnSuccess(webrtc::SessionDescriptionInterface* raw) override {
    std::unique_ptr<webrtc::SessionDescriptionInterface> desc(raw);
    std::string sdp; desc->ToString(&sdp); cb_(std::move(sdp), "");
  }
  void OnFailure(webrtc::RTCError e) override { cb_("", std::string(e.message())); }
 private: std::function<void(std::string, std::string)> cb_;
};
}
RtcRuntime::RtcRuntime() {
  const char* native_log=std::getenv("LK_WEBRTC_LOG");
  webrtc::LogMessage::LogToDebug(native_log && std::string_view(native_log)=="info" ? webrtc::LS_INFO : webrtc::LS_ERROR);
  ssl_ = webrtc::InitializeSSL();
  if (!ssl_) throw std::runtime_error("WebRTC SSL initialization failed");
  network_ = webrtc::Thread::CreateWithSocketServer();
  worker_ = webrtc::Thread::Create(); signaling_ = webrtc::Thread::Create();
  network_->SetName("rtc-network", nullptr); worker_->SetName("rtc-worker", nullptr); signaling_->SetName("rtc-signaling", nullptr);
  if (!network_->Start() || !worker_->Start() || !signaling_->Start()) throw std::runtime_error("WebRTC thread startup failed");
  webrtc::PeerConnectionFactoryDependencies deps;
  deps.network_thread = network_.get(); deps.worker_thread = worker_.get(); deps.signaling_thread = signaling_.get();
#ifdef _WIN32
  deps.task_queue_factory = webrtc::CreateTaskQueueWinFactory();
#else
  deps.task_queue_factory = webrtc::CreateDefaultTaskQueueFactory();
#endif
  deps.sctp_factory = std::make_unique<NativeSctpFactory>(network_.get());
  deps.adm = webrtc::make_ref_counted<HeadlessAudioDevice>();
  deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
  deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
  codec_audit = std::make_shared<CodecAudit>();
  deps.video_encoder_factory = audited_video_encoders(codec_audit);
  deps.video_decoder_factory = audited_video_decoders(codec_audit);
  // Received PCM already passed the publisher's processing. Keep server-side
  // echo cancellation, noise suppression and gain control out of the relay.
  webrtc::EnableMedia(deps);
  factory = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
  if (!factory) throw std::runtime_error("WebRTC factory creation failed");
  sfu = std::make_unique<SfuRouter>(signaling_.get(), network_.get(), worker_.get());
}
Json RtcRuntime::media_metrics() const {
  auto result = sfu->metrics();
  result["video_encode_calls"] = codec_audit->video_encode_calls.load();
  result["video_decode_calls"] = codec_audit->video_decode_calls.load();
  return result;
}
RtcRuntime::~RtcRuntime() {
  sfu.reset();
  factory = nullptr;
  if (signaling_) signaling_->Stop();
  if (worker_) worker_->Stop();
  if (network_) network_->Stop();
  if (ssl_) webrtc::CleanupSSL();
}
struct Peer::Channel : webrtc::DataChannelObserver {
  std::weak_ptr<Peer> owner;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> channel;
  Channel(std::weak_ptr<Peer> o, webrtc::scoped_refptr<webrtc::DataChannelInterface> c) : owner(std::move(o)), channel(std::move(c)) {}
  ~Channel() override { channel->UnregisterObserver(); }
  void schedule_flush() {
    if (auto p = owner.lock()) boost::asio::post(p->io_, [weak = owner] { if (auto p = weak.lock()) p->flush_data(); });
  }
  void OnStateChange() override { schedule_flush(); }
  void OnBufferedAmountChange(uint64_t) override { schedule_flush(); }
  void OnMessage(const webrtc::DataBuffer& buffer) override {
    if (buffer.size() > 65536 || !buffer.binary) return;
    if (auto p = owner.lock()) {
      std::string bytes(reinterpret_cast<const char*>(buffer.data.data()), buffer.size());
      bool reliable = channel->label() == "_reliable";
      boost::asio::post(p->io_, [weak = owner, bytes = std::move(bytes), reliable] {
        if (auto p = weak.lock(); p && !p->closed_ && p->events_.data) p->events_.data(bytes, reliable);
      });
    }
  }
};
Peer::Peer(boost::asio::io_context& io, RtcRuntime& runtime, bool subscriber, PeerEvents events)
    : io_(io), runtime_(runtime), subscriber_(subscriber), events_(std::move(events)) {}
std::shared_ptr<Peer> Peer::create(boost::asio::io_context& io, RtcRuntime& runtime, const Config& config, bool subscriber, PeerEvents events) {
  auto peer = std::shared_ptr<Peer>(new Peer(io, runtime, subscriber, std::move(events)));
  peer->initialize(config); return peer;
}
void Peer::initialize(const Config& config) {
  webrtc::PeerConnectionInterface::RTCConfiguration rtc;
  rtc.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  rtc.set_min_port(config.rtc_port_min); rtc.set_max_port(config.rtc_port_max);
  rtc.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
  for (const auto& server : config.ice_servers) {
    webrtc::PeerConnectionInterface::IceServer s;
    s.urls = server.urls; s.username = server.username; s.password = server.credential; rtc.servers.push_back(std::move(s));
  }
  auto result = runtime_.factory->CreatePeerConnectionOrError(rtc, webrtc::PeerConnectionDependencies(this));
  if (!result.ok()) throw std::runtime_error(std::string(result.error().message()));
  pc_ = result.MoveValue();
  if (config.media_mode == "sfu") sfu_ = std::make_unique<SfuSession>(*runtime_.sfu, subscriber_);
  if (subscriber_) {
    for (bool reliable : {true, false}) {
      webrtc::DataChannelInit init;
      init.ordered = reliable;
      if (!reliable) init.maxRetransmits = 0;
      auto channel = pc_->CreateDataChannelOrError(reliable ? "_reliable" : "_lossy", &init);
      if (!channel.ok()) throw std::runtime_error(std::string(channel.error().message()));
      observe_channel(channel.MoveValue());
    }
  }
}
Peer::~Peer() { close(); }
void Peer::close() {
  if (closed_) return;
  closed_ = true;
  sfu_.reset();
  // Unregister before destroying observers; Close synchronizes with the signaling thread.
  channels_.clear(); senders_.clear();
  if (pc_) { pc_->Close(); pc_ = nullptr; }
}
void Peer::fail(const std::string& message) {
  if (!closed_ && events_.error) events_.error(message);
}
void Peer::remote_description(const std::string& type, const std::string& sdp) {
  if (closed_) return;
  if ((subscriber_ && type != "answer") || (!subscriber_ && type != "offer")) throw Error(400, "invalid_argument", "Unexpected SDP type");
  webrtc::SdpParseError error;
  auto desc = webrtc::CreateSessionDescription(subscriber_ ? webrtc::SdpType::kAnswer : webrtc::SdpType::kOffer, sdp, &error);
  if (!desc) throw Error(400, "invalid_argument", "Invalid SDP: " + error.description);
  if (sfu_) sfu_->detach();
  auto weak = weak_from_this();
  auto observer = webrtc::make_ref_counted<SetObserver>([weak, io = &io_](std::string error) {
    boost::asio::post(*io, [weak, error] {
      auto p = weak.lock(); if (!p || p->closed_) return;
      if (!error.empty()) return p->fail(error);
      p->remote_ready_ = true;
      auto candidates = std::exchange(p->candidates_, {});
      try { for (auto& c : candidates) p->ice_candidate(c); }
      catch (const std::exception& e) { p->fail(e.what()); return; }
      if (p->subscriber_) {
        try { if (p->sfu_) p->sfu_->refresh(p->pc_); }
        catch (const std::exception& e) { p->fail(e.what()); return; }
        p->negotiating_ = false;
        if (p->events_.negotiated) p->events_.negotiated();
        if (p->dirty_) p->negotiate();
      }
      else p->create_description(false);
    });
  });
  pc_->SetRemoteDescription(observer.get(), desc.release());
}
void Peer::create_description(bool offer) {
  if (closed_) return;
  if (sfu_) {
    // Advertise only the packet adapter's supported codec contract. Native
    // defaults also advertise RED/ULPFEC and other codecs; negotiating those
    // without corresponding receive modules silently discards valid media.
    for (const auto& transceiver : pc_->GetTransceivers()) {
      if (transceiver->stopped()) continue;
      auto capabilities = runtime_.factory->GetRtpReceiverCapabilities(transceiver->media_type());
      std::vector<webrtc::RtpCodecCapability> codecs;
      for (const auto& codec : capabilities.codecs)
        if (codec.name == "opus" || codec.name == "VP8" || codec.name == "rtx") codecs.push_back(codec);
      auto result = transceiver->SetCodecPreferences(codecs);
      if (!result.ok()) { fail(std::string(result.message())); return; }
    }
  }
  auto weak = weak_from_this();
  auto observer = webrtc::make_ref_counted<CreateObserver>([weak, io = &io_, offer](std::string sdp, std::string error) {
    boost::asio::post(*io, [weak, offer, sdp, error]() mutable {
      auto p = weak.lock(); if (!p || p->closed_) return;
      if (!error.empty()) return p->fail(error);
      auto desc = webrtc::CreateSessionDescription(offer ? webrtc::SdpType::kOffer : webrtc::SdpType::kAnswer, sdp);
      if (!desc) return p->fail("Cannot parse native local description");
      if (!offer && p->sfu_) {
        try { p->sfu_->prepare_answer(p->pc_,*desc); desc->ToString(&sdp); }
        catch (const std::exception& e) { p->fail(e.what()); return; }
      }
      auto set = webrtc::make_ref_counted<SetObserver>([weak, io = &p->io_, offer, sdp](std::string error) {
        boost::asio::post(*io, [weak, offer, sdp, error] {
          if (auto p = weak.lock(); p && !p->closed_) {
            if (!error.empty()) p->fail(error);
            else {
              try { if (p->sfu_) p->sfu_->refresh(p->pc_); }
              catch (const std::exception& e) { p->fail(e.what()); return; }
              p->events_.description(offer ? "offer" : "answer", sdp);
            }
          }
        });
      });
      if (p->sfu_) p->sfu_->detach();
      p->pc_->SetLocalDescription(set.get(), desc.release());
    });
  });
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
  if (offer) pc_->CreateOffer(observer.get(), options); else pc_->CreateAnswer(observer.get(), options);
}
void Peer::negotiate() {
  if (closed_ || !subscriber_) return;
  if (negotiating_) { dirty_ = true; return; }
  negotiating_ = true; dirty_ = false; create_description(true);
}
void Peer::ice_candidate(const std::string& json) {
  if (closed_) return;
  if (!remote_ready_) {
    if (candidates_.size() >= 256) throw Error(400, "invalid_argument", "Too many ICE candidates");
    candidates_.push_back(json); return;
  }
  auto j = Json::parse(json);
  if (!j.contains("candidate") || j["candidate"].is_null() || j["candidate"] == "") return;
  webrtc::SdpParseError error;
  std::unique_ptr<webrtc::IceCandidateInterface> c(webrtc::CreateIceCandidate(j.value("sdpMid", ""), j.value("sdpMLineIndex", 0), j.at("candidate").get<std::string>(), &error));
  if (!c || !pc_->AddIceCandidate(c.get())) throw Error(400, "invalid_argument", "Invalid ICE candidate");
}
void Peer::add_track(const std::string& sid, const std::string& participant_sid, Track remote) {
  if (closed_ || senders_.contains(sid)) return;
  Track local;
  if (remote->kind() == "audio") {
    auto track = static_cast<webrtc::AudioTrackInterface*>(remote.get());
    local = runtime_.factory->CreateAudioTrack(sid, track->GetSource());
  } else {
    auto track = static_cast<webrtc::VideoTrackInterface*>(remote.get());
    local = runtime_.factory->CreateVideoTrack(webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>(track->GetSource()), sid);
  }
  auto result = pc_->AddTrack(local, {participant_sid + "|" + sid});
  if (!result.ok()) { fail(std::string(result.error().message())); return; }
  senders_[sid] = result.MoveValue();
  if (sfu_) sfu_->add(sid, senders_[sid]->id(), remote);
  negotiate();
}
void Peer::remove_track(const std::string& sid) {
  auto it = senders_.find(sid); if (closed_ || it == senders_.end()) return;
  if (sfu_) sfu_->remove(sid);
  auto result = pc_->RemoveTrackOrError(it->second);
  senders_.erase(it);
  if (!result.ok()) { fail(std::string(result.message())); return; }
  negotiate();
}
void Peer::enable_track(const std::string& sid, bool enabled) {
  if (sfu_) { sfu_->enable(sid, enabled); return; }
  auto it = senders_.find(sid);
  if (it != senders_.end()) it->second->track()->set_enabled(enabled);
}
void Peer::track_settings(const std::string& sid,const VideoSubscription& settings) {
  if (sfu_) sfu_->settings(sid,settings);
}
void Peer::observe_channel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
  if (closed_) return;
  if ((channel->label() != "_reliable" && channel->label() != "_lossy") || channels_.size() >= 2) { channel->Close(); return; }
  auto observer = std::make_unique<Channel>(weak_from_this(), channel);
  channel->RegisterObserver(observer.get()); channels_.push_back(std::move(observer));
}
bool Peer::send_data(const std::string& bytes, bool reliable) {
  if (closed_) return false;
  if (reliable) {
    if (reliable_bytes_ + bytes.size() > 1024 * 1024) return false;
    reliable_bytes_ += bytes.size(); reliable_queue_.push_back(bytes); flush_data(); return true;
  }
  for (auto& c : channels_) {
    if (c->channel->label() == (reliable ? "_reliable" : "_lossy") && c->channel->state() == webrtc::DataChannelInterface::kOpen && c->channel->buffered_amount() < 1024 * 1024)
      return c->channel->Send(webrtc::DataBuffer(webrtc::CopyOnWriteBuffer(bytes.data(), bytes.size()), true));
  }
  return false;
}
void Peer::flush_data() {
  if (closed_) return;
  for (auto& c : channels_) if (c->channel->label() == "_reliable" && c->channel->state() == webrtc::DataChannelInterface::kOpen) {
    while (!reliable_queue_.empty() && c->channel->buffered_amount() < 1024 * 1024) {
      const auto& bytes = reliable_queue_.front();
      if (!c->channel->Send(webrtc::DataBuffer(webrtc::CopyOnWriteBuffer(bytes.data(), bytes.size()), true))) break;
      reliable_bytes_ -= bytes.size(); reliable_queue_.pop_front();
    }
  }
}
void Peer::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
  boost::asio::post(io_, [weak = weak_from_this(), channel] { if (auto p = weak.lock()) p->observe_channel(channel); });
}
void Peer::OnIceCandidate(const webrtc::IceCandidateInterface* c) {
  std::string candidate; c->ToString(&candidate);
  auto json = Json{{"candidate", candidate}, {"sdpMid", c->sdp_mid()}, {"sdpMLineIndex", c->sdp_mline_index()}}.dump();
  boost::asio::post(io_, [weak = weak_from_this(), json] { if (auto p = weak.lock(); p && !p->closed_) p->events_.candidate(json); });
}
void Peer::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) {
  boost::asio::post(io_, [weak = weak_from_this(), state] {
    if (auto p = weak.lock(); p && !p->closed_) {
      if (state == webrtc::PeerConnectionInterface::PeerConnectionState::kConnected && p->events_.connected) p->events_.connected();
      if (state == webrtc::PeerConnectionInterface::PeerConnectionState::kFailed) p->fail("WebRTC connection failed");
    }
  });
}
void Peer::OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> t) {
  auto track = t->receiver()->track(); std::string mid = t->mid().value_or("");
  boost::asio::post(io_, [weak = weak_from_this(), track, mid] { if (auto p = weak.lock(); p && !p->closed_ && p->events_.track) p->events_.track(track, mid); });
}
void Peer::OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> r) {
  auto id = r->track()->id();
  boost::asio::post(io_, [weak = weak_from_this(), id] { if (auto p = weak.lock(); p && !p->closed_ && p->events_.remove_track) p->events_.remove_track(id); });
}
}
