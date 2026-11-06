#include "lk/av_control.hpp"
#include "lk/rooms.hpp"

#include <iostream>
#if defined(_WIN32)
#include <rtc_base/win32_socket_init.h>
#endif

namespace {
int failures = 0;

void check(bool value, const char* message) {
  if (!value) {
    std::cerr << message << '\n';
    ++failures;
  }
}
}  // namespace

int main() {
  try {
    lk::Config config;
    config.av_mode = true;
    config.node_id = "node-a";
    config.node_incarnation = "boot-a";
    config.av_rtc_key = "z-rtc";
    config.av_management_key = "a-management";
    config.keys = {{"a-management", "management-secret"}, {"z-rtc", "rtc-secret"}};
    lk::Auth auth(config.keys);
    const lk::Json token{{"iss", "z-rtc"},
                         {"sub", "participant-a"},
                         {"exp", 1600},
                         {"av_grant", "grant-a"},
                         {"av_policy_version", 2},
                         {"av_binding_generation", 7},
                         {"av_node_id", "node-a"},
                         {"av_node_incarnation", "boot-a"},
                         {"av_grant_expires_at", 1800},
                         {"video",
                          {{"roomJoin", true},
                           {"room", "room-a"},
                           {"canPublish", false},
                           {"canPublishData", false}}}};
    auto claims = auth.verify(lk::Auth::sign(token, "rtc-secret"), 1500);
    check(lk::av_rtc_claims_valid(config, claims, 1500), "valid AV claims rejected");
    check(!lk::av_management_claims_valid(config, claims), "RTC key accepted for management");
    const auto refreshed = auth.verify(auth.refresh(claims, 1590), 1590);
    check(refreshed.issuer == "z-rtc", "refresh switched to first/management signer");
    check(refreshed.expires_at == 1650 && !refreshed.publish && !refreshed.publish_data,
          "refresh widened capabilities or exceeded 60-second credential TTL");
    check(lk::av_same_authorization(refreshed, claims), "refresh dropped binding/version");
    check(auth.verify(auth.refresh(claims, 1790), 1790).expires_at == 1800,
          "refresh exceeded absolute authorization expiration");
    check(auth.refresh(claims, 1800).empty(), "expired authorization refreshed");
    auto altered = claims;
    altered.av_grant.clear();
    check(!lk::av_rtc_claims_valid(config, altered, 1500), "missing grant accepted");
    altered = claims;
    altered.issuer = "a-management";
    check(!lk::av_rtc_claims_valid(config, altered, 1500), "management key accepted for RTC");
    altered = claims;
    altered.av_node_incarnation = "previous-boot";
    check(!lk::av_rtc_claims_valid(config, altered, 1500), "old node incarnation accepted");
    altered = claims;
    ++altered.av_policy_version;
    check(!lk::av_same_authorization(claims, altered), "resume may change permission version");
    check(!lk::av_rtc_claims_valid(config, claims, 1601),
          "AV claims inherited legacy expiry grace");

#if defined(_WIN32)
    webrtc::WinsockInitializer winsock;
#endif
    boost::asio::io_context io;
    lk::RtcRuntime rtc(false);
    lk::Rooms rooms(io, rtc, config);
    lk::Claims admin;
    admin.issuer = "a-management";
    admin.admin = true;
    admin.create = true;
    admin.list = true;
    admin.room = "room-a";
    lk::Json operation{{"operation_id", "create-a"},   {"node_id", "node-a"},
                       {"node_incarnation", "boot-a"}, {"room", "room-a"},
                       {"binding_generation", 7},      {"target_policy_revision", 1},
                       {"method", "CreateRoom"},       {"payload", {{"name", "room-a"}}}};
    int status = 0;
    auto created = rooms.av_operation(operation, admin, status);
    check(status == 200, "create operation failed");
    check(rooms.av_operation(operation, admin, status) == created && status == 200,
          "identical operation not idempotent");
    operation["payload"]["metadata"] = "changed";
    rooms.av_operation(operation, admin, status);
    check(status == 409, "same operation key accepted changed payload");
    operation["payload"].erase("metadata");
    auto wrong_node = operation;
    wrong_node["node_incarnation"] = "old";
    rooms.av_operation(wrong_node, admin, status);
    check(status == 409, "old node operation accepted");
    auto wrong_generation = operation;
    wrong_generation["binding_generation"] = 8;
    rooms.av_operation(wrong_generation, admin, status);
    check(status == 409, "room name reused across binding generation");
    auto remove_identity = operation;
    remove_identity["method"] = "RemoveParticipant";
    remove_identity["operation_id"] = "remove-identity";
    remove_identity["target_policy_revision"] = 2;
    remove_identity["payload"] = {{"room", "room-a"}, {"identity", "participant-a"}};
    rooms.av_operation(remove_identity, admin, status);
    check(status == 200, "remove of absent identity did not record a tombstone");
    auto late_claims = claims;
    late_claims.av_policy_version = 100;
    late_claims.expires_at = lk::now_ms() / 1000 + 60;
    late_claims.av_grant_expires_at = late_claims.expires_at + 60;
    check(!rooms.join(late_claims, false, {}),
          "removed identity resurrected using a numerically higher policy version");
    auto restore_identity = remove_identity;
    restore_identity["operation_id"] = "restore-identity";
    restore_identity["method"] = "UpdateParticipant";
    restore_identity["target_policy_revision"] = 101;
    rooms.av_operation(restore_identity, admin, status);
    check(status == 409, "removed identity accepted a later permission update");
    auto update = operation;
    update["method"] = "UpdateRoomMetadata";
    update["operation_id"] = "update-a";
    update["target_policy_revision"] = 3;
    update["payload"] = {{"room", "room-a"}, {"metadata", "latest"}};
    rooms.av_operation(update, admin, status);
    check(status == 200, "metadata operation failed");
    update["operation_id"] = "delayed-a";
    update["target_policy_revision"] = 2;
    update["payload"]["metadata"] = "old";
    rooms.av_operation(update, admin, status);
    check(status == 409, "delayed old command changed room");
    check(
        rooms.api("ListRooms", {{"names", {"room-a"}}}, admin)["rooms"][0]["metadata"] == "latest",
        "stale operation mutated media state");
    auto remove = operation;
    remove["method"] = "DeleteRoom";
    remove["operation_id"] = "delete-a";
    remove["target_policy_revision"] = 4;
    remove["payload"] = {{"room", "room-a"}};
    rooms.av_operation(remove, admin, status);
    check(status == 200, "delete operation failed");
    operation["operation_id"] = "late-create";
    operation["target_policy_revision"] = 5;
    rooms.av_operation(operation, admin, status);
    check(status == 409, "sealed room resurrected by late create");
    rooms.av_operation(remove, admin, status);
    check(status == 200, "delete retry not idempotent");
    rooms.av_operation(remove, claims, status);
    check(status == 403, "RTC claims authorized operation");
    rooms.shutdown();
  } catch (const std::exception& error) {
    // Legacy authentication/media constructors are an exception boundary.
    std::cerr << error.what() << '\n';
    return 1;
  }
  return failures == 0 ? 0 : 1;
}
