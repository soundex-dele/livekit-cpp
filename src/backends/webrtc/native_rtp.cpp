#include "lk/native_rtp.hpp"
#include <api/call/transport.h>
#include <api/video/rtp_video_frame_assembler.h>
#include <call/rtx_receive_stream.h>
#include <modules/rtp_rtcp/include/receive_statistics.h>
#include <modules/rtp_rtcp/source/rtp_header_extensions.h>
#include <modules/rtp_rtcp/source/rtp_packet_history.h>
#include <modules/rtp_rtcp/source/rtp_sender.h>
#include <modules/rtp_rtcp/source/rtcp_receiver.h>
#include <modules/rtp_rtcp/source/rtp_rtcp_impl2.h>
#include <modules/rtp_rtcp/source/packet_sequencer.h>
#include <modules/rtp_rtcp/source/rtp_format_vp8.h>
#include <modules/video_coding/nack_requester.h>
#include <modules/congestion_controller/include/receive_side_congestion_controller.h>
#include <modules/rtp_rtcp/source/rtcp_packet/remb.h>
#include <rtc_base/task_utils/repeating_task.h>
#include <rtc_base/rate_limiter.h>
#include <rtc_base/crypto_random.h>
#include <rtc_base/logging.h>
#include <rtc_base/numerics/sequence_number_util.h>
#include <modules/include/module_common_types_public.h>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <deque>
#include <set>

namespace lk {
struct NativeReceiveTransport::Impl {
  Rtcp send;
  uint32_t feedback_ssrc=webrtc::CreateRandomNonZeroId();
  webrtc::ReceiveSideCongestionController controller;
  webrtc::RepeatingTaskHandle task;
  Impl(const webrtc::Environment& env,Rtcp callback)
      : send(std::move(callback)),controller(env,
        [this](std::vector<std::unique_ptr<webrtc::rtcp::RtcpPacket>> packets) {
          for (const auto& packet:packets) { auto b=packet->Build(); send({b.data(),b.size()}); }
        },[this](int64_t bitrate,std::vector<uint32_t> ssrcs) {
          webrtc::rtcp::Remb remb; remb.SetSenderSsrc(feedback_ssrc);
          remb.SetBitrateBps(bitrate); remb.SetSsrcs(std::move(ssrcs));
          auto b=remb.Build(); send({b.data(),b.size()});
        }) {
    task=webrtc::RepeatingTaskHandle::Start(webrtc::TaskQueueBase::Current(),[this]{return controller.MaybeProcess();},
      webrtc::TaskQueueBase::DelayPrecision::kLow,&env.clock());
  }
  ~Impl() { task.Stop(); }
};
NativeReceiveTransport::NativeReceiveTransport(const webrtc::Environment& env,Rtcp send)
    : impl_(std::make_unique<Impl>(env,std::move(send))) {}
NativeReceiveTransport::~NativeReceiveTransport()=default;
void NativeReceiveTransport::receive(const webrtc::RtpPacketReceived& packet,bool audio) {
  impl_->controller.OnReceivedPacket(packet,audio ? webrtc::MediaType::AUDIO : webrtc::MediaType::VIDEO);
}
webrtc::TimeDelta NativeReceiveTransport::process() { return impl_->controller.MaybeProcess(); }

struct NativeRtpSender::Impl : webrtc::RtpPacketSender,
    webrtc::RTCPReceiver::ModuleRtpRtcp,webrtc::RtcpIntraFrameObserver {
  webrtc::Environment env;
  NativeSendConfig config;
  Send send;
  std::function<void()> keyframe;
  std::shared_ptr<NativeSendTransport> transport;
  webrtc::RtpPacketHistory history;
  webrtc::RateLimiter limiter;
  webrtc::PacketSequencer sequencer;
  std::unique_ptr<webrtc::RTPSender> sender;
  std::unique_ptr<webrtc::RTCPReceiver> receiver;
  bool enabled=true,media_paused=false;
  std::optional<uint16_t> offset;
  uint16_t next_sequence;
  uint32_t packets=0,octets=0;
  uint32_t frame_source=0,frame_timestamp_offset=0;
  std::optional<uint32_t> last_frame_timestamp;
  webrtc::Timestamp last_frame_time=webrtc::Timestamp::MinusInfinity();
  uint16_t picture_id=0;
  std::set<int64_t> admitted_frames;
  bool needs_keyframe=true;
  bool batch_admitted=true;
  int frame_diagnostics=0;
  Impl(const webrtc::Environment& e,NativeSendConfig c,Send s,std::function<void()> k,std::shared_ptr<NativeSendTransport> t)
      : env(e),config(std::move(c)),send(std::move(s)),keyframe(std::move(k)),transport(std::move(t)),
        history(env,webrtc::RtpPacketHistory::PaddingMode::kDefault),
        limiter(&env.clock(),1000),sequencer(config.ssrc,config.rtx_ssrc,!config.audio,&env.clock()),
        next_sequence(sequencer.media_sequence_number()) {
    if (!config.ssrc || config.payload_type<0 || config.payload_type>127) throw std::invalid_argument("Invalid SFU sender configuration");
    history.SetStorePacketsStatus(webrtc::RtpPacketHistory::StorageMode::kStoreAndCull,512);
    limiter.SetMaxRate(2000000);
    webrtc::RtpRtcpInterface::Configuration native;
    native.audio=config.audio; native.local_media_ssrc=config.ssrc;
    native.rtx_send_ssrc=config.rtx_ssrc; native.extmap_allow_mixed=config.extmap_allow_mixed;
    native.always_send_mid_and_rid=true; native.intra_frame_callback=this;
    native.retransmission_rate_limiter=&limiter;
    sender=std::make_unique<webrtc::RTPSender>(env,native,&history,this);
    receiver=std::make_unique<webrtc::RTCPReceiver>(env,native,this);
    sender->SetMaxRtpPacketSize(2000);
    for (const auto& ext:config.extensions) {
      // Hop-specific identifiers are populated by the shared send transport.
      if (ext.uri==webrtc::RtpExtension::kMidUri || ext.uri==webrtc::RtpExtension::kAudioLevelUri ||
          ext.uri==webrtc::RtpExtension::kVideoRotationUri || (transport &&
          (ext.uri==webrtc::RtpExtension::kTransportSequenceNumberUri || ext.uri==webrtc::RtpExtension::kAbsSendTimeUri)))
        sender->RegisterRtpHeaderExtension(ext.uri,ext.id);
    }
    sender->SetMid(config.mid);
    if (config.rtx_ssrc && config.rtx_payload_type>=0) {
      sender->SetRtxPayloadType(config.rtx_payload_type,config.payload_type);
      sender->SetRtxStatus(webrtc::kRtxRetransmitted);
    }
    if (transport) transport->attach(config.ssrc,config.rtx_ssrc,{
      [this](auto& packet) {
        if (packet.packet_type()==webrtc::RtpPacketMediaType::kRetransmission ||
            packet.packet_type()==webrtc::RtpPacketMediaType::kPadding) sequencer.Sequence(packet);
      },
      [this](const auto& packet){return enabled && (!media_paused || packet.packet_type()==webrtc::RtpPacketMediaType::kPadding) && send(packet);},
      [this](auto packet,bool sent){complete(std::move(packet),sent);},
      [this](size_t size) {
        std::vector<std::unique_ptr<webrtc::RtpPacketToSend>> padding;
        if (enabled && !config.audio && config.rtx_ssrc)
          padding=sender->GeneratePadding(size,packets>0,false);
        return padding;
      },[this](webrtc::ArrayView<const uint16_t> seq) {
        for (auto number:seq) history.MarkPacketAsSent(number);
      }});
  }
  ~Impl() { if (transport) transport->detach(config.ssrc,config.rtx_ssrc); }
  void complete(std::unique_ptr<webrtc::RtpPacketToSend> packet,bool sent) {
    if (packet->packet_type()==webrtc::RtpPacketMediaType::kRetransmission)
      history.MarkPacketAsSent(*packet->retransmitted_sequence_number());
    else if (packet->packet_type()!=webrtc::RtpPacketMediaType::kPadding) {
      if (sent) { ++packets; octets+=uint32_t(packet->payload_size()); }
      history.PutRtpPacket(std::move(packet),env.clock().CurrentTime());
    }
  }
  void EnqueuePackets(std::vector<std::unique_ptr<webrtc::RtpPacketToSend>> batch) override {
    for (auto& packet:batch) {
      bool retransmit=packet->packet_type()==webrtc::RtpPacketMediaType::kRetransmission;
      if (retransmit && !transport) sequencer.Sequence(*packet);
      if (!transport) {
        bool sent=enabled && send(*packet); complete(std::move(packet),sent);
      }
    }
    if (transport) batch_admitted=transport->enqueue(std::move(batch));
  }
  void SetTmmbn(std::vector<webrtc::rtcp::TmmbItem>) override {}
  void OnRequestSendReport() override {}
  void OnReceivedNack(const std::vector<uint16_t>& sequences) override {
    if (transport) limiter.SetMaxRate(uint32_t(std::max<int64_t>(30000,transport->stats().target_bps)));
    if (enabled && !media_paused) sender->OnReceivedNack(sequences,receiver->AverageRtt().value_or(webrtc::TimeDelta::Millis(100)).ms());
  }
  void OnReceivedRtcpReportBlocks(webrtc::ArrayView<const webrtc::ReportBlockData>) override {}
  void OnReceivedIntraFrameRequest(uint32_t ssrc) override {
    if (enabled && ssrc==config.ssrc && keyframe) keyframe();
  }
  void forward(const webrtc::RtpPacketReceived& received) {
    if (!enabled || media_paused || received.payload_size()==0) return;
    if (!offset) offset=uint16_t(next_sequence-received.SequenceNumber());
    auto packet=sender->AllocatePacket(received.Csrcs());
    packet->SetPayloadType(uint8_t(config.payload_type));
    packet->SetSequenceNumber(uint16_t(received.SequenceNumber()+*offset));
    packet->SetTimestamp(received.Timestamp()); packet->SetMarker(received.Marker());
    packet->set_packet_type(config.audio ? webrtc::RtpPacketMediaType::kAudio : webrtc::RtpPacketMediaType::kVideo);
    packet->set_allow_retransmission(true); packet->set_capture_time(env.clock().CurrentTime());
    if (auto extension=received.GetRawExtension<webrtc::AudioLevelExtension>(); !extension.empty())
      packet->SetRawExtension<webrtc::AudioLevelExtension>(extension);
    if (auto extension=received.GetRawExtension<webrtc::VideoOrientation>(); !extension.empty())
      packet->SetRawExtension<webrtc::VideoOrientation>(extension);
    if (received.payload_size()>packet->FreeCapacity()) return;
    std::memcpy(packet->AllocatePayload(received.payload_size()),received.payload().data(),received.payload_size());
    if (packet->SequenceNumber()==next_sequence || webrtc::IsNewerSequenceNumber(packet->SequenceNumber(),next_sequence))
      next_sequence=uint16_t(packet->SequenceNumber()+1);
    std::vector<std::unique_ptr<webrtc::RtpPacketToSend>> batch;
    batch.push_back(std::move(packet)); sender->EnqueuePackets(std::move(batch));
  }
  bool forward_frame(const webrtc::EncodedFrame& frame,uint32_t source_ssrc) {
    if (!enabled || media_paused || config.audio || !frame.size() || !source_ssrc) return false;
    bool switching=frame_source!=source_ssrc;
    if (!frame.is_keyframe()) {
      bool missing=needs_keyframe || switching || frame.num_references>webrtc::EncodedFrame::kMaxFrameReferences;
      for (size_t i=0; !missing && i<frame.num_references; ++i)
        missing=!admitted_frames.contains(frame.references[i]);
      if (missing) {
        if (frame_diagnostics<8) { ++frame_diagnostics; RTC_LOG(LS_INFO)<<"SFU frame gate missing dependency source="<<source_ssrc<<" id="<<frame.Id()<<" switching="<<switching; }
        if (keyframe) keyframe(); return false;
      }
    }
    if (!switching && admitted_frames.contains(frame.Id())) {
      if (frame_diagnostics<8) { ++frame_diagnostics; RTC_LOG(LS_INFO)<<"SFU frame gate duplicate source="<<source_ssrc<<" id="<<frame.Id(); }
      return false;
    }
    auto now=env.clock().CurrentTime();
    uint32_t timestamp_offset=frame_timestamp_offset;
    if (switching) {
      timestamp_offset=0;
      if (last_frame_timestamp && (!webrtc::IsNewerTimestamp(frame.RtpTimestamp(),*last_frame_timestamp) ||
          uint32_t(frame.RtpTimestamp()-*last_frame_timestamp)>450000)) {
        auto elapsed=std::clamp<int64_t>((now-last_frame_time).us()*90/1000,1,90000);
        timestamp_offset=uint32_t(*last_frame_timestamp+elapsed-frame.RtpTimestamp());
      }
    }
    uint32_t timestamp=frame.RtpTimestamp()+timestamp_offset;
    if (last_frame_timestamp && !webrtc::IsNewerTimestamp(timestamp,*last_frame_timestamp)) {
      if (frame_diagnostics<8) { ++frame_diagnostics; RTC_LOG(LS_INFO)<<"SFU frame gate timestamp source="<<source_ssrc<<" timestamp="<<timestamp<<" last="<<*last_frame_timestamp<<" offset="<<timestamp_offset; }
      return false;
    }
    webrtc::RTPVideoHeaderVP8 header; header.InitRTPVideoHeaderVP8();
    header.pictureId=int16_t(picture_id & 0x7fff);
    webrtc::RtpPacketizer::PayloadSizeLimits limits; limits.max_payload_len=1100;
    webrtc::RtpPacketizerVp8 packetizer({frame.data(),frame.size()},limits,header);
    std::vector<std::unique_ptr<webrtc::RtpPacketToSend>> batch;
    auto sequence=next_sequence;
    while (packetizer.NumPackets()) {
      auto packet=sender->AllocatePacket({});
      packet->SetPayloadType(uint8_t(config.payload_type)); packet->SetTimestamp(timestamp);
      packet->SetSequenceNumber(sequence++);
      packet->set_packet_type(webrtc::RtpPacketMediaType::kVideo);
      packet->set_allow_retransmission(true); packet->set_capture_time(now);
      packet->set_is_key_frame(frame.is_keyframe());
      packet->SetExtension<webrtc::VideoOrientation>(frame.rotation());
      if (!packetizer.NextPacket(packet.get())) return false;
      batch.push_back(std::move(packet));
    }
    if (batch.empty()) return false;
    batch_admitted=true;
    sender->EnqueuePackets(std::move(batch));
    if (!batch_admitted) {
      if (!switching) needs_keyframe=true;
      if (keyframe) keyframe();
      return false;
    }
    next_sequence=sequence; ++picture_id;
    if (switching || frame.is_keyframe()) admitted_frames.clear();
    admitted_frames.insert(frame.Id());
    while (admitted_frames.size()>512) admitted_frames.erase(admitted_frames.begin());
    needs_keyframe=false; frame_source=source_ssrc; frame_timestamp_offset=timestamp_offset;
    last_frame_timestamp=timestamp; last_frame_time=now;
    return true;
  }
};
NativeRtpSender::NativeRtpSender(const webrtc::Environment& env,NativeSendConfig config,Send send,std::function<void()> keyframe,
    std::shared_ptr<NativeSendTransport> transport)
    : impl_(std::make_unique<Impl>(env,std::move(config),std::move(send),std::move(keyframe),std::move(transport))) {}
NativeRtpSender::~NativeRtpSender()=default;
void NativeRtpSender::forward(const webrtc::RtpPacketReceived& packet) { impl_->forward(packet); }
bool NativeRtpSender::forward_frame(const webrtc::EncodedFrame& frame,uint32_t source_ssrc) { return impl_->forward_frame(frame,source_ssrc); }
void NativeRtpSender::feedback(webrtc::ArrayView<const uint8_t> packet) { impl_->receiver->IncomingPacket(packet); }
void NativeRtpSender::set_enabled(bool enabled) {
  if (impl_->enabled==enabled) return;
  impl_->enabled=enabled; impl_->offset.reset(); impl_->history.Clear();
  impl_->needs_keyframe=true; impl_->admitted_frames.clear();
  if (impl_->transport) impl_->transport->discard(impl_->config.ssrc,impl_->config.rtx_ssrc);
  impl_->sender->SetSendingMediaStatus(enabled);
}
void NativeRtpSender::set_media_paused(bool paused) {
  if (impl_->media_paused==paused) return;
  impl_->media_paused=paused;
  // Pause already cleared media state. On resume retain padding currently in
  // the native pacer so opening a video does not truncate an active probe.
  if (!paused) return;
  impl_->offset.reset(); impl_->history.Clear();
  impl_->needs_keyframe=true; impl_->admitted_frames.clear();
  if (impl_->transport) impl_->transport->discard(impl_->config.ssrc,impl_->config.rtx_ssrc);
}
webrtc::rtcp::SenderReport NativeRtpSender::translate_report(const webrtc::rtcp::SenderReport& original) const {
  webrtc::rtcp::SenderReport report;
  report.SetSenderSsrc(impl_->config.ssrc); report.SetNtp(original.ntp()); report.SetRtpTimestamp(original.rtp_timestamp()+impl_->frame_timestamp_offset);
  report.SetPacketCount(impl_->packets); report.SetOctetCount(impl_->octets); return report;
}

struct NativeVideoAssembler::Impl {
  Frame callback;
  webrtc::RtpVideoFrameAssembler assembler{webrtc::RtpVideoFrameAssembler::kVp8};
  // M137's assembler does not extract the orientation extension itself.
  // Preserve it with native extension accessors, bounded by its packet window.
  std::deque<std::pair<uint32_t,webrtc::VideoRotation>> rotations;
  explicit Impl(Frame frame) : callback(std::move(frame)) {}
  void receive(const webrtc::RtpPacketReceived& packet) {
    if (packet.Marker()) {
      auto rotation=webrtc::kVideoRotation_0;
      packet.GetExtension<webrtc::VideoOrientation>(&rotation);
      rotations.emplace_back(packet.Timestamp(),rotation);
      if (rotations.size()>2048) rotations.pop_front();
    }
    for (auto& assembled:assembler.InsertPacket(packet)) {
      auto frame=assembled.ExtractFrame();
      auto found=std::find_if(rotations.rbegin(),rotations.rend(),[&](const auto& entry) {
        return entry.first==frame->RtpTimestamp();
      });
      if (found!=rotations.rend()) frame->rotation_=found->second;
      callback(*frame);
    }
  }
};
NativeVideoAssembler::NativeVideoAssembler(Frame frame) : impl_(std::make_unique<Impl>(std::move(frame))) {}
NativeVideoAssembler::~NativeVideoAssembler()=default;
void NativeVideoAssembler::receive(const webrtc::RtpPacketReceived& packet) { impl_->receive(packet); }

struct NativeRtpReceiver::Impl : webrtc::RtpPacketSinkInterface,webrtc::Transport,
    webrtc::NackSender,webrtc::KeyFrameRequestSender,webrtc::RtcpRttStats {
  webrtc::Environment env;
  NativeReceiveConfig config;
  Media media;
  Rtcp rtcp;
  std::unique_ptr<webrtc::ReceiveStatistics> statistics;
  std::unique_ptr<webrtc::ModuleRtpRtcpImpl2> module;
  std::unique_ptr<webrtc::NackPeriodicProcessor> nack_processor;
  std::unique_ptr<webrtc::NackRequester> nack;
  std::unique_ptr<webrtc::RtxReceiveStream> rtx;
  int64_t rtt_ms=100;
  webrtc::Timestamp last_keyframe=webrtc::Timestamp::MinusInfinity();
  Impl(const webrtc::Environment& e,NativeReceiveConfig c,Media m,Rtcp r)
      : env(e),config(c),media(std::move(m)),rtcp(std::move(r)) {
    if (!config.ssrc || !webrtc::TaskQueueBase::Current()) throw std::invalid_argument("SFU receiver requires signaled SSRC and native task queue");
    statistics=webrtc::ReceiveStatistics::CreateThreadCompatible(&env.clock());
    webrtc::RtpRtcpInterface::Configuration native;
    native.audio=config.audio; native.receiver_only=true; native.local_media_ssrc=webrtc::CreateRandomNonZeroId();
    native.outgoing_transport=this; native.receive_statistics=statistics.get(); native.rtt_stats=this;
    module=std::make_unique<webrtc::ModuleRtpRtcpImpl2>(env,native);
    module->SetRemoteSSRC(config.ssrc); module->SetRTCPStatus(webrtc::RtcpMode::kCompound);
    if (!config.audio) {
      nack_processor=std::make_unique<webrtc::NackPeriodicProcessor>();
      nack=std::make_unique<webrtc::NackRequester>(webrtc::TaskQueueBase::Current(),nack_processor.get(),&env.clock(),this,this,env.field_trials());
    }
    if (config.rtx_payload_type>=0)
      rtx=std::make_unique<webrtc::RtxReceiveStream>(this,std::map<int,int>{{config.rtx_payload_type,config.payload_type}},config.ssrc);
  }
  void OnRtpPacket(const webrtc::RtpPacketReceived& packet) override {
    if (packet.PayloadType()!=config.payload_type || packet.Ssrc()!=config.ssrc) return;
    auto received=packet;
    // BUNDLE's transport map can reflect the last configured m-line. Identify
    // per-source extensions using the native map from this negotiated receiver.
    received.IdentifyExtensions(webrtc::RtpHeaderExtensionMap(config.extensions));
    received.set_payload_type_frequency(config.audio ? 48000 : 90000);
    statistics->OnRtpPacket(received);
    if (nack) nack->OnReceivedPacket(received.SequenceNumber(),received.recovered());
    if (received.payload_size() || !config.audio) media(received);
  }
  bool SendRtp(webrtc::ArrayView<const uint8_t>,const webrtc::PacketOptions&) override { return false; }
  bool SendRtcp(webrtc::ArrayView<const uint8_t> packet) override { return rtcp(packet); }
  void SendNack(const std::vector<uint16_t>& seq,bool) override { module->SendNack(seq); }
  void RequestKeyFrame() override {
    auto now=env.clock().CurrentTime();
    if (config.audio || now-last_keyframe<webrtc::TimeDelta::Millis(200)) return;
    last_keyframe=now; module->SendPictureLossIndication();
  }
  void OnRttUpdate(int64_t rtt) override { rtt_ms=rtt; if (nack) nack->UpdateRtt(rtt); }
  int64_t LastProcessedRtt() const override { return rtt_ms; }
};
NativeRtpReceiver::NativeRtpReceiver(const webrtc::Environment& env,NativeReceiveConfig config,Media media,Rtcp rtcp)
    : impl_(std::make_unique<Impl>(env,config,std::move(media),std::move(rtcp))) {}
NativeRtpReceiver::~NativeRtpReceiver()=default;
void NativeRtpReceiver::receive(const webrtc::RtpPacketReceived& packet) {
  if (impl_->rtx && packet.PayloadType()==impl_->config.rtx_payload_type) impl_->rtx->OnRtpPacket(packet);
  else impl_->OnRtpPacket(packet);
}
void NativeRtpReceiver::feedback(webrtc::ArrayView<const uint8_t> packet) { impl_->module->IncomingRtcpPacket(packet); }
void NativeRtpReceiver::request_keyframe() { impl_->RequestKeyFrame(); }
}
