#include "lk/native_rtp.hpp"
#include "lk/video_allocation.hpp"
#include <api/environment/environment_factory.h>
#include <modules/pacing/prioritized_packet_queue.h>
#include <modules/rtp_rtcp/source/rtp_header_extensions.h>
#include <modules/rtp_rtcp/source/rtcp_packet/remb.h>
#include <modules/rtp_rtcp/source/rtcp_packet/nack.h>
#include <rtc_base/thread.h>
#include <rtc_base/time_utils.h>
#include <rtc_base/logging.h>
#include <cstdlib>
#include <system_wrappers/include/clock.h>
#include <deque>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void native_queue_pause_test() {
  using webrtc::Timestamp;
  using webrtc::TimeDelta;
  using webrtc::RtpPacketMediaType;
  auto packet=[](uint16_t seq,RtpPacketMediaType type) {
    auto p=std::make_unique<webrtc::RtpPacketToSend>(nullptr);
    p->SetSsrc(type==RtpPacketMediaType::kAudio ? 22 : 11);
    p->SetSequenceNumber(seq); p->set_packet_type(type); p->AllocatePayload(100);
    return p;
  };
  webrtc::PacketQueueTTL ttl; ttl.video=TimeDelta::Millis(500);
  webrtc::PrioritizedPacketQueue queue(Timestamp::Zero(),true,ttl);
  queue.SetPauseState(true,Timestamp::Zero());
  queue.SetPauseState(false,Timestamp::Millis(1000));
  queue.Push(Timestamp::Millis(1000),packet(1,RtpPacketMediaType::kVideo));
  queue.Push(Timestamp::Millis(1001),packet(2,RtpPacketMediaType::kVideo));
  check(queue.SizeInPackets()==2,"Native TTL discarded a 1 ms old packet after a 1 s empty pause");
  check(queue.LeadingPacketEnqueueTime(RtpPacketMediaType::kVideo)==Timestamp::Millis(1000),
        "Native leading packet time included pauses before this packet existed");
  queue.Push(Timestamp::Millis(1002),packet(3,RtpPacketMediaType::kAudio));
  check(queue.Pop()->SequenceNumber()==3,"Audio priority changed after pause");
  check(queue.SizeInPackets()==2,"Switching priority discarded fresh video after pause");
  check(queue.Pop()->SequenceNumber()==1 && queue.Pop()->SequenceNumber()==2,"Fresh video order changed");
  queue.Push(Timestamp::Millis(1100),packet(4,RtpPacketMediaType::kVideo));
  queue.SetPauseState(true,Timestamp::Millis(1200));
  queue.SetPauseState(false,Timestamp::Millis(1500));
  check(queue.AverageQueueTime()==TimeDelta::Millis(100),"Average queue time must still exclude pauses");
  queue.Push(Timestamp::Millis(1601),packet(5,RtpPacketMediaType::kVideo));
  check(queue.SizeInPackets()==1 && queue.Pop()->SequenceNumber()==5,"Actual wall-clock TTL was disabled");
  for (int round=0;round<8;++round) {
    auto start=Timestamp::Millis(2000+round*1000);
    queue.SetPauseState(true,start); queue.SetPauseState(false,start+TimeDelta::Millis(750));
    queue.Push(start+TimeDelta::Millis(750),packet(6,RtpPacketMediaType::kVideo));
    queue.Push(start+TimeDelta::Millis(751),packet(7,RtpPacketMediaType::kVideo));
    check(queue.SizeInPackets()==2,"Accumulated renegotiation pauses expired new packets");
    queue.Pop(); queue.Pop();
  }
}
webrtc::RtpPacketReceived media(uint16_t seq,size_t size) {
  webrtc::RtpPacketReceived p;
  p.SetSsrc(11); p.SetPayloadType(96); p.SetSequenceNumber(seq); p.SetTimestamp(90000+seq*3000); p.SetMarker(true);
  std::fill_n(p.AllocatePayload(size),size,uint8_t(seq)); return p;
}
struct Observed {
  webrtc::RtpPacketReceived packet;
  webrtc::RtpPacketMediaType type;
  webrtc::Timestamp sent;
};
struct NativeClockScope : webrtc::ClockInterface {
  webrtc::Clock& clock;
  webrtc::ClockInterface* previous;
  explicit NativeClockScope(webrtc::Clock& c) : clock(c),previous(webrtc::SetClockForTesting(this)) {}
  ~NativeClockScope() override {webrtc::SetClockForTesting(previous);}
  int64_t TimeNanos() const override {return clock.CurrentTime().us()*1000;}
};
void advancing_clock_probe_test() {
  // Unlike a frozen SimulatedClock, real time advances between the adapter's
  // read and PacingController::NextSendTime(), which may return its own "now".
  struct AdvancingClock : webrtc::SimulatedClock {
    AdvancingClock():webrtc::SimulatedClock(1000000){}
    webrtc::Timestamp CurrentTime() override {
      AdvanceTimeMicroseconds(1);return webrtc::SimulatedClock::CurrentTime();
    }
  } clock;
  NativeClockScope scope(clock);auto env=webrtc::CreateEnvironment(&clock);
  auto transport=std::make_shared<lk::NativeSendTransport>(env);
  lk::NativeSendConfig video;video.ssrc=111;video.rtx_ssrc=112;video.rtx_payload_type=97;
  video.extensions={{webrtc::RtpExtension::kTransportSequenceNumberUri,5}};
  lk::NativeRtpSender sender(env,video,[](const auto&){return true;},[]{},transport);
  sender.set_media_paused(true);transport->set_available(true);
  for(int ms=0;ms<300;++ms){transport->process();clock.AdvanceTimeMilliseconds(1);}
  check(transport->stats().probe_packets>=5,"Advancing real clock starved the initial padding probe");
}
void shaped_link_test(bool cold_start=false,bool video_only=false) {
  webrtc::SimulatedClock clock(1000000);
  // M137's native AlrDetector records its start with global TimeMillis, whereas
  // ProbeController uses Environment::clock. Both must share simulated time.
  NativeClockScope native_clock(clock); auto env=webrtc::CreateEnvironment(&clock);
  auto transport=std::make_shared<lk::NativeSendTransport>(env);
  struct Delivery { webrtc::RtpPacketReceived packet; webrtc::Timestamp at; bool audio; };
  std::deque<Delivery> wire;
  auto next_free=clock.CurrentTime();
  int64_t capacity=1500000;
  uint64_t link_drops=0,audio_received=0,video_before=0,video_recovered=0;
  uint16_t video_sequence=0;
  auto last_audio=clock.CurrentTime(); int64_t max_audio_gap=0;
  lk::NativeReceiveTransport receiver(env,[&](auto bytes){transport->feedback(bytes);return true;});
  auto emit=[&](const webrtc::RtpPacketToSend& packet) {
    auto now=clock.CurrentTime();
    webrtc::PacketInfo info; info.included_in_feedback=true; info.included_in_allocation=true; info.packet_size_bytes=packet.size();
    transport->sent_packet(webrtc::SentPacketInfo(*packet.transport_sequence_number(),now.ms(),info));
    // A shared FIFO link with finite serialization rate and 250 ms tail-drop
    // queue. Native TWCC sees the resulting delay/loss; no target setter/REMB.
    if (next_free-now>webrtc::TimeDelta::Millis(250)) {++link_drops; return true;}
    next_free=std::max(next_free,now)+webrtc::TimeDelta::Micros(int64_t(packet.size()+48)*8000000/capacity);
    webrtc::RtpPacketReceived parsed(&packet.extension_manager());
    check(parsed.Parse(packet.data(),packet.size()),"Shaped-link packet parse failed");
    wire.push_back({std::move(parsed),next_free+webrtc::TimeDelta::Millis(25),packet.packet_type()==webrtc::RtpPacketMediaType::kAudio});
    return true;
  };
  lk::NativeSendConfig video; video.ssrc=111; video.rtx_ssrc=112; video.payload_type=100; video.rtx_payload_type=101;
  video.extensions={{webrtc::RtpExtension::kTransportSequenceNumberUri,5},{webrtc::RtpExtension::kAbsSendTimeUri,3}};
  auto audio=video; audio.audio=true; audio.ssrc=211; audio.rtx_ssrc.reset(); audio.rtx_payload_type=-1;
  lk::NativeRtpSender video_sender(env,video,emit,[]{},transport);
  std::unique_ptr<lk::NativeRtpSender> audio_sender;
  if (!video_only) audio_sender=std::make_unique<lk::NativeRtpSender>(env,audio,emit,[]{},transport);
  const int64_t audio_bps=video_only ? 0 : 64000;
  std::vector<lk::VideoBandwidthRequest> requests{{"video",1,{{11,80000},{12,200000},{13,450000}}}};
  if (cold_start) requests[0].layers={{11,450000},{12,700000},{13,900000}};
  bool high_before=false,paused=false,recovered=false;
  int first_video_ms=-1;
  int64_t minimum_target=20000000,maximum_target=0;
  transport->set_available(true);
  for (int ms=0;ms<60000;++ms) {
    capacity=ms<10000 || ms>=25000 ? 1500000 : 100000;
    if (ms%200==0) {
      auto stats=transport->stats();
      auto allocation=lk::allocate_video_bandwidth(stats.target_bps,audio_bps,clock.TimeInMilliseconds(),requests);
      auto selected=requests[0].state.selected;
      if (selected && first_video_ms<0) first_video_ms=ms;
      transport->set_padding_demand(lk::recovery_padding_demand(stats.target_bps,audio_bps+allocation.video_allocated_bps,selected==13 ? 0 : (video_only ? requests[0].layers.back().bps : 514000)));
      if(video_only&&ms%5000==0&&std::getenv("LK_WEBRTC_LOG"))std::cout<<"Video-only ms="<<ms<<" target="<<stats.target_bps<<" selected="<<selected<<" allocated="<<allocation.video_allocated_bps<<" padding="<<stats.padding<<" probes="<<stats.probe_clusters<<'\n';
      video_sender.set_media_paused(!selected);
      if (ms>=5000 && ms<10000) high_before|=selected==13;
      if (ms>=15000 && ms<25000) {paused|=!selected; minimum_target=std::min(minimum_target,stats.target_bps);}
      if (ms>=35000) {recovered|=selected==13; maximum_target=std::max(maximum_target,stats.target_bps);}
    }
    if (ms%20==0) {
      auto seq=uint16_t(ms/20); if(audio_sender)audio_sender->forward(media(seq,20));
      for (auto layer:requests[0].layers) if (layer.ssrc==requests[0].state.selected) {
        // Model 20 ms of wire demand with MTU-sized RTP packets. A single
        // 900 kbps packet per tick exceeds the sender capacity and is silently
        // discarded, leaving only probes instead of the intended media load.
        const auto wire_bytes=size_t(layer.bps/400);
        const auto packets=(wire_bytes+1199)/1200;
        size_t remaining=wire_bytes-packets*80;
        for (size_t i=0;i<packets;++i) {
          const auto payload=remaining/(packets-i); remaining-=payload;
          auto packet=media(video_sequence++,payload);
          packet.SetTimestamp(90000+ms*90); packet.SetMarker(i+1==packets);
          video_sender.forward(packet);
        }
      }
    }
    transport->process();
    while (!wire.empty() && wire.front().at<=clock.CurrentTime()) {
      auto packet=std::move(wire.front()); wire.pop_front(); packet.packet.set_arrival_time(clock.CurrentTime());
      receiver.receive(packet.packet,packet.audio);
      if (packet.audio) {++audio_received; max_audio_gap=std::max(max_audio_gap,(clock.CurrentTime()-last_audio).ms());last_audio=clock.CurrentTime();}
      else if (packet.packet.Ssrc()==video.ssrc && packet.packet.payload_size()>0) {
        if (ms>=5000 && ms<10000) ++video_before;
        if (ms>=35000) ++video_recovered;
      }
    }
    receiver.process(); clock.AdvanceTimeMilliseconds(1);
  }
  std::cout<<"Shaped link: video_only="<<video_only<<" high="<<high_before<<" paused="<<paused<<" recovered="<<recovered
    <<" target_min="<<minimum_target<<" target_recovered_max="<<maximum_target<<" drops="<<link_drops
    <<" audio_packets="<<audio_received<<" audio_gap_ms="<<max_audio_gap<<" first_video_ms="<<first_video_ms
    <<" padding="<<transport->stats().padding<<" probes="<<transport->stats().probe_clusters<<" probe_feedback="<<transport->stats().probe_feedback_packets<<'\n';
  check(video_before>100 && video_recovered>100,"Shaped link did not deliver actual video before and after congestion");
  if (!cold_start) check(high_before,"Unconstrained native estimate never admitted high video");
  check(paused && minimum_target<170000,"Serial-link collapse did not reach allocator through native TWCC");
  check(recovered,"Native probes did not recover video after serial-link capacity returned");
  if (!video_only) check(audio_received>2500 && max_audio_gap<2000,"Audio starved during link collapse/recovery");
  check(transport->stats().probe_feedback_packets>0,"Shaped-link recovery had no native probe feedback");
  if (cold_start) check(first_video_ms>=0 && first_video_ms<3000,"Native padding failed to bootstrap a video above the initial bandwidth budget");
}
void run() {
  webrtc::SimulatedClock clock(1000000);
  auto env=webrtc::CreateEnvironment(&clock);
  auto transport=std::make_shared<lk::NativeSendTransport>(env);
  std::vector<Observed> sent;
  std::deque<Observed> wire;
  lk::NativeReceiveTransport receiver(env,[&](auto bytes){transport->feedback(bytes);return true;});
  auto emit=[&](const webrtc::RtpPacketToSend& packet) {
    check(packet.transport_sequence_number().has_value(),"Missing hop transport sequence metadata");
    uint16_t seq;
    check(packet.GetExtension<webrtc::TransportSequenceNumber>(&seq),"Missing negotiated TWCC extension");
    check(seq==uint16_t(*packet.transport_sequence_number()),"Transport sequence metadata differs from wire");
    if (!sent.empty()) {
      uint16_t prior; sent.back().packet.GetExtension<webrtc::TransportSequenceNumber>(&prior);
      check(seq==uint16_t(prior+1),"Audio/video/RTX/padding must share sequence space");
    }
    webrtc::RtpPacketReceived parsed(&packet.extension_manager());
    check(parsed.Parse(packet.data(),packet.size()),"Native wire parse failed");
    Observed entry{parsed,*packet.packet_type(),clock.CurrentTime()};
    sent.push_back(entry); wire.push_back(entry);
    webrtc::PacketInfo info; info.included_in_feedback=true; info.included_in_allocation=true;
    info.packet_size_bytes=packet.size();
    transport->sent_packet(webrtc::SentPacketInfo(seq,clock.TimeInMilliseconds(),info));
    return true;
  };
  lk::NativeSendConfig video;
  video.ssrc=111; video.rtx_ssrc=112; video.payload_type=100; video.rtx_payload_type=101; video.mid="v";
  video.extensions={{webrtc::RtpExtension::kMidUri,4},{webrtc::RtpExtension::kTransportSequenceNumberUri,5},
                    {webrtc::RtpExtension::kAbsSendTimeUri,3}};
  auto audio=video; audio.ssrc=211; audio.rtx_ssrc.reset(); audio.rtx_payload_type=-1; audio.payload_type=111; audio.audio=true; audio.mid="a";
  auto pump=[&](int duration) {
    for (int ms=0;ms<duration;++ms) {
      transport->process();
      while (!wire.empty() && wire.front().sent+webrtc::TimeDelta::Millis(25)<=clock.CurrentTime()) {
        auto delivered=std::move(wire.front()); wire.pop_front();
        delivered.packet.set_arrival_time(clock.CurrentTime());
        receiver.receive(delivered.packet,delivered.type==webrtc::RtpPacketMediaType::kAudio);
      }
      receiver.process(); clock.AdvanceTimeMilliseconds(1);
    }
  };
  {
    lk::NativeRtpSender video_sender(env,video,emit,[]{},transport);
    lk::NativeRtpSender audio_sender(env,audio,emit,[]{},transport);
    for (uint16_t i=0;i<30;++i) video_sender.forward(media(i,1000));
    audio_sender.forward(media(1,100)); audio_sender.forward(media(2,100));
    pump(10); check(sent.empty(),"Unavailable transport sent queued media");
    check(transport->stats().queue_bytes>30000,"Native pacer did not retain queued media");
    transport->set_available(true); pump(1);
    auto first_media=std::find_if(sent.begin(),sent.end(),[](const auto& p){return p.type!=webrtc::RtpPacketMediaType::kPadding;});
    check(first_media!=sent.end() && first_media->type==webrtc::RtpPacketMediaType::kAudio,"Native pacer did not prioritize audio");
    check(transport->stats().queue_bytes>0,"Video burst bypassed pacing");
    pump(900);
    auto stats=transport->stats();
    check(stats.feedback>0 && stats.feedback_packets>0,"Native TWCC did not reach GoogCC feedback adapter");
    check(stats.target_bps>0 && stats.pacing_bps>0,"GoogCC produced no send budget");
    size_t video_count=0; std::optional<uint16_t> last_video;
    for (const auto& p:sent) if (p.type==webrtc::RtpPacketMediaType::kVideo) {++video_count;last_video=p.packet.SequenceNumber();}
    check(video_count==30,"Paced baseline did not deliver complete encoded video burst");
    check(sent.back().sent>sent.front().sent,"All video packets were sent as one unpaced burst");

    // Feed a real native REMB packet, not a direct setter of the output rate.
    webrtc::rtcp::Remb remb; remb.SetSenderSsrc(999); remb.SetSsrcs({111}); remb.SetBitrateBps(80000);
    auto b=remb.Build(); transport->feedback({b.data(),b.size()}); pump(50);
    check(transport->stats().target_bps<=80000,"Remote bitrate feedback did not reduce native target");
    std::vector<lk::VideoBandwidthRequest> allocation{{"video",1,{{11,100000},{12,500000}}}};
    auto budget=lk::allocate_video_bandwidth(transport->stats().target_bps,64000,clock.TimeInMilliseconds(),allocation);
    check(budget.video_allocated_bps==0 && allocation[0].state.selected==0,"Real native feedback did not pause unaffordable video");

    webrtc::rtcp::Nack nack; nack.SetSenderSsrc(999); nack.SetMediaSsrc(111); nack.SetPacketIds({*last_video});
    auto n=nack.Build(); video_sender.feedback({n.data(),n.size()});
    size_t before=sent.size(); pump(100);
    bool rtx=false;
    for (size_t i=before;i<sent.size();++i) rtx|=sent[i].type==webrtc::RtpPacketMediaType::kRetransmission && sent[i].packet.Ssrc()==112;
    check(rtx,"Native RTX did not pass through shared pacing transport");

    transport->set_available(false);
    video_sender.forward(media(31,1000)); audio_sender.forward(media(3,100));
    check(transport->stats().queue_bytes>0,"Pause test did not enqueue packets");
    video_sender.set_enabled(false); audio_sender.set_enabled(false);
    check(transport->stats().queue_bytes==0,"Muting must cancel paced packets immediately");
    before=sent.size(); transport->set_available(true); pump(30);
    check(sent.size()==before,"Muted sender emitted a queued packet or probe");
    video_sender.set_enabled(true); audio_sender.set_enabled(true);
    transport->set_available(false); video_sender.forward(media(32,1000)); audio_sender.forward(media(4,100));
  }
  check(transport->stats().queue_bytes==0,"Stream destruction retained queued media");
  const auto before=sent.size(); transport->set_available(true); pump(50);
  check(sent.size()==before,"Destroyed sender callback was invoked");
  auto bounded=std::make_shared<lk::NativeSendTransport>(env,16000);
  {
    lk::NativeRtpSender queued(env,video,[](const auto&){throw std::runtime_error("Paused queue sent");return false;},[]{},bounded);
    for (uint16_t i=0;i<100;++i) queued.forward(media(i,1000));
    check(bounded->stats().queue_bytes<=16000 && bounded->stats().queue_dropped>0,"Unavailable transport exceeded its queue budget");
  }
  check(bounded->stats().queue_bytes==0,"Bounded queue did not clean up on teardown");

  // Writable transport before any scheduled task executes used to start the
  // initial probes before M137 applied the repeated-probing option. Deliberately
  // supply no receiver feedback so native initial probing must retry.
  auto late=std::make_shared<lk::NativeSendTransport>(env);
  late->set_available(true);
  for (int ms=0;ms<600;++ms) {late->process();clock.AdvanceTimeMilliseconds(1);}
  check(late->stats().probe_clusters==0,"Native probing started without an attached RTP sender");
  lk::NativeRtpSender late_sender(env,video,[](const auto&){return true;},[]{},late);
  late_sender.set_media_paused(true);
  for (int ms=0;ms<1500;++ms) {late->process();clock.AdvanceTimeMilliseconds(1);}
  check(late->stats().probe_packets>0 && late->stats().sent==late->stats().padding,
        "Late video-only attachment could not bootstrap using native padding");

  auto retry=std::make_shared<lk::NativeSendTransport>(env);
  lk::NativeRtpSender retry_sender(env,video,[](const auto&){return true;},[]{},retry);
  retry_sender.forward(media(1,1000));
  retry_sender.set_media_paused(true);
  retry->set_padding_demand(64000);
  check(retry->stats().queue_bytes==0,"Bandwidth pause did not cancel queued media");
  retry_sender.forward(media(2,1000));
  check(retry->stats().queue_bytes==0,"Bandwidth-paused sender admitted media");
  retry->set_available(true);
  for (int ms=0;ms<2200;++ms) { retry->process(); clock.AdvanceTimeMilliseconds(1); }
  check(retry->stats().probe_clusters>=3,"Writable-before-first-task suppressed repeated native initial probing");
  check(retry->stats().probe_packets>0,"Native probe clusters did not send probe packets");
  check(retry->stats().sent==retry->stats().padding,"Budget pause emitted media instead of padding-only probes");
  retry->set_available(false);
  auto padding=std::make_unique<webrtc::RtpPacketToSend>(nullptr);
  padding->SetSsrc(112); padding->SetPayloadType(101); padding->SetPadding(100);
  padding->set_packet_type(webrtc::RtpPacketMediaType::kPadding);
  padding->set_capture_time(clock.CurrentTime());
  std::vector<std::unique_ptr<webrtc::RtpPacketToSend>> pending_probe;
  pending_probe.push_back(std::move(padding)); check(retry->enqueue(std::move(pending_probe)),"Probe queue fixture rejected");
  auto queued_probe=retry->stats().queue_bytes;
  retry_sender.set_media_paused(false);
  check(queued_probe>0 && retry->stats().queue_bytes==queued_probe,"Opening video truncated queued native padding");
}
}
int main() {
  if(const char* value=std::getenv("LK_WEBRTC_LOG");value&&std::string_view(value)=="info")webrtc::LogMessage::LogToDebug(webrtc::LS_INFO);
  auto thread=webrtc::Thread::Create(); thread->SetName("native-send-test",nullptr);
  if (!thread->Start()) return 1;
  std::string error;
  thread->BlockingCall([&]{try {advancing_clock_probe_test();native_queue_pause_test();run();shaped_link_test();shaped_link_test(true);shaped_link_test(true,true);} catch(const std::exception& e){error=e.what();}});
  thread->Stop();
  if (!error.empty()) {std::cerr<<error<<'\n';return 1;}
  std::cout<<"PASS native pacing, audio priority, TWCC, bitrate feedback, RTX, mute and teardown\n";
}
