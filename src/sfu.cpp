#include "lk/sfu.hpp"
#include "lk/native_rtp.hpp"
#include "lk/video_allocation.hpp"
#include <api/environment/environment_factory.h>
#include <pc/rtp_transceiver.h>
#include <pc/channel.h>
#include <p2p/dtls/dtls_transport_internal.h>
#include <modules/rtp_rtcp/source/rtcp_packet/common_header.h>
#include <modules/rtp_rtcp/source/rtcp_packet/nack.h>
#include <modules/rtp_rtcp/source/rtcp_packet/pli.h>
#include <modules/rtp_rtcp/include/rtp_header_extension_map.h>
#include <modules/rtp_rtcp/source/rtp_header_extensions.h>
#include <system_wrappers/include/clock.h>
#include <rtc_base/logging.h>
#include <rtc_base/rate_statistics.h>
#include <rtc_base/task_utils/repeating_task.h>
#ifdef _WIN32
#include <rtc_base/task_queue_win.h>
#endif
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace lk {
namespace {
using Transport = webrtc::RtpTransportInternal;
using TrackRef = webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface>;
using Bytes = webrtc::ArrayView<const uint8_t>;
struct Counters {
  uint64_t received=0,forwarded=0,retransmitted=0,nacks=0,plis=0,dropped=0;
  uint64_t assembled_frames=0,forwarded_frames=0,gated_frames=0;
  uint64_t layers_discovered=0,layer_switches=0,unmapped_packets=0;
  uint64_t bandwidth_pauses=0;
  uint64_t next_subscription=0;
  NativeSendTransport::Stats retired;
};
std::string lower(std::string s) {
  std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return char(std::tolower(c));}); return s;
}
bool send_packet(Transport* transport,Bytes bytes,bool rtcp,const webrtc::RtpPacketToSend* packet=nullptr) {
  if (!transport || !transport->IsSrtpActive() || !transport->IsWritable(rtcp)) return false;
  webrtc::CopyOnWriteBuffer buffer(bytes.data(),bytes.size(),bytes.size()+64);
  // RtpTransport applies SRTP. The underlying DTLS transport must not wrap it
  // again as application data (the same flag is used by native BaseChannel).
  webrtc::AsyncSocketPacketOptions options;
  if (packet && packet->transport_sequence_number()) {
    options.packet_id=*packet->transport_sequence_number();
    options.info_signaled_after_sent.included_in_feedback=packet->HasExtension<webrtc::TransportSequenceNumber>();
    options.info_signaled_after_sent.included_in_allocation=true;
  }
  return rtcp ? transport->SendRtcpPacket(&buffer,options,webrtc::PF_SRTP_BYPASS)
              : transport->SendRtpPacket(&buffer,options,webrtc::PF_SRTP_BYPASS);
}
template<class F> void rtcp_blocks(Bytes bytes,F&& callback) {
  const auto* cursor=bytes.data(); const auto* end=cursor+bytes.size();
  while (cursor<end) {
    webrtc::rtcp::CommonHeader header;
    if (!header.Parse(cursor,size_t(end-cursor))) return;
    callback(header); cursor=header.NextPacket();
  }
}
struct ReceiveTransport {
  Transport* transport;
  NativeReceiveTransport native;
  ReceiveTransport(const webrtc::Environment& env,Transport* t)
      : transport(t),native(env,[this](Bytes bytes){return send_packet(transport,bytes,true);}) {}
};
struct Output;
struct SendTransport {
  webrtc::Environment env;
  Transport* transport=nullptr;
  std::shared_ptr<NativeSendTransport> native;
  std::shared_ptr<Counters> counters;
  std::map<std::string,std::weak_ptr<Output>> outputs;
  webrtc::RepeatingTaskHandle allocation_task;
  MediaAllocation allocation;
  SendTransport(const webrtc::Environment&,std::shared_ptr<Counters>,webrtc::TaskQueueBase*);
  void allocate();
  void ready() { native->set_available(transport && transport->IsSrtpActive() && transport->IsWritable(false)); }
  void unbind() {
    if (transport) {
      transport->UnsubscribeWritableState(this); transport->UnsubscribeReadyToSend(this);
      transport->UnsubscribeSentPacket(this); transport->UnsubscribeNetworkRouteChanged(this);
      transport=nullptr;
    }
    native->set_available(false);
  }
  void bind(Transport* t) {
    if (transport==t) {ready(); return;}
    unbind(); transport=t;
    transport->SubscribeWritableState(this,[this](bool){ready();});
    transport->SubscribeReadyToSend(this,[this](bool){ready();});
    transport->SubscribeSentPacket(this,[this](const auto& packet){native->sent_packet(packet);});
    transport->SubscribeNetworkRouteChanged(this,[this](const auto& route){if (route) native->network_route(*route);});
    ready();
  }
  ~SendTransport() {
    allocation_task.Stop();
    unbind(); auto s=native->stats(); auto& r=counters->retired;
    r.sent+=s.sent; r.padding+=s.padding; r.feedback+=s.feedback; r.feedback_packets+=s.feedback_packets; r.dropped+=s.dropped;
    r.queue_dropped+=s.queue_dropped;
    r.probe_clusters+=s.probe_clusters; r.probe_packets+=s.probe_packets; r.probe_feedback_packets+=s.probe_feedback_packets;
  }
};
struct LayerSpec {
  uint32_t ssrc=0,rtx=0;
  std::string rid;
  int quality=2;
  bool operator==(const LayerSpec&) const = default;
};
struct Source : webrtc::RtpPacketSinkInterface {
  struct Layer {
    LayerSpec spec;
    uint32_t width=0,height=0;
    webrtc::Timestamp last_packet=webrtc::Timestamp::MinusInfinity(),last_frame=webrtc::Timestamp::MinusInfinity();
    std::unique_ptr<NativeRtpReceiver> receiver;
    std::unique_ptr<NativeVideoAssembler> assembler;
    webrtc::RateStatistics demand{2000,webrtc::RateStatistics::kBpsScale};
    webrtc::Timestamp first_sample=webrtc::Timestamp::MinusInfinity();
    std::set<int64_t> measured_frames;
    int64_t demand_bps(webrtc::Timestamp now,bool audio) const {
      int64_t fallback=audio ? 64000 : (width ? (uint64_t(width)*height<=320*240 ? 150000 :
        uint64_t(width)*height<=640*360 ? 450000 : 1200000) : (spec.quality==0 ? 150000 : spec.quality==1 ? 450000 : 1200000));
      if (first_sample.IsInfinite() || now-first_sample<webrtc::TimeDelta::Millis(500)) return fallback;
      return std::max<int64_t>(audio ? 64000 : 30000,demand.Rate(now.ms()).value_or(fallback));
    }
  };
  webrtc::Environment env;
  TrackRef track;
  Transport* transport=nullptr;
  std::vector<std::weak_ptr<Output>> outputs;
  std::shared_ptr<Counters> counters;
  NativeReceiveConfig config;
  std::map<uint32_t,std::unique_ptr<Layer>> layers;
  std::vector<LayerSpec> negotiated;
  std::map<std::string,int> rids;
  std::map<uint32_t,uint32_t> repairs;
  std::weak_ptr<ReceiveTransport> congestion;
  webrtc::RtpHeaderExtensionMap extensions;
  bool registered=false;
  uint64_t generation=0;
  Source(const webrtc::Environment& e,TrackRef t,std::shared_ptr<Counters> c) : env(e),track(std::move(t)),counters(std::move(c)) {}
  void OnRtpPacket(const webrtc::RtpPacketReceived& packet) override {
    auto identified=packet; identified.IdentifyExtensions(extensions);
    if (auto cc=congestion.lock()) cc->native.receive(identified,config.audio);
    uint32_t primary=identified.Ssrc();
    if (identified.PayloadType()==config.rtx_payload_type) {
      auto repair=repairs.find(primary);
      if (repair==repairs.end()) {
        std::string rid;
        if (identified.GetExtension<webrtc::RepairedRtpStreamId>(&rid) && rids.contains(rid))
          for (const auto& [ssrc,layer]:layers) if (layer->spec.rid==rid && !layer->spec.rtx) {
            layer->spec.rtx=primary; repairs[primary]=ssrc; break;
          }
        repair=repairs.find(primary);
      }
      if (repair==repairs.end()) { ++counters->unmapped_packets; return; }
      primary=repair->second;
    } else if (identified.PayloadType()==config.payload_type) {
      std::string rid; identified.GetExtension<webrtc::RtpStreamId>(&rid);
      if (!layers.contains(primary) && counters->unmapped_packets<5)
        RTC_LOG(LS_INFO)<<"SFU unknown primary "<<primary<<" rid="<<rid<<" negotiated-rids="<<rids.size()<<" known-layers="<<layers.size();
      if (!layers.contains(primary) && !config.audio && rids.contains(rid) && layers.size()<3) {
        bool bound=false;
        for (const auto& [_,layer]:layers) bound|=layer->spec.rid==rid;
        if (!bound) add_layer({primary,0,rid,rids.at(rid)});
      }
      if (auto it=layers.find(primary);it!=layers.end() && !rid.empty()) {
        if (!rids.contains(rid) || (!it->second->spec.rid.empty() && it->second->spec.rid!=rid)) {
          ++counters->unmapped_packets; return;
        }
        it->second->spec.rid=rid; it->second->spec.quality=rids.at(rid);
      }
    } else return;
    auto it=layers.find(primary);
    if (it==layers.end()) { ++counters->unmapped_packets; return; }
    it->second->last_packet=env.clock().CurrentTime();
    it->second->receiver->receive(identified);
  }
  void route(uint32_t,const webrtc::RtpPacketReceived&);
  void route_frame(uint32_t,const webrtc::EncodedFrame&);
  uint32_t choose(const VideoSubscription& settings) const {
    std::vector<VideoLayerChoice> choices;
    for (const auto& [ssrc,layer]:layers)
      if (env.clock().CurrentTime()-(layer->width ? layer->last_frame : layer->last_packet)<webrtc::TimeDelta::Seconds(2))
        choices.push_back({ssrc,layer->width,layer->height,layer->spec.quality});
    return select_video_layer(std::move(choices),settings);
  }
  std::vector<VideoLayerRate> rates(const VideoSubscription& settings) const {
    std::vector<VideoLayerRate> result;
    auto target=choose(settings); if (!target) return result;
    auto now=env.clock().CurrentTime();
    std::vector<const Layer*> ordered;
    for (const auto& [_,layer]:layers)
      if (now-(layer->width ? layer->last_frame : layer->last_packet)<webrtc::TimeDelta::Seconds(2)) ordered.push_back(layer.get());
    std::sort(ordered.begin(),ordered.end(),[](auto a,auto b) {
      return a->spec.quality!=b->spec.quality ? a->spec.quality<b->spec.quality : a->spec.ssrc<b->spec.ssrc;
    });
    for (auto layer:ordered) {
      result.push_back({layer->spec.ssrc,layer->demand_bps(now,false)});
      if (layer->spec.ssrc==target) break;
    }
    return result;
  }
  int64_t audio_demand() const {
    int64_t demand=64000;
    for (const auto& [_,layer]:layers) demand=std::max(demand,layer->demand_bps(env.clock().CurrentTime(),true));
    return demand;
  }
  void keyframe(uint32_t ssrc=0) {
    if (transport) for (auto& [id,layer]:layers) if (!ssrc || id==ssrc) layer->receiver->request_keyframe();
  }
  void add_layer(const LayerSpec& spec) {
    if (!spec.ssrc || layers.contains(spec.ssrc)) return;
    auto layer=std::make_unique<Layer>(); layer->spec=spec;
    auto receive_config=config; receive_config.ssrc=spec.ssrc;
    if (!config.audio) layer->assembler=std::make_unique<NativeVideoAssembler>([this,id=spec.ssrc](const auto& frame){route_frame(id,frame);});
    layer->receiver=std::make_unique<NativeRtpReceiver>(env,receive_config,
      [this,id=spec.ssrc](const auto& packet){route(id,packet);},[this](Bytes packet) {
        bool sent=send_packet(transport,packet,true);
        if (sent) rtcp_blocks(packet,[this](const auto& header) {
          if (header.type()==webrtc::rtcp::Nack::kPacketType && header.fmt()==webrtc::rtcp::Nack::kFeedbackMessageType) ++counters->nacks;
          if (header.type()==webrtc::rtcp::Pli::kPacketType && header.fmt()==webrtc::rtcp::Pli::kFeedbackMessageType) ++counters->plis;
        });
        return sent;
      });
    if (spec.rtx) repairs[spec.rtx]=spec.ssrc;
    layers[spec.ssrc]=std::move(layer); if (!config.audio) ++counters->layers_discovered;
  }
  void configure(const NativeReceiveConfig& next,const std::vector<LayerSpec>& specs,const std::map<std::string,int>& allowed_rids) {
    if (config==next && negotiated==specs && rids==allowed_rids) return;
    layers.clear(); repairs.clear(); config=next; negotiated=specs; rids=allowed_rids;
    ++generation;
    extensions=webrtc::RtpHeaderExtensionMap(config.extensions);
    RTC_LOG(LS_INFO)<<"SFU source config track="<<track->id()<<" layers="<<specs.size()<<" rids="<<rids.size();
    for (const auto& [rid,quality]:rids) RTC_LOG(LS_INFO)<<"SFU RID "<<rid<<" quality="<<quality;
    for (const auto& extension:config.extensions) RTC_LOG(LS_INFO)<<"SFU extension "<<extension.id<<" "<<extension.uri;
    for (const auto& spec:specs) add_layer(spec);
  }
};
struct Output : std::enable_shared_from_this<Output> {
  std::string sender_id;
  std::shared_ptr<Source> source;
  Transport* transport=nullptr;
  NativeSendConfig config;
  std::shared_ptr<SendTransport> paced;
  std::unique_ptr<NativeRtpSender> sender;
  bool enabled=true,started=false;
  VideoSubscription settings;
  VideoAllocationState allocation;
  int64_t allocated_bps=0;
  uint32_t active_layer=0;
  uint64_t source_generation=0;
  uint64_t subscription_order=0;
  void configure(const webrtc::Environment& env,const NativeSendConfig& next,std::shared_ptr<SendTransport> next_transport) {
    if (sender && config==next && paced==next_transport) return;
    sender.reset(); paced=std::move(next_transport);
    config=next; started=false; active_layer=0; source_generation=source->generation;
    RTC_LOG(LS_INFO)<<"SFU sender mid="<<config.mid<<" ssrc="<<config.ssrc
      <<" rtx="<<config.rtx_ssrc.value_or(0)<<" extensions="<<config.extensions.size();
    allocation={}; allocated_bps=0;
    paced->outputs[sender_id]=weak_from_this();
    sender=std::make_unique<NativeRtpSender>(env,config,[this](const auto& packet) {
      bool sent=send_packet(transport,{packet.data(),packet.size()},false,&packet);
      if (sent) {
        if (packet.packet_type()==webrtc::RtpPacketMediaType::kRetransmission) ++source->counters->retransmitted;
        else if (packet.packet_type()!=webrtc::RtpPacketMediaType::kPadding) ++source->counters->forwarded;
      } else ++source->counters->dropped;
      return sent;
    },[this]{if (allocation.selected) source->keyframe(allocation.selected);},paced->native);
    sender->set_enabled(enabled);
    if (!config.audio) sender->set_media_paused(true);
  }
  void forward(const webrtc::RtpPacketReceived& packet) {
    if (!enabled || !transport || !sender) return;
    paced->ready();
    if (!started) { started=true; source->keyframe(); }
    sender->forward(packet);
  }
  void sender_report(const webrtc::rtcp::SenderReport& report) {
    if (!enabled || !started || !sender) return;
    if (!config.audio && report.sender_ssrc()!=active_layer) return;
    auto bytes=sender->translate_report(report).Build();
    send_packet(transport,{bytes.data(),bytes.size()},true);
  }
  void forward_frame(uint32_t layer,const webrtc::EncodedFrame& frame) {
    if (!enabled || !transport || !sender) return;
    if (source_generation!=source->generation) {
      source_generation=source->generation; active_layer=0; started=false;
      sender->set_enabled(false); sender->set_enabled(true);
    }
    auto target=allocation.selected;
    if (!target) return;
    if (target!=active_layer && !(layer==target && frame.is_keyframe())) source->keyframe(target);
    bool switching=layer==target && (layer!=active_layer || !started) && frame.is_keyframe();
    // Keep the previous layer while waiting for the target's keyframe. A
    // subscriber's native sender verifies dependencies and rewrites frame IDs.
    if (!switching && (layer!=active_layer || !started)) return;
    paced->ready();
    if (sender->forward_frame(frame,layer)) {
      if (active_layer && active_layer!=layer) ++source->counters->layer_switches;
      active_layer=layer;
      started=true; ++source->counters->forwarded_frames;
    } else ++source->counters->gated_frames;
  }
  void apply_allocation(const VideoAllocationState& next,int64_t cost) {
    if (allocation.selected!=next.selected)
      RTC_LOG(LS_INFO)<<"SFU allocation mid="<<config.mid<<" selected="<<next.selected<<" cost="<<cost;
    if (allocation.selected && !next.selected) ++source->counters->bandwidth_pauses;
    allocation=next; allocated_bps=cost;
    if (!sender) return;
    if (!next.selected) { sender->set_media_paused(true); started=false; return; }
    auto active=source->layers.find(active_layer);
    // A downshift must also cancel already queued high-layer packets. The old
    // layer may continue during a switch only while it fits the new allocation.
    if (active_layer!=next.selected && (active==source->layers.end() ||
        active->second->demand_bps(source->env.clock().CurrentTime(),false)>cost)) {
      sender->set_media_paused(true); started=false;
    }
    sender->set_media_paused(false);
    if (active_layer!=next.selected || !started) source->keyframe(next.selected);
  }
};
SendTransport::SendTransport(const webrtc::Environment& e,std::shared_ptr<Counters> c,webrtc::TaskQueueBase* timer)
    : env(e),native(std::make_shared<NativeSendTransport>(env,4*1024*1024,timer)),counters(std::move(c)) {
  allocation_task=webrtc::RepeatingTaskHandle::Start(webrtc::TaskQueueBase::Current(),[this] {
    allocate(); return webrtc::TimeDelta::Millis(200);
  },webrtc::TaskQueueBase::DelayPrecision::kLow,&env.clock());
}
void SendTransport::allocate() {
  // Writable/ready callbacks may run before SRTP keys become active. Check
  // readiness independently of admitted media, otherwise a video-only
  // subscriber above the initial budget can never start native probing.
  ready();
  std::vector<VideoBandwidthRequest> requests;
  std::vector<std::shared_ptr<Output>> videos;
  int64_t audio=0;
  for (auto it=outputs.begin();it!=outputs.end();) {
    auto out=it->second.lock();
    if (!out || out->paced.get()!=this) {it=outputs.erase(it); continue;}
    ++it;
    if (!out->enabled || !out->transport) continue;
    if (out->config.audio) audio+=out->source->audio_demand();
    else { requests.push_back({out->sender_id,out->settings.priority,out->source->rates(out->settings),out->allocation,out->subscription_order}); videos.push_back(out); }
  }
  int64_t demand=audio; bool needs_growth=false;
  for (const auto& request:requests) if (!request.layers.empty()) {
    int64_t maximum=0;
    for (auto layer:request.layers) maximum=std::max(maximum,layer.bps);
    demand+=maximum; needs_growth|=request.state.selected!=request.layers.back().ssrc;
  }
  allocation=allocate_video_bandwidth(native->stats().target_bps,audio,env.clock().TimeInMilliseconds(),requests);
  // Preserve native ALR probing when admitted media is already well below
  // target. Filling all spare capacity with padding would conceal ALR and
  // leave a partially paused subscriber on slow additive growth instead.
  auto used=audio+allocation.video_allocated_bps;
  native->set_padding_demand(recovery_padding_demand(native->stats().target_bps,used,needs_growth ? demand : 0));
  for (size_t i=0;i<requests.size();++i) {
    int64_t cost=0;
    for (auto layer:requests[i].layers) if (layer.ssrc==requests[i].state.selected) cost=layer.bps;
    videos[i]->apply_allocation(requests[i].state,cost);
  }
}
void Source::route(uint32_t id,const webrtc::RtpPacketReceived& packet) {
  ++counters->received;
  auto& layer=*layers.at(id);
  if (layer.assembler) {
    layer.assembler->receive(packet);
    // Native loss recovery may run out of usable references after burst loss.
    // Ask the existing native RTCP module for a keyframe; its throttle applies.
    if (packet.Marker() && env.clock().CurrentTime()-layer.last_frame>webrtc::TimeDelta::Millis(500)) keyframe(id);
    return;
  }
  if (packet.payload_size()) {
    auto now=env.clock().CurrentTime(); if (layer.first_sample.IsInfinite()) layer.first_sample=now;
    layer.demand.Update(int64_t(packet.payload_size()+80),now.ms());
  }
  for (auto it=outputs.begin();it!=outputs.end();) {
    if (auto output=it->lock()) { output->forward(packet); ++it; }
    else it=outputs.erase(it);
  }
}
void Source::route_frame(uint32_t id,const webrtc::EncodedFrame& frame) {
  auto& layer=*layers.at(id);
  ++counters->assembled_frames;
  // RTX/payload padding can make native assembly emit an already seen frame.
  // Only unique complete encoded frames contribute to media demand/activity.
  if (layer.measured_frames.insert(frame.Id()).second) {
    auto now=env.clock().CurrentTime(); layer.last_frame=now;
    if (layer.first_sample.IsInfinite()) layer.first_sample=now;
    layer.demand.Update(int64_t(frame.size()+((frame.size()+1099)/1100)*80),now.ms());
    while (layer.measured_frames.size()>512) layer.measured_frames.erase(layer.measured_frames.begin());
  }
  if (frame._encodedWidth && frame._encodedHeight) { layer.width=frame._encodedWidth; layer.height=frame._encodedHeight; }
  for (auto it=outputs.begin();it!=outputs.end();) {
    if (auto output=it->lock()) {
      // Discovery can make a base layer affordable on this very keyframe.
      // Allocate now rather than discarding it until the next 200 ms tick,
      // which would also incur another upstream PLI/keyframe round trip.
      if (frame.is_keyframe() && output->enabled && output->paced && !output->allocation.initialized)
        output->paced->allocate();
      output->forward_frame(id,frame); ++it;
    }
    else it=outputs.erase(it);
  }
}
struct Snapshot {
  webrtc::BaseChannel* channel=nullptr;
  std::string mid,sender_id;
  TrackRef track;
  webrtc::RtpParameters parameters;
  std::vector<webrtc::StreamParams> streams;
};
int primary_codec(const webrtc::RtpParameters& p,bool audio) {
  for (const auto& codec:p.codecs)
    if (lower(codec.name)==(audio ? "opus" : "vp8")) return codec.payload_type;
  return -1;
}
int rtx_codec(const webrtc::RtpParameters& p,int primary) {
  for (const auto& codec:p.codecs) if (lower(codec.name)=="rtx") {
    auto apt=codec.parameters.find("apt");
    if (apt!=codec.parameters.end() && apt->second==std::to_string(primary)) return codec.payload_type;
  }
  return -1;
}
}
struct SfuRouter::Impl {
  webrtc::Thread *signaling,*network,*worker;
#ifdef _WIN32
  webrtc::Environment env=webrtc::CreateEnvironment(webrtc::CreateTaskQueueWinFactory());
#else
  webrtc::Environment env=webrtc::CreateEnvironment();
#endif
  std::unique_ptr<webrtc::TaskQueueBase,webrtc::TaskQueueDeleter> pacer_timer=
      env.task_queue_factory().CreateTaskQueue("sfu-pacer-timer",webrtc::TaskQueueFactory::Priority::HIGH);
  std::shared_ptr<Counters> counters=std::make_shared<Counters>();
  std::map<const webrtc::MediaStreamTrackInterface*,std::weak_ptr<Source>> sources;
  std::vector<std::weak_ptr<SendTransport>> senders;
  std::shared_ptr<Source> source(TrackRef track) {
    for (auto it=sources.begin();it!=sources.end();) {
      if (it->second.expired()) it=sources.erase(it); else ++it;
    }
    auto& weak=sources[track.get()];
    if (auto existing=weak.lock()) return existing;
    auto created=std::make_shared<Source>(env,std::move(track),counters); weak=created; return created;
  }
};
struct SfuSession::State : std::enable_shared_from_this<State> {
  std::shared_ptr<SfuRouter::Impl> router;
  bool subscriber;
  std::map<std::string,std::shared_ptr<Output>> outputs;
  std::vector<std::shared_ptr<Source>> inputs;
  // Transport ownership of a weak observer token prevents callback accumulation
  // across renegotiation, including allocation of a new transport at an old address.
  std::map<Transport*,std::weak_ptr<int>> observers;
  std::map<Transport*,std::shared_ptr<ReceiveTransport>> receive_transports;
  std::map<Transport*,std::shared_ptr<SendTransport>> send_transports;
  void detach() {
    for (auto& input:inputs) {
      if (input->registered && input->transport) input->transport->UnregisterRtpDemuxerSink(input.get());
      input->registered=false; input->transport=nullptr;
    }
    for (auto& [_,output]:outputs) output->transport=nullptr;
    for (auto& [_,receiver]:receive_transports) receiver->transport=nullptr;
    for (auto& [_,sender]:send_transports) sender->unbind();
  }
  void rtcp(Transport* transport,Bytes bytes) {
    if (subscriber) {
      if (auto it=send_transports.find(transport);it!=send_transports.end()) it->second->native->feedback(bytes);
      for (auto& [_,out]:outputs) if (out->transport==transport && out->sender) out->sender->feedback(bytes);
      return;
    }
    for (auto& input:inputs) if (input->transport==transport)
      for (auto& [_,layer]:input->layers) layer->receiver->feedback(bytes);
    rtcp_blocks(bytes,[&](const auto& header) {
      if (header.type()!=webrtc::rtcp::SenderReport::kPacketType) return;
      webrtc::rtcp::SenderReport report;
      if (!report.Parse(header)) return;
      for (auto& input:inputs) if (input->transport==transport && input->layers.contains(report.sender_ssrc()))
        for (auto& weak:input->outputs) if (auto out=weak.lock()) out->sender_report(report);
    });
  }
  void bind(const std::vector<Snapshot>& snapshots) {
    detach();
    for (auto it=observers.begin();it!=observers.end();) {
      if (it->second.expired()) it=observers.erase(it); else ++it;
    }
    std::set<Transport*> observed;
    std::vector<std::shared_ptr<Source>> active_inputs;
    for (const auto& s:snapshots) {
      auto* transport=s.channel->rtp_transport(); if (!transport) continue;
      // PeerConnection supplies negotiation and ICE/DTLS/SRTP, while the SFU
      // owns RTP/RTCP through its native sender/receiver modules. Enable(false)
      // stops playout/encoding but does not stop the default media channels'
      // periodic RTCP (including a second transport-wide feedback generator).
      // Remove their send interfaces on the network thread. Native SetInterface
      // also invalidates already posted sends; keep the BaseChannel transport
      // itself attached so SDP negotiation and transport lifecycle still work.
      s.channel->media_send_channel()->SetInterface(nullptr);
      s.channel->media_receive_channel()->SetInterface(nullptr);
      uint32_t ssrc=s.parameters.encodings.empty() ? 0 : s.parameters.encodings.front().ssrc.value_or(0);
      if (subscriber && !ssrc) continue;
      if (subscriber) {
        auto& sender=send_transports[transport];
        if (!sender || observers[transport].expired()) {
          sender=std::make_shared<SendTransport>(router->env,router->counters,router->pacer_timer.get());
          router->senders.push_back(sender);
        }
        sender->bind(transport);
        for (auto& [_,out]:outputs) if (out->sender_id==s.sender_id) {
          NativeSendConfig config;
          config.audio=out->source->track->kind()=="audio";
          config.payload_type=primary_codec(s.parameters,config.audio);
          if (config.payload_type<0) continue;
          config.ssrc=ssrc; config.mid=s.mid; config.extensions=s.parameters.header_extensions;
          config.rtx_payload_type=rtx_codec(s.parameters,config.payload_type);
          for (const auto& stream:s.streams) {
            uint32_t rtx=0; if (stream.GetFidSsrc(ssrc,&rtx)) config.rtx_ssrc=rtx;
          }
          out->configure(router->env,config,sender); out->transport=transport; out->source->keyframe();
        }
      } else {
        auto& receiver=receive_transports[transport];
        if (!receiver || observers[transport].expired()) receiver=std::make_shared<ReceiveTransport>(router->env,transport);
        receiver->transport=transport;
        auto input=router->source(s.track);
        input->congestion=receiver;
        NativeReceiveConfig config;
        config.audio=s.track->kind()=="audio";
        config.payload_type=primary_codec(s.parameters,config.audio);
        if (config.payload_type<0) continue;
        config.rtx_payload_type=rtx_codec(s.parameters,config.payload_type);
        config.extensions=s.parameters.header_extensions;
        std::vector<LayerSpec> specs;
        std::vector<std::string> rid_order;
        for (const auto& stream:s.streams) {
          for (const auto& rid:stream.rids()) if (std::find(rid_order.begin(),rid_order.end(),rid.rid)==rid_order.end()) rid_order.push_back(rid.rid);
          std::vector<uint32_t> primaries; stream.GetPrimarySsrcs(&primaries);
          for (auto primary:primaries) {
            if (!primary) continue;
            uint32_t repair=0; stream.GetFidSsrc(primary,&repair);
            specs.push_back({primary,repair,{}});
          }
        }
        if (specs.empty() && ssrc && rid_order.empty()) specs.push_back({ssrc,0,{}});
        if (specs.size()>3 || rid_order.size()>3) throw std::runtime_error("SFU supports at most three simulcast layers");
        size_t layer_count=std::max(specs.size(),rid_order.size());
        std::map<std::string,int> allowed_rids;
        for (size_t i=0;i<rid_order.size();++i) allowed_rids[rid_order[i]]=layer_count<2 ? 2 : int(i*2/(layer_count-1));
        for (size_t i=0;i<specs.size();++i) {
          specs[i].quality=layer_count<2 ? 2 : int(i*2/(layer_count-1));
          if (i<rid_order.size()) specs[i].rid=rid_order[i];
        }
        input->configure(config,specs,allowed_rids);
        webrtc::RtpDemuxerCriteria criteria(s.mid);
        for (const auto& stream:s.streams) for (auto stream_ssrc:stream.ssrcs) criteria.ssrcs().insert(stream_ssrc);
        if (ssrc) criteria.ssrcs().insert(ssrc);
        transport->UnregisterRtpDemuxerSink(s.channel);
        if (!transport->RegisterRtpDemuxerSink(criteria,input.get())) throw std::runtime_error("Cannot bind SFU RTP sink");
        input->transport=transport; input->registered=true; active_inputs.push_back(input);
      }
      if (observed.insert(transport).second && observers[transport].expired()) {
        auto token=std::make_shared<int>(0); observers[transport]=token;
        std::weak_ptr<State> weak=shared_from_this();
        transport->SubscribeRtcpPacketReceived(token.get(),[weak,token,transport](webrtc::CopyOnWriteBuffer* bytes,int64_t) {
          if (auto state=weak.lock()) state->rtcp(transport,{bytes->cdata(),bytes->size()});
        });
      }
    }
    if (!subscriber) inputs=std::move(active_inputs);
    for (auto it=receive_transports.begin();it!=receive_transports.end();) {
      if (!observed.contains(it->first)) it=receive_transports.erase(it); else ++it;
    }
    for (auto it=send_transports.begin();it!=send_transports.end();) {
      if (!observed.contains(it->first)) it=send_transports.erase(it); else ++it;
    }
  }
};
SfuRouter::SfuRouter(webrtc::Thread* signaling,webrtc::Thread* network,webrtc::Thread* worker) : impl_(std::make_shared<Impl>()) {
  impl_->signaling=signaling; impl_->network=network; impl_->worker=worker;
}
SfuRouter::~SfuRouter()=default;
Json SfuRouter::metrics() const {
  return impl_->network->BlockingCall([&] {
    auto& c=*impl_->counters;
    auto paced=c.retired;
    MediaAllocation allocation;
    for (auto it=impl_->senders.begin();it!=impl_->senders.end();) {
      if (auto sender=it->lock()) {
        auto s=sender->native->stats(); paced.sent+=s.sent; paced.padding+=s.padding; paced.feedback+=s.feedback;
        allocation.media_budget_bps+=sender->allocation.media_budget_bps;
        allocation.audio_reserved_bps+=sender->allocation.audio_reserved_bps;
        allocation.video_allocated_bps+=sender->allocation.video_allocated_bps;
        allocation.paused+=sender->allocation.paused;
        paced.feedback_packets+=s.feedback_packets; paced.dropped+=s.dropped;
        paced.queue_dropped+=s.queue_dropped;
        paced.probe_clusters+=s.probe_clusters; paced.probe_packets+=s.probe_packets;
        paced.probe_feedback_packets+=s.probe_feedback_packets;
        paced.target_bps+=s.target_bps; paced.pacing_bps+=s.pacing_bps; paced.queue_bytes+=s.queue_bytes;
        paced.queue_ms=std::max(paced.queue_ms,s.queue_ms); ++it;
      } else it=impl_->senders.erase(it);
    }
    return Json{{"sfu_rtp_received",c.received},{"sfu_rtp_forwarded",c.forwarded},
      {"sfu_rtp_retransmitted",c.retransmitted},{"sfu_upstream_nacks",c.nacks},
      {"sfu_upstream_plis",c.plis},{"sfu_rtp_dropped",c.dropped},
      {"sfu_video_frames_assembled",c.assembled_frames},{"sfu_video_frames_forwarded",c.forwarded_frames},
      {"sfu_video_frames_gated",c.gated_frames},
      {"sfu_layers_discovered",c.layers_discovered},{"sfu_layer_switches",c.layer_switches},
      {"sfu_unmapped_packets",c.unmapped_packets},
      {"sfu_media_budget_bps",allocation.media_budget_bps},{"sfu_audio_reserved_bps",allocation.audio_reserved_bps},
      {"sfu_video_allocated_bps",allocation.video_allocated_bps},{"sfu_video_budget_paused",allocation.paused},
      {"sfu_bandwidth_pauses",c.bandwidth_pauses},
      {"sfu_paced_sent",paced.sent},{"sfu_padding_sent",paced.padding},
      {"sfu_probe_clusters",paced.probe_clusters},{"sfu_probe_packets",paced.probe_packets},
      {"sfu_probe_feedback_packets",paced.probe_feedback_packets},
      {"sfu_downstream_feedback",paced.feedback},{"sfu_feedback_packets",paced.feedback_packets},
      {"sfu_target_bps",paced.target_bps},{"sfu_pacing_bps",paced.pacing_bps},
      {"sfu_pacer_queue_bytes",paced.queue_bytes},{"sfu_pacer_queue_ms",paced.queue_ms},
      {"sfu_pacer_admission_drops",paced.queue_dropped}};
  });
}
SfuSession::SfuSession(SfuRouter& router,bool subscriber) : state_(std::make_shared<State>()) {
  state_->router=router.impl_; state_->subscriber=subscriber;
}
SfuSession::~SfuSession() {
  detach(); state_->router->network->BlockingCall([&] {
    state_->outputs.clear(); state_->inputs.clear(); state_->receive_transports.clear(); state_->send_transports.clear();
  });
}
void SfuSession::add(const std::string& sid,const std::string& sender_id,TrackRef track) {
  state_->router->network->BlockingCall([&] {
    if (state_->outputs.contains(sid)) return;
    auto out=std::make_shared<Output>(); out->sender_id=sender_id;
    out->subscription_order=++state_->router->counters->next_subscription;
    out->source=state_->router->source(track); out->source->outputs.push_back(out); state_->outputs[sid]=out;
  });
}
void SfuSession::remove(const std::string& sid) {
  state_->router->network->BlockingCall([&] { state_->outputs.erase(sid); });
}
void SfuSession::enable(const std::string& sid,bool enabled) {
  state_->router->network->BlockingCall([&] {
    auto it=state_->outputs.find(sid); if (it==state_->outputs.end() || it->second->enabled==enabled) return;
    auto& out=*it->second; out.enabled=enabled; out.started=false;
    if (out.sender) out.sender->set_enabled(enabled);
    if (out.paced) out.paced->allocate();
    if (enabled) out.source->keyframe();
  });
}
void SfuSession::settings(const std::string& sid,const VideoSubscription& settings) {
  state_->router->network->BlockingCall([&] {
    auto it=state_->outputs.find(sid); if (it==state_->outputs.end()) return;
    auto& out=*it->second;
    if (out.settings==settings) return;
    out.settings=settings;
    if (out.paced) out.paced->allocate();
  });
}
void SfuSession::detach() { state_->router->network->BlockingCall([&] { state_->detach(); }); }
void SfuSession::prepare_answer(webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,webrtc::SessionDescriptionInterface& answer) {
  if (state_->subscriber) return;
  state_->router->signaling->BlockingCall([&] {
    if (!pc->remote_description()) return;
    for (const auto& offered:pc->remote_description()->description()->contents()) {
      const auto* remote=offered.media_description();
      auto* accepted=answer.description()->GetContentByName(offered.mid());
      if (!remote || remote->type()!=webrtc::MediaType::VIDEO || !remote->HasSimulcast() || !accepted || accepted->rejected) continue;
      auto* local=accepted->media_description();
      if (local->direction()!=webrtc::RtpTransceiverDirection::kRecvOnly && local->direction()!=webrtc::RtpTransceiverDirection::kSendRecv) continue;
      // M137's ordinary PeerConnection only negotiates send simulcast. This
      // SFU explicitly accepts the offered receive layers using native JSEP
      // objects and the native serializer, leaving ICE/DTLS/codec negotiation intact.
      const auto& send_layers=remote->simulcast_description().send_layers();
      if (send_layers.empty()) continue;
      if (send_layers.size()>3) throw std::runtime_error("At most three simulcast layers are supported");
      std::vector<webrtc::RidDescription> rids;
      for (const auto& alternatives:send_layers) {
        if (alternatives.size()!=1) throw std::runtime_error("Simulcast RID alternatives are not supported");
        bool found=false;
        for (const auto& stream:remote->streams()) for (auto rid:stream.rids()) if (rid.rid==alternatives.front().rid) {
          rid.direction=webrtc::RidDirection::kReceive; rids.push_back(std::move(rid)); found=true;
        }
        if (!found) throw std::runtime_error("Simulcast layer has no negotiated RID");
      }
      local->set_receive_rids(rids);
      local->simulcast_description().receive_layers()=send_layers;
    }
  });
}
void SfuSession::refresh(webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {
  std::vector<Snapshot> snapshots;
  for (auto& transceiver:pc->GetTransceivers()) {
    Snapshot s; s.mid=transceiver->mid().value_or("");
    if (s.mid.empty() || transceiver->stopped()) continue;
    s.sender_id=transceiver->sender()->id(); s.track=transceiver->receiver()->track();
    s.parameters=state_->subscriber ? transceiver->sender()->GetParameters() : transceiver->receiver()->GetParameters();
    s.channel=state_->router->signaling->BlockingCall([&] {
      // RID-only receivers have no native receive stream yet, and M137's
      // default receiver parameters omit the negotiated extension map. Read
      // the accepted native JSEP content, not a handwritten SDP parser.
      if (!state_->subscriber && pc->local_description()) {
        const auto* content=pc->local_description()->description()->GetContentByName(s.mid);
        if (content && content->media_description())
          s.parameters.header_extensions=content->media_description()->rtp_header_extensions();
      }
      // The pinned M137 PeerConnection returns exactly this proxy type. Keep
      // version-dependent access inside this single adapter translation unit.
      auto* proxy=static_cast<webrtc::RtpTransceiverProxyWithInternal<webrtc::RtpTransceiver>*>(transceiver.get());
      auto* channel=static_cast<webrtc::BaseChannel*>(proxy->internal()->channel());
      if (channel) channel->Enable(false);
      return channel;
    });
    if (s.channel) {
      // StreamParams are worker-thread owned; snapshot their FID associations.
      state_->router->worker->BlockingCall([&] {
        s.streams=state_->subscriber ? s.channel->local_streams() : s.channel->remote_streams();
      });
      snapshots.push_back(std::move(s));
    }
  }
  state_->router->network->BlockingCall([&] { state_->bind(snapshots); });
}
}
