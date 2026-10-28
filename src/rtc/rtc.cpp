#include <pc/session_description.h>
#include <pc/rtp_transceiver.h>
#include "lk/rtc.hpp"
#include "lk/headless_audio.hpp"
#include "lk/sfu.hpp"
#include "lk/codec_audit.hpp"
#include "lk/media_codec.hpp"
#include <boost/asio/post.hpp>
#include <api/audio/audio_device.h>
#include <api/create_peerconnection_factory.h>
#include <api/enable_media.h>
#include <api/field_trials.h>
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
RtcRuntime::RtcRuntime(bool packet_forwarding) {
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
  if(packet_forwarding)deps.trials=webrtc::FieldTrials::CreateNoGlobal("WebRTC-DependencyDescriptorAdvertised/Enabled/WebRTC-GenericDescriptorAdvertised/Enabled/");
  deps.sctp_factory = std::make_unique<NativeSctpFactory>(network_.get());
  deps.adm = webrtc::make_ref_counted<HeadlessAudioDevice>();
  deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
  deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
  codec_audit = std::make_shared<CodecAudit>();
  deps.video_encoder_factory = audited_video_encoders(codec_audit,packet_forwarding);
  deps.video_decoder_factory = audited_video_decoders(codec_audit,packet_forwarding);
  // Received PCM already passed the publisher's processing. Keep server-side
  // echo cancellation, noise suppression and gain control out of the relay.
  webrtc::EnableMedia(deps);
  factory = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
  if (!factory) throw std::runtime_error("WebRTC factory creation failed");
  // Local senders and browser receivers must have a loopback ICE path. Without
  // it, local integration may select a VPN interface even with a localhost URL.
  webrtc::PeerConnectionFactoryInterface::Options options;
  options.network_ignore_mask = 0;
  factory->SetOptions(options);
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
Peer::Peer(boost::asio::io_context& io, RtcRuntime& runtime, bool subscriber, PeerEvents events, int single_pc_protocol)
    : io_(io), runtime_(runtime), subscriber_(subscriber), single_pc_protocol_(single_pc_protocol), events_(std::move(events)) {}
std::shared_ptr<Peer> Peer::create(boost::asio::io_context& io, RtcRuntime& runtime, const Config& config, bool subscriber, PeerEvents events, int single_pc_protocol) {
  auto peer = std::shared_ptr<Peer>(new Peer(io, runtime, subscriber, std::move(events), single_pc_protocol));
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
  if (config.media_mode == "sfu") sfu_ = std::make_unique<SfuSession>(*runtime_.sfu, subscriber_, single_pc_protocol_ >= 0);
  if (subscriber_ || single_pc_protocol_ > 17) {
    for (bool reliable : {true, false}) {
      webrtc::DataChannelInit init;
      init.ordered = reliable;
      if (single_pc_protocol_ > 17) { init.negotiated = true; init.id = reliable ? 100 : 101; }
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
void Peer::remote_description(const std::string& type, const std::string& sdp, uint32_t id) {
  if (closed_) return;
  if ((subscriber_ && type != "answer") || (!subscriber_ && type != "offer")) throw Error(400, "invalid_argument", "Unexpected SDP type");
  webrtc::SdpParseError error;
  auto desc = webrtc::CreateSessionDescription(subscriber_ ? webrtc::SdpType::kAnswer : webrtc::SdpType::kOffer, sdp, &error);
  if (!desc) throw Error(400, "invalid_argument", "Invalid SDP: " + error.description);
  if (sfu_) sfu_->detach();
  if (single_pc_protocol_ >= 0) { negotiating_ = true; dirty_ = false; }
  auto weak = weak_from_this();
  auto observer = webrtc::make_ref_counted<SetObserver>([weak, io = &io_, id](std::string error) {
    boost::asio::post(*io, [weak, error, id] {
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
      else p->create_description(false, id);
    });
  });
  pc_->SetRemoteDescription(observer.get(), desc.release());
}
void Peer::create_description(bool offer, uint32_t id) {
  if (offer) id = ++next_offer_id_;
  if (closed_) return;
  if (sfu_) {
    // Advertise only the packet adapter's supported codec contract. Native
    // defaults also advertise RED/ULPFEC and other codecs; negotiating those
    // without corresponding receive modules silently discards valid media.
    for (const auto& transceiver : pc_->GetTransceivers()) {
      if (transceiver->stopped()) continue;
      auto capabilities = runtime_.factory->GetRtpReceiverCapabilities(transceiver->media_type());
      std::vector<webrtc::RtpCodecCapability> codecs;
      auto source=sender_codecs_.find(transceiver->sender()->id());
      std::optional<webrtc::RtpCodecParameters> selected;
      if (source!=sender_codecs_.end() && !source->second.empty()) selected=source->second.front();
      else if (!offer && pc_->remote_description() && transceiver->mid()) {
        const auto* content=pc_->remote_description()->description()->GetContentByName(*transceiver->mid());
        auto requested=publication_codecs_.find(transceiver->receiver()->track()->id());
        if (content) for (const auto& offered:content->media_description()->codecs()) {
          webrtc::RtpCodecParameters candidate;
          candidate.name=offered.name;candidate.clock_rate=offered.clockrate;
          if (transceiver->media_type()==webrtc::MediaType::AUDIO) candidate.num_channels=int(offered.channels);
          candidate.parameters=offered.params;candidate.payload_type=offered.id;
          if (forwarding_codec(candidate) && (requested==publication_codecs_.end() || codec_name(candidate.name)==requested->second)) {
            selected=std::move(candidate);break;
          }
        }
      }
      for (const auto& codec : capabilities.codecs)
        if (codec_name(codec.name)=="rtx" || (codec_name(codec.name)=="red" && transceiver->media_type()==webrtc::MediaType::AUDIO && (!selected || codec_name(selected->name)=="opus")) || (forwarding_codec(codec) &&
            (source!=sender_codecs_.end() ? std::any_of(source->second.begin(),source->second.end(),[&](const auto& choice){return same_forwarding_codec(codec,choice);}) : (!selected || same_forwarding_codec(codec,*selected))))) codecs.push_back(codec);
      if(source!=sender_codecs_.end()) {
        auto rank=[&](const auto& codec){for(size_t i=0;i<source->second.size();++i)if(same_forwarding_codec(codec,source->second[i]))return i;return source->second.size();};
        std::stable_sort(codecs.begin(),codecs.end(),[&](const auto& a,const auto& b){return rank(a)<rank(b);});
      } else if(!offer && transceiver->media_type()==webrtc::MediaType::AUDIO && pc_->remote_description() && transceiver->mid()) {
        const auto* content=pc_->remote_description()->description()->GetContentByName(*transceiver->mid());
        if(content && content->media_description()){
          // Preserve the publisher's RED/Opus preference. Selecting Opus as
          // the inner codec must not silently move it ahead of offered RED.
          const auto& offered=content->media_description()->codecs();
          auto rank=[&](const auto& codec){
            for(size_t i=0;i<offered.size();++i)if(codec_name(codec.name)==codec_name(offered[i].name) && codec.clock_rate==offered[i].clockrate)return i;
            return offered.size();
          };
          std::stable_sort(codecs.begin(),codecs.end(),[&](const auto& a,const auto& b){return rank(a)<rank(b);});
        }
      }
      auto result = transceiver->SetCodecPreferences(codecs);
      if (!result.ok()) { fail(std::string(result.message())); return; }
    }
  }
  auto weak = weak_from_this();
  auto observer = webrtc::make_ref_counted<CreateObserver>([weak, io = &io_, offer, id](std::string sdp, std::string error) {
    boost::asio::post(*io, [weak, offer, sdp, error, id]() mutable {
      auto p = weak.lock(); if (!p || p->closed_) return;
      if (!error.empty()) return p->fail(error);
      auto desc = webrtc::CreateSessionDescription(offer ? webrtc::SdpType::kOffer : webrtc::SdpType::kAnswer, sdp);
      if (!desc) return p->fail("Cannot parse native local description");
      if (!offer && p->sfu_) {
        try { p->sfu_->prepare_answer(p->pc_,*desc); desc->ToString(&sdp); }
        catch (const std::exception& e) { p->fail(e.what()); return; }
      }
      auto set = webrtc::make_ref_counted<SetObserver>([weak, io = &p->io_, offer, sdp, id](std::string error) {
        boost::asio::post(*io, [weak, offer, sdp, error, id] {
          if (auto p = weak.lock(); p && !p->closed_) {
            if (!error.empty()) p->fail(error);
            else {
              try { if (p->sfu_) p->sfu_->refresh(p->pc_); }
              catch (const std::exception& e) { p->fail(e.what()); return; }
              p->deliver_tracks();
              if (p->closed_) return;
              p->events_.description(offer ? "offer" : "answer", sdp, id);
              if (!offer && p->single_pc_protocol_ >= 0) {
                p->negotiating_ = false;
                if (p->events_.negotiated) p->events_.negotiated();
                // Answering the initial offer can leave server-added tracks
                // without a matching client media section. Request only new ones.
                p->negotiate();
              }
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
  if (closed_) return;
  if (single_pc_protocol_ >= 0) {
    if (negotiating_ || !remote_ready_) { dirty_ = true; return; }
    uint32_t audio = 0, video = 0;
    for (const auto& t : pc_->GetTransceivers()) {
      if (t->stopped() || t->mid() || !t->sender()->track()) continue;
      if (!requested_senders_.insert(t->sender()->id()).second) continue;
      if (t->media_type() == webrtc::MediaType::AUDIO) ++audio; else ++video;
    }
    if ((audio || video || dirty_) && events_.media_sections) events_.media_sections(audio, video);
    dirty_ = false;
    return;
  }
  if (!subscriber_) return;
  if (negotiating_) { dirty_ = true; return; }
  negotiating_ = true; dirty_ = false; create_description(true);
}
void Peer::restart_ice() {
  if (closed_) return;
  if (single_pc_protocol_ >= 0) {
    // The client restarts this PC. Reissue any media-section request lost on
    // the old signal transport after its next offer has been answered.
    requested_senders_.clear(); dirty_ = true; return;
  }
  if (!subscriber_) return;
  pc_->RestartIce();
  // A previously sent offer/answer may have been lost with the signal socket.
  // CreateOffer can replace a local offer; do not wait forever for its answer.
  negotiating_ = false;
  negotiate();
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
void Peer::add_track(const std::string& sid, const std::string& participant_sid, Track remote, const std::optional<webrtc::RtpCodecParameters>& codec,const std::vector<TrackVariant>& variants) {
  if (closed_) return;
  std::vector<Track> variant_tracks;
  std::vector<webrtc::RtpCodecParameters> codecs;
  if(codec)codecs.push_back(*codec);
  for(const auto& variant:variants){if(variant.track)variant_tracks.push_back(variant.track);if(variant.codec)codecs.push_back(*variant.codec);}
  if(senders_.contains(sid)) {
    if(sfu_){
      auto& current=sender_codecs_[senders_[sid]->id()];
      bool changed=current!=codecs;current=codecs;
      sfu_->add(sid,senders_[sid]->id(),remote,variant_tracks);
      sfu_->refresh(pc_);
      if(changed){dirty_=true;negotiate();}
    }
    return;
  }
  Track local;
  if (remote->kind() == "audio") {
    auto track = static_cast<webrtc::AudioTrackInterface*>(remote.get());
    local = runtime_.factory->CreateAudioTrack(sid, track->GetSource());
  } else {
    auto track = static_cast<webrtc::VideoTrackInterface*>(remote.get());
    local = runtime_.factory->CreateVideoTrack(webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>(track->GetSource()), sid);
  }
  const std::string stream_id = participant_sid + "|" + sid;
  if (single_pc_protocol_ >= 0) {
    // AddTrack may reuse a recvonly server transceiver whose remote side is
    // sendonly. That peer cannot receive our subscription on that media section.
    // Reuse only client receive sections; otherwise request a new one.
    for (const auto& transceiver : pc_->GetTransceivers()) {
      if (transceiver->stopped() || transceiver->sender()->track() ||
          (transceiver->media_type() == webrtc::MediaType::AUDIO) != (local->kind() == "audio")) continue;
      const auto remote_direction = runtime_.signaling_->BlockingCall([&]() -> std::optional<webrtc::RtpTransceiverDirection> {
        auto* proxy = static_cast<webrtc::RtpTransceiverProxyWithInternal<webrtc::RtpTransceiver>*>(transceiver.get());
        // Preserve AddTrack's native no-reuse-after-send rule: reusing an old
        // sender also reuses its MSID/SSRC and breaks new publication identity.
        if (proxy->internal()->has_ever_been_used_to_send()) return {};
        if (!pc_->remote_description() || !transceiver->mid()) return {};
        const auto* content = pc_->remote_description()->description()->GetContentByName(*transceiver->mid());
        if (!content || content->rejected || !content->media_description()) return {};
        return content->media_description()->direction();
      });
      if (!remote_direction || (*remote_direction != webrtc::RtpTransceiverDirection::kRecvOnly &&
                                *remote_direction != webrtc::RtpTransceiverDirection::kSendRecv)) continue;
      auto sender = transceiver->sender();
      if (!sender->SetTrack(local.get())) { fail("Cannot attach subscription track"); return; }
      sender->SetStreams({stream_id});
      auto result = transceiver->SetDirectionWithError(*remote_direction == webrtc::RtpTransceiverDirection::kSendRecv
          ? webrtc::RtpTransceiverDirection::kSendRecv : webrtc::RtpTransceiverDirection::kSendOnly);
      if (!result.ok()) { fail(std::string(result.message())); return; }
      senders_[sid] = sender; break;
    }
    if (!senders_.contains(sid)) {
      webrtc::RtpTransceiverInit init; init.direction = webrtc::RtpTransceiverDirection::kSendOnly; init.stream_ids = {stream_id};
      auto result = pc_->AddTransceiver(local, init);
      if (!result.ok()) { fail(std::string(result.error().message())); return; }
      auto transceiver = result.MoveValue();
      // M137 associates new remote recvonly sections only with transceivers
      // created by AddTrack (JSEP 5.10). We chose this transceiver explicitly
      // to avoid reusing a remote sendonly section, then restore that native
      // association eligibility. Keep this pinned adapter alongside the SFU's
      // existing native transceiver access; no SDP MID is assigned by hand.
      runtime_.signaling_->BlockingCall([&] {
        auto* proxy = static_cast<webrtc::RtpTransceiverProxyWithInternal<webrtc::RtpTransceiver>*>(transceiver.get());
        proxy->internal()->set_created_by_addtrack(true);
      });
      senders_[sid] = transceiver->sender();
    }
  } else {
    auto result = pc_->AddTrack(local, {stream_id});
    if (!result.ok()) { fail(std::string(result.error().message())); return; }
    senders_[sid] = result.MoveValue();
  }
  if (sfu_ && codec) sender_codecs_[senders_[sid]->id()]=std::move(codecs);
  if (sfu_) sfu_->add(sid, senders_[sid]->id(), remote,variant_tracks);
  dirty_ = true; negotiate();
}
void Peer::remove_track(const std::string& sid) {
  auto it = senders_.find(sid); if (closed_ || it == senders_.end()) return;
  if (sfu_) sfu_->remove(sid);
  auto result = pc_->RemoveTrackOrError(it->second);
  requested_senders_.erase(it->second->id());
  sender_codecs_.erase(it->second->id());
  senders_.erase(it);
  if (!result.ok()) { fail(std::string(result.message())); return; }
  dirty_ = true; negotiate();
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
  // OnTrack precedes answer application; GetParameters can still be empty.
  // Publish metadata/subscriptions only after the negotiated channel exists.
  boost::asio::post(io_, [weak = weak_from_this(), t] {
    if (auto p=weak.lock();p && !p->closed_) p->pending_tracks_.push_back(t);
  });
}
std::map<std::string,MediaSubscriptionState> Peer::media_states() const {return sfu_ ? sfu_->states() : std::map<std::string,MediaSubscriptionState>{};}
void Peer::publication_codec(const std::string& cid,const std::string& codec,std::optional<bool> encrypted) {
  if(sfu_)sfu_->publication(cid,encrypted);
  if (codec.empty()) publication_codecs_.erase(cid);
  else publication_codecs_[cid]=codec_name(codec);
}
void Peer::deliver_tracks() {
  auto tracks=std::exchange(pending_tracks_,{});
  for (const auto& t:tracks) {
    if (closed_) break;
    auto mid=t->mid().value_or("");
    auto parameters=t->receiver()->GetParameters();
    if (pc_->local_description()) {
      const auto* content=pc_->local_description()->description()->GetContentByName(mid);
      if (content && content->media_description()) negotiated_codecs(parameters,*content->media_description());
    }
    if (events_.track) events_.track(t->receiver()->track(),mid,parameters);
  }
}
void Peer::OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> r) {
  auto id = r->track()->id();
  boost::asio::post(io_, [weak = weak_from_this(), id] { if (auto p = weak.lock(); p && !p->closed_ && p->events_.remove_track) p->events_.remove_track(id); });
}
}
