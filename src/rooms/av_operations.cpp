#include "lk/rooms.hpp"
#include "lk/av_control.hpp"

#include <set>

namespace lk {

Json Rooms::av_operation(const Json& input, const Claims& claims, int& status) {
  auto fail = [&](int code, const char* reason) {
    status = code;
    return Json{{"error", {{"code", reason}}}};
  };
  if (!av_management_claims_valid(config_, claims)) {
    return fail(403, "management_credential_required");
  }
  if (!input.is_object()) {
    return fail(400, "invalid_operation");
  }
  for (const auto* field : {"operation_id", "node_id", "node_incarnation", "room", "method"}) {
    if (!input.value(field, Json()).is_string() ||
        input[field].get_ref<const std::string&>().empty()) {
      return fail(400, "invalid_operation");
    }
  }
  if (!input.value("binding_generation", Json()).is_number_integer() ||
      !input.value("target_policy_revision", Json()).is_number_integer() ||
      !input.value("payload", Json()).is_object()) {
    return fail(400, "invalid_operation");
  }
  const auto room_name = input["room"].get<std::string>();
  const auto operation_id = input["operation_id"].get<std::string>();
  const auto method = input["method"].get<std::string>();
  const auto generation = input["binding_generation"].get<int64_t>();
  const auto revision = input["target_policy_revision"].get<int64_t>();
  auto payload = input["payload"];
  if (generation <= 0 || revision <= 0 || room_name.size() > 256 || operation_id.size() > 128) {
    return fail(400, "invalid_operation");
  }
  if (input["node_id"] != config_.node_id ||
      input["node_incarnation"] != config_.node_incarnation) {
    return fail(409, "node_incarnation_mismatch");
  }
  static const std::set<std::string> methods = {
      "CreateRoom",         "DeleteRoom",         "RemoveParticipant",  "UpdateParticipant",
      "MutePublishedTrack", "UpdateRoomMetadata", "UpdateSubscriptions"};
  if (!methods.count(method)) {
    return fail(400, "unsupported_operation");
  }
  const auto room_field = method == "CreateRoom" ? "name" : "room";
  if (payload.value(room_field, Json()) != room_name) {
    return fail(400, "room_mismatch");
  }
  if (method == "CreateRoom" || method == "DeleteRoom") {
    if (!claims.create) {
      return fail(403, "room_create_permission_required");
    }
  } else if (!claims.admin || claims.room != room_name) {
    return fail(403, "room_admin_permission_required");
  }
  const bool participant_target = method == "RemoveParticipant" || method == "UpdateParticipant" ||
                                  method == "MutePublishedTrack" || method == "UpdateSubscriptions";
  if (participant_target && (!payload.value("identity", Json()).is_string() ||
                             payload["identity"].get_ref<const std::string&>().empty())) {
    return fail(400, "identity_required");
  }
  const auto target = participant_target ? payload["identity"].get<std::string>() : std::string();
  auto existing = av_fences_.find(room_name);
  if (existing == av_fences_.end()) {
    if (method != "CreateRoom" && method != "DeleteRoom") {
      return fail(409, "room_not_prepared");
    }
    // Bound lifecycle tombstones; never evict one and allow an old create to
    // run again. A fresh node incarnation makes all old commands invalid.
    if (av_fences_.size() >= 100000) {
      return fail(503, "node_lifecycle_capacity");
    }
    existing = av_fences_.emplace(room_name, AvRoomFence{}).first;
    existing->second.generation = generation;
  }
  auto& fence = existing->second;
  if (fence.generation != generation) {
    return fail(409, "binding_generation_mismatch");
  }
  if (!fence.targets.count(target) && fence.targets.size() >= 100000) {
    return fail(503, "room_lifecycle_capacity");
  }
  auto& checkpoint = fence.targets[target];
  const auto request = input.dump();
  if (revision < checkpoint.revision ||
      (revision == checkpoint.revision &&
       (checkpoint.operation_id != operation_id || checkpoint.request != request))) {
    return fail(409, "stale_operation");
  }
  if (revision == checkpoint.revision && !checkpoint.result.is_null()) {
    status = 200;
    return checkpoint.result;
  }
  if (checkpoint.removed && method != "RemoveParticipant") {
    return fail(409, "participant_ended");
  }
  if (fence.sealed) {
    if (method == "DeleteRoom") {
      status = 200;
      return Json{
          {"operation_id", operation_id}, {"state", "succeeded"}, {"result", Json::object()}};
    }
    return fail(409, "room_ended");
  }
  checkpoint.revision = revision;
  checkpoint.operation_id = operation_id;
  checkpoint.request = request;
  checkpoint.result = Json();
  Json result = Json::object();
  // Existing RoomService performs protobuf conversion and uses legacy Error
  // exceptions. Convert that boundary into an explicit operation result.
  try {
    result = api(method, payload, claims);
  } catch (const Error& error) {
    if (error.status != 404 || (method != "DeleteRoom" && method != "RemoveParticipant")) {
      status = error.status;
      return Json{{"error", {{"code", error.code}}}};
    }
  } catch (const std::exception&) {
    return fail(500, "media_operation_failed");
  }
  if (method == "DeleteRoom") {
    fence.sealed = true;
  }
  if (method == "RemoveParticipant") {
    // An identity belongs to exactly one RTCGrant. A later policy revision
    // cannot revive it; a replacement authorization must use a fresh identity.
    checkpoint.removed = true;
  }
  if (method == "UpdateParticipant") {
    auto found = rooms_.find(room_name);
    if (found != rooms_.end()) {
      auto participant = found->second.participants.find(target);
      if (participant != found->second.participants.end()) {
        participant->second->claims.av_policy_version = revision;
        refresh_token(participant->second);
      }
    }
  }
  checkpoint.result =
      Json{{"operation_id", operation_id}, {"state", "succeeded"}, {"result", result}};
  status = 200;
  return checkpoint.result;
}

}  // namespace lk
