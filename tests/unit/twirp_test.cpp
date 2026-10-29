#include "lk/twirp.hpp"
#include "livekit_room.pb.h"
#include <iostream>

namespace {
void check(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}

template <typename T>
void malformed(const std::string& json) {
  T message;
  try {
    lk::parse_twirp_json(json, message);
  } catch (const lk::Error& e) {
    check(e.status == 400 && e.code == "malformed", "Wrong decoding error contract");
    return;
  }
  throw std::runtime_error("Invalid request accepted: " + json);
}
}  // namespace

int main() {
  try {
    livekit::CreateRoomRequest room;
    lk::parse_twirp_json(
        R"( {"name":"braces } [ and \"quotes\"","emptyTimeout":30} {"ignored":true})", room);
    check(room.name() == "braces } [ and \"quotes\"" && room.empty_timeout() == 30,
          "First JSON value parsing changed");
    malformed<livekit::CreateRoomRequest>(R"({"empty_timeout":30,"emptyTimeout":60})");
    malformed<livekit::ListRoomsRequest>(R"({"names":[],"names":[]})");
    malformed<livekit::UpdateParticipantRequest>(
        R"({"permission":{"canPublish":true,"can_publish":false}})");
    malformed<livekit::UpdateParticipantRequest>(R"({"attributes":{"key":"one","key":"two"}})");
    malformed<livekit::UpdateSubscriptionsRequest>(
        R"({"participantTracks":[{"trackSids":[],"track_sids":[]}]})");
    for (const auto* json :
         {"", "null", "[]", "{", "{]", "{\"names\":42}", "{\"names\":[] garbage}"})
      malformed<livekit::ListRoomsRequest>(json);
    livekit::ListRoomsRequest list;
    lk::parse_twirp_json(R"({"names":null,"future":{"x":1,"x":2},"future":2})", list);
    check(list.names().empty(), "Unknown fields or null repeated value rejected");
    livekit::UpdateParticipantRequest participant;
    lk::parse_twirp_json(
        R"({"attributes":{"name":"a","permission":"b"},"permission":{"canPublish":true}})",
        participant);
    check(participant.attributes().at("name") == "a" && participant.permission().can_publish(),
          "Map context leaked into message fields");
    livekit::ListRoomsResponse rooms;
    rooms.add_rooms()->set_name("test");
    auto response = lk::Json::parse(lk::print_twirp_json(rooms));
    check(response["rooms"][0]["version"].is_null() &&
              response["rooms"][0]["enabled_codecs"].is_array(),
          "Go unpopulated message/repeated output missing");
    livekit::ParticipantInfo info;
    response = lk::Json::parse(lk::print_twirp_json(info));
    check(response["permission"].is_null(), "Unset nested message not printed as null");
    livekit::SendDataRequest data;
    response = lk::Json::parse(lk::print_twirp_json(data));
    check(!response.contains("topic"), "Unset proto3 optional field must stay absent");
    std::cout << "PASS Twirp JSON framing, duplicate aliases/maps/nested fields and unpopulated "
                 "messages\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
