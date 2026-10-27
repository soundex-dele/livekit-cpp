#include "lk/subscription_permissions.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
}
int main() {
  try {
    lk::SubscriptionPermissions acl;
    std::map<std::string, std::string> participants{{"PA_1", "bob"}};
    auto resolve = [&](const std::string& sid) { auto it = participants.find(sid); return it == participants.end() ? "" : it->second; };
    check(acl.allows("bob", "TR_1"), "Default must allow all");
    livekit::SubscriptionPermission policy;
    check(acl.update(policy, resolve) && !acl.allows("bob", "TR_1"), "Empty deny policy must deny all");
    auto* grant = policy.add_track_permissions(); grant->set_participant_identity("bob"); grant->add_track_sids("TR_1");
    check(acl.update(policy, resolve) && acl.allows("bob", "TR_1") && !acl.allows("bob", "TR_2") && !acl.allows("alice", "TR_1"), "Per-track isolation failed");
    grant->clear_participant_identity(); grant->set_participant_sid("PA_1"); grant->set_all_tracks(true);
    check(acl.update(policy, resolve) && acl.allows("bob", "future"), "SID resolution failed");
    participants.clear();
    check(acl.allows("bob", "future"), "Resolved SID must remain an identity grant after disconnect");
    check(acl.update(policy, resolve) && !acl.allows("bob", "TR_1"), "Unknown SID was retained on replacement");
    grant->set_participant_identity("alice"); participants["PA_1"] = "bob";
    check(acl.update(policy, resolve) && acl.allows("alice", "TR_2") && !acl.allows("bob", "TR_2"), "Explicit identity must win over SID");
    auto* last = policy.add_track_permissions(); last->set_participant_identity("alice"); last->add_track_sids("TR_3");
    check(acl.update(policy, resolve) && !acl.allows("alice", "TR_2") && acl.allows("alice", "TR_3"), "Duplicate entry must replace not merge");
    policy.mutable_track_permissions(0)->set_participant_identity("bob");
    policy.add_track_permissions();
    check(!acl.update(policy, resolve) && acl.allows("alice", "TR_3") && !acl.allows("bob", "TR_1"), "Invalid entry partially replaced policy");
    policy.set_all_participants(true);
    check(acl.update(policy, resolve) && acl.allows("anyone", "future"), "All-participants must override malformed entries");
    std::cout << "PASS: subscription ACL defaults, isolation, SID resolution, replacement and atomic rejection\n";
  } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
