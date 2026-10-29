#pragma once
#include "livekit_models.pb.h"
#include <algorithm>
#include <deque>
#include <set>
#include <string>

namespace lk {
// Matches the room-wide, insertion-ordered 100-entry UUID cache in Go.
// Invalid UUID byte lengths bypass deduplication; a duplicate does not refresh
// its position. The zero UUID is the Go implementation's initial sentinel.
class UserPacketDeduper {
 public:
  UserPacketDeduper() : order_{std::string(16, '\0')}, seen_{order_.front()} {}

  bool duplicate(const std::string& nonce) {
    if (nonce.size() != 16)
      return false;
    if (seen_.contains(nonce))
      return true;
    order_.push_back(nonce);
    seen_.insert(nonce);
    if (order_.size() > 100) {
      seen_.erase(order_.front());
      order_.pop_front();
    }
    return false;
  }

 private:
  std::deque<std::string> order_;
  std::set<std::string> seen_;
};

inline bool prepare_client_data(livekit::DataPacket& packet, const std::string& sid,
                                const std::string& identity, bool hidden, bool agent, bool reliable,
                                uint32_t& last_sequence) {
  // The channel is authoritative; zero remains the legacy unsequenced form.
  if (reliable && packet.sequence()) {
    if (packet.sequence() <= last_sequence)
      return false;
    last_sequence = packet.sequence();
  }
  packet.set_kind(reliable ? livekit::DataPacket::RELIABLE : livekit::DataPacket::LOSSY);
  packet.set_participant_sid(sid);
  bool represented =
      agent && !packet.participant_identity().empty() && packet.participant_identity() != identity;
  if (packet.has_chat_message() && represented)
    packet.mutable_chat_message()->set_generated(true);
  else if (packet.has_stream_header() && packet.stream_header().has_text_header() && represented)
    packet.mutable_stream_header()->mutable_text_header()->set_generated(true);
  else
    represented = false;
  if (!represented)
    packet.set_participant_identity(hidden ? "" : identity);
  if (packet.has_user()) {
    packet.mutable_user()->set_participant_identity(hidden ? "" : identity);
    packet.mutable_user()->set_participant_sid(hidden ? "" : sid);
  }
  if (packet.has_transcription() && !agent)
    return false;
  // Metrics have a separate permission/subscription/aggregation path in Go.
  // Until that path exists, never leak them to general data subscribers.
  if (packet.has_metrics())
    return false;
  return true;
}

inline void normalize_data_destinations(livekit::DataPacket& packet) {
  if (!packet.has_user())
    return;
  if (packet.destination_identities().empty())
    *packet.mutable_destination_identities() = packet.user().destination_identities();
  else
    *packet.mutable_user()->mutable_destination_identities() = packet.destination_identities();
}

inline bool data_destination_matches(const livekit::DataPacket& packet, const std::string& identity,
                                     const std::string& sid) {
  const auto& identities = packet.destination_identities();
  const auto& sids = packet.user().destination_sids();
  return (identities.empty() && sids.empty()) ||
         std::find(identities.begin(), identities.end(), identity) != identities.end() ||
         std::find(sids.begin(), sids.end(), sid) != sids.end();
}
}  // namespace lk
