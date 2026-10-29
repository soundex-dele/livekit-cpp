#include "lk/native_rtp.hpp"
#include <api/transport/goog_cc_factory.h>
#include <api/task_queue/pending_task_safety_flag.h>
#include <modules/pacing/pacing_controller.h>
#include <modules/congestion_controller/rtp/transport_feedback_adapter.h>
#include <modules/rtp_rtcp/source/rtp_header_extensions.h>
#include <modules/rtp_rtcp/source/rtcp_packet/common_header.h>
#include <modules/rtp_rtcp/source/rtcp_packet/transport_feedback.h>
#include <modules/rtp_rtcp/source/rtcp_packet/remb.h>
#include <rtc_base/task_utils/repeating_task.h>
#include <rtc_base/logging.h>
#include <algorithm>
#include <map>
#include <stdexcept>

namespace lk {
struct NativeSendTransport::Impl : webrtc::PacingController::PacketSender {
  webrtc::Environment env;
  std::map<uint32_t, Stream> streams;
  std::map<uint32_t, uint32_t> primary;
  std::map<uint32_t, uint32_t> rtx;
  webrtc::GoogCcNetworkControllerFactory factory;
  std::unique_ptr<webrtc::NetworkControllerInterface> controller;
  webrtc::TransportFeedbackAdapter feedback_adapter;
  webrtc::PacingController pacer;
  webrtc::RepeatingTaskHandle task;
  webrtc::TaskQueueBase* owner = webrtc::TaskQueueBase::Current();
  webrtc::TaskQueueBase* timer = nullptr;
  webrtc::ScopedTaskSafety safety;
  webrtc::TargetRateConstraints constraints;
  webrtc::Timestamp next_control = webrtc::Timestamp::MinusInfinity();
  std::optional<webrtc::DataSize> congestion_window;
  std::optional<webrtc::NetworkRoute> route;
  uint16_t sequence = 0;
  size_t overhead = 48;  // Conservative default until the ICE/SRTP route reports it.
  bool available = false, network_available = false;
  int64_t padding_demand_bps = -1;
  Stats counters;
  const size_t max_queue_bytes;

  static webrtc::PacingController::Configuration pacing_config() {
    auto c = webrtc::PacingController::DefaultConfiguration();
    c.drain_large_queues = false;  // Do not defeat the bandwidth estimate to drain video.
    c.prioritize_audio_retransmission = true;
    c.packet_queue_ttl.video = webrtc::TimeDelta::Millis(500);
    c.packet_queue_ttl.video_retransmission = webrtc::TimeDelta::Millis(250);
    c.packet_queue_ttl.audio_retransmission = webrtc::TimeDelta::Millis(150);
    c.send_burst_interval = webrtc::TimeDelta::Millis(5);
    return c;
  }

  explicit Impl(const webrtc::Environment& e, size_t limit, webrtc::TaskQueueBase* timer_queue)
      : env(e),
        pacer(&env.clock(), this, env.field_trials(), pacing_config()),
        timer(timer_queue),
        max_queue_bytes(limit) {
    if (!webrtc::TaskQueueBase::Current())
      throw std::invalid_argument("Native send transport requires a task queue");
    if (limit < 2000)
      throw std::invalid_argument("Pacer queue must hold at least one RTP packet");
    webrtc::NetworkControllerConfig config(env);
    constraints.at_time = env.clock().CurrentTime();
    constraints.min_data_rate = webrtc::DataRate::KilobitsPerSec(30);
    constraints.starting_rate = webrtc::DataRate::KilobitsPerSec(300);
    constraints.max_data_rate = webrtc::DataRate::KilobitsPerSec(20000);
    config.constraints = constraints;
    config.stream_based_config.at_time = constraints.at_time;
    config.stream_based_config.requests_alr_probing = true;
    config.stream_based_config.enable_repeated_initial_probing = true;
    controller = factory.Create(config);
    pacer.SetPacingRates(*constraints.starting_rate, webrtc::DataRate::Zero());
    pacer.SetAccountForAudioPackets(true);
    pacer.SetAllowProbeWithoutMediaPacket(true);
    pacer.SetIncludeOverhead();
    pacer.SetTransportOverhead(webrtc::DataSize::Bytes(48));
    pacer.Pause();
    // M137 consumes initial_config on its first process call, including ALR
    // and repeated-initial-probe options. Do this while network availability
    // is still false: an already-writable ICE transport must not start probing
    // before those options have been installed.
    process();
    if (timer)
      schedule(webrtc::TimeDelta::Zero());
    else
      task = webrtc::RepeatingTaskHandle::Start(
          owner, [this] { return process(); }, webrtc::TaskQueueBase::DelayPrecision::kHigh,
          &env.clock());
  }

  ~Impl() {
    task.Stop();
  }

  void schedule(webrtc::TimeDelta delay) {
    // rtc::Thread's socket wait cannot honor high precision on Windows. Use
    // libwebrtc's native timer queue to wake it, without moving RTP state or
    // transport calls off their owner. The router outlives all pending timers.
    timer->PostDelayedHighPrecisionTask(
        [queue = owner, alive = safety.flag(), this] {
          queue->PostTask(webrtc::SafeTask(alive, [this] { schedule(process()); }));
        },
        delay);
  }

  void apply(webrtc::NetworkControlUpdate update) {
    if (update.target_rate)
      counters.target_bps = update.target_rate->target_rate.bps();
    if (update.pacer_config)
      pacer.SetPacingRates(update.pacer_config->data_rate(), update.pacer_config->pad_rate());
    if (!update.probe_cluster_configs.empty()) {
      counters.probe_clusters += update.probe_cluster_configs.size();
      pacer.CreateProbeClusters(update.probe_cluster_configs);
    }
    if (update.congestion_window)
      congestion_window = update.congestion_window;
    pacer.SetCongested(congestion_window &&
                       feedback_adapter.GetOutstandingData() >= *congestion_window);
  }

  webrtc::TimeDelta process() {
    auto now = env.clock().CurrentTime();
    if (now >= next_control) {
      webrtc::ProcessInterval interval;
      interval.at_time = now;
      interval.pacer_queue = pacer.QueueSizeData();
      apply(controller->OnProcessInterval(interval));
      next_control = now + factory.GetProcessInterval();
    }
    if (available) {
      // NextSendTime returns its own current time for a newly started probe.
      // Reading `now` before it can perpetually defer that probe on a real
      // clock, even though frozen-clock tests pass. Match the native paced
      // sender's ordering and early-probe margin.
      const auto next = pacer.NextSendTime();
      const auto margin = pacer.IsProbing() ? webrtc::PacingController::kMaxEarlyProbeProcessing
                                            : webrtc::TimeDelta::Zero();
      if (next <= env.clock().CurrentTime() + margin)
        pacer.ProcessPackets();
    }
    auto next = available ? std::min(pacer.NextSendTime(), next_control) : next_control;
    auto delay = next - env.clock().CurrentTime();
    return std::clamp(delay, webrtc::TimeDelta::Millis(1), webrtc::TimeDelta::Millis(10));
  }

  void SendPacket(std::unique_ptr<webrtc::RtpPacketToSend> packet,
                  const webrtc::PacedPacketInfo& info) override {
    auto p = primary.find(packet->Ssrc());
    if (p == primary.end())
      return;
    auto& stream = streams.at(p->second);
    stream.prepare(*packet);
    auto now = env.clock().CurrentTime();
    // Native packet accessors generate hop-specific extensions at actual send.
    packet->set_transport_sequence_number(++sequence);
    if (packet->HasExtension<webrtc::TransportSequenceNumber>())
      packet->SetExtension<webrtc::TransportSequenceNumber>(sequence);
    if (packet->HasExtension<webrtc::AbsoluteSendTime>())
      packet->SetExtension<webrtc::AbsoluteSendTime>(
          webrtc::AbsoluteSendTime::To24Bits(env.clock().ConvertTimestampToNtpTime(now)));
    feedback_adapter.AddPacket(*packet, info, overhead, now);
    bool sent = available && stream.send(*packet);
    if (sent) {
      ++counters.sent;
      if (info.probe_cluster_id != webrtc::PacedPacketInfo::kNotAProbe)
        ++counters.probe_packets;
      if (packet->packet_type() == webrtc::RtpPacketMediaType::kPadding)
        ++counters.padding;
    } else
      ++counters.dropped;
    stream.complete(std::move(packet), sent);
  }

  std::vector<std::unique_ptr<webrtc::RtpPacketToSend>> FetchFec() override {
    return {};
  }

  std::vector<std::unique_ptr<webrtc::RtpPacketToSend>> GeneratePadding(
      webrtc::DataSize size) override {
    if (!available)
      return {};
    for (auto& [_, stream] : streams) {
      auto packets = stream.padding(size.bytes());
      if (!packets.empty())
        return packets;
    }
    return {};
  }

  void OnAbortedRetransmissions(uint32_t ssrc, webrtc::ArrayView<const uint16_t> seq) override {
    if (auto p = primary.find(ssrc); p != primary.end())
      streams.at(p->second).abort(seq);
  }

  std::optional<uint32_t> GetRtxSsrcForMedia(uint32_t ssrc) const override {
    auto found = rtx.find(ssrc);
    return found == rtx.end() ? std::nullopt : std::optional(found->second);
  }
};

NativeSendTransport::NativeSendTransport(const webrtc::Environment& env, size_t limit,
                                         webrtc::TaskQueueBase* timer)
    : impl_(std::make_unique<Impl>(env, limit, timer)) {}

NativeSendTransport::~NativeSendTransport() = default;

void NativeSendTransport::attach(uint32_t ssrc, std::optional<uint32_t> rtx, Stream stream) {
  if (impl_->primary.contains(ssrc) || (rtx && impl_->primary.contains(*rtx)))
    throw std::invalid_argument("Duplicate sender SSRC");
  impl_->streams.emplace(ssrc, std::move(stream));
  impl_->primary[ssrc] = ssrc;
  if (rtx) {
    impl_->primary[*rtx] = ssrc;
    impl_->rtx[ssrc] = *rtx;
  }
  set_available(impl_->network_available);
}

void NativeSendTransport::discard(uint32_t ssrc, std::optional<uint32_t> rtx) {
  impl_->pacer.RemovePacketsForSsrc(ssrc);
  if (rtx)
    impl_->pacer.RemovePacketsForSsrc(*rtx);
}

void NativeSendTransport::detach(uint32_t ssrc, std::optional<uint32_t> rtx) {
  discard(ssrc, rtx);
  impl_->streams.erase(ssrc);
  impl_->primary.erase(ssrc);
  if (rtx)
    impl_->primary.erase(*rtx);
  impl_->rtx.erase(ssrc);
  set_available(impl_->network_available);
}

bool NativeSendTransport::enqueue(std::vector<std::unique_ptr<webrtc::RtpPacketToSend>> batch) {
  size_t available = impl_->max_queue_bytes -
                     std::min(impl_->max_queue_bytes, size_t(impl_->pacer.QueueSizeData().bytes()));
  bool fits = true;
  for (const auto& packet : batch) {
    // QueueSizeData includes the configured native per-packet overhead, which
    // can exceed the RTP header size. Use the same accounting for the new batch.
    size_t cost =
        std::max(packet->size(), packet->payload_size() + packet->padding_size() + impl_->overhead);
    if (cost > available) {
      fits = false;
      break;
    }
    available -= cost;
  }
  if (!fits) {
    impl_->counters.queue_dropped += batch.size();
    for (const auto& packet : batch)
      if (packet->retransmitted_sequence_number()) {
        if (auto p = impl_->primary.find(packet->Ssrc()); p != impl_->primary.end()) {
          uint16_t sequence = *packet->retransmitted_sequence_number();
          impl_->streams.at(p->second).abort({&sequence, 1});
        }
      }
    return false;
  }
  for (auto& packet : batch)
    impl_->pacer.EnqueuePacket(std::move(packet));
  return true;
}

void NativeSendTransport::set_available(bool available) {
  impl_->network_available = available;
  // Initial probing needs an attached native RTP sender that can generate
  // padding. Starting with no streams latches the native pacer's send-failure
  // state before its first packet; a video waiting for budget then cannot wake
  // it. Defer availability until attachment, without fabricating a media packet.
  available = available && !impl_->streams.empty();
  if (available == impl_->available)
    return;
  RTC_LOG(LS_INFO) << "SFU transport " << impl_.get() << " available=" << available
                   << " streams=" << impl_->streams.size();
  impl_->available = available;
  if (available)
    impl_->pacer.Resume();
  else
    impl_->pacer.Pause();
  impl_->apply(
      impl_->controller->OnNetworkAvailability({impl_->env.clock().CurrentTime(), available}));
}

void NativeSendTransport::sent_packet(const webrtc::SentPacketInfo& sent) {
  if (auto packet = impl_->feedback_adapter.ProcessSentPacket(sent))
    impl_->apply(impl_->controller->OnSentPacket(*packet));
}

void NativeSendTransport::set_padding_demand(int64_t padding_bps) {
  padding_bps = std::max<int64_t>(0, padding_bps);
  if (padding_bps == impl_->padding_demand_bps)
    return;
  impl_->padding_demand_bps = padding_bps;
  webrtc::StreamsConfig config;
  config.at_time = impl_->env.clock().CurrentTime();
  // Do not set max_total_allocated_bitrate from source demand: M137 treats a
  // nonzero allocation as the end of repeated initial probing, even if no
  // media was admitted yet. Padding demand is not an encoder allocation.
  config.max_padding_rate = webrtc::DataRate::BitsPerSec(padding_bps);
  impl_->apply(impl_->controller->OnStreamsConfig(config));
}

void NativeSendTransport::network_route(const webrtc::NetworkRoute& route) {
  if (impl_->route && *impl_->route == route)
    return;
  impl_->route = route;
  impl_->feedback_adapter.SetNetworkRoute(route);
  if (route.packet_overhead > 0) {
    impl_->overhead = size_t(route.packet_overhead);
    impl_->pacer.SetTransportOverhead(webrtc::DataSize::Bytes(impl_->overhead));
  }
  webrtc::NetworkRouteChange change;
  change.at_time = impl_->env.clock().CurrentTime();
  change.constraints = impl_->constraints;
  change.constraints.at_time = change.at_time;
  impl_->apply(impl_->controller->OnNetworkRouteChange(change));
}

void NativeSendTransport::feedback(webrtc::ArrayView<const uint8_t> bytes) {
  auto* cursor = bytes.data();
  const auto* end = cursor + bytes.size();
  while (cursor < end) {
    webrtc::rtcp::CommonHeader header;
    if (!header.Parse(cursor, size_t(end - cursor)))
      return;
    if (header.type() == webrtc::rtcp::TransportFeedback::kPacketType &&
        header.fmt() == webrtc::rtcp::TransportFeedback::kFeedbackMessageType) {
      webrtc::rtcp::TransportFeedback feedback;
      if (feedback.Parse(header))
        if (auto report = impl_->feedback_adapter.ProcessTransportFeedback(
                feedback, impl_->env.clock().CurrentTime())) {
          ++impl_->counters.feedback;
          impl_->counters.feedback_packets += report->packet_feedbacks.size();
          for (const auto& packet : report->packet_feedbacks)
            if (packet.IsReceived() && packet.sent_packet.pacing_info.probe_cluster_id !=
                                           webrtc::PacedPacketInfo::kNotAProbe)
              ++impl_->counters.probe_feedback_packets;
          impl_->apply(impl_->controller->OnTransportPacketsFeedback(*report));
        }
    } else if (header.type() == webrtc::rtcp::Remb::kPacketType &&
               header.fmt() == webrtc::rtcp::Psfb::kAfbMessageType) {
      webrtc::rtcp::Remb remb;
      if (remb.Parse(header)) {
        RTC_LOG(LS_INFO) << "SFU remote bitrate transport=" << impl_.get()
                         << " bps=" << remb.bitrate_bps();
        impl_->apply(impl_->controller->OnRemoteBitrateReport(
            {impl_->env.clock().CurrentTime(), webrtc::DataRate::BitsPerSec(remb.bitrate_bps())}));
      }
    }
    cursor = header.NextPacket();
  }
}

webrtc::TimeDelta NativeSendTransport::process() {
  return impl_->process();
}

NativeSendTransport::Stats NativeSendTransport::stats() const {
  auto result = impl_->counters;
  result.pacing_bps = impl_->pacer.pacing_rate().bps();
  result.queue_bytes = impl_->pacer.QueueSizeData().bytes();
  result.queue_ms = impl_->pacer.ExpectedQueueTime().ms();
  return result;
}
}  // namespace lk
