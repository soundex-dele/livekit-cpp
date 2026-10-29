#pragma once
#include <functional>
#include <map>
#include <set>
#include <string>
#pragma push_macro("PASSTHROUGH")
#undef PASSTHROUGH
#include "livekit_rtc.pb.h"
#pragma pop_macro("PASSTHROUGH")

namespace lk {
// Room-thread state. Resolve SID grants once, to identities, like Go's
// UpTrackManager. A failed update must leave the previous policy intact.
class SubscriptionPermissions {
 public:
  using Resolve = std::function<std::string(const std::string&)>;

  bool update(const livekit::SubscriptionPermission& policy, const Resolve& resolve) {
    if (policy.all_participants()) {
      all_ = true;
      entries_.clear();
      return true;
    }
    std::map<std::string, Entry> next;
    for (const auto& grant : policy.track_permissions()) {
      auto identity = grant.participant_identity();
      if (identity.empty()) {
        if (grant.participant_sid().empty())
          return false;
        identity = resolve(grant.participant_sid());
        if (identity.empty())
          continue;
      }
      // An explicit identity wins even if the supplied SID belongs to another
      // participant. Repeated entries replace, rather than merge, in Go.
      next[identity] = {grant.all_tracks(), {grant.track_sids().begin(), grant.track_sids().end()}};
    }
    entries_ = std::move(next);
    all_ = false;
    return true;
  }

  bool allows(const std::string& identity, const std::string& track) const {
    if (all_)
      return true;
    auto it = entries_.find(identity);
    return it != entries_.end() && (it->second.all || it->second.tracks.contains(track));
  }

 private:
  struct Entry {
    bool all;
    std::set<std::string> tracks;
  };

  bool all_ = true;
  std::map<std::string, Entry> entries_;
};
}  // namespace lk
