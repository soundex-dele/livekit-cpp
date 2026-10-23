#include "lk/rooms.hpp"
#include <iostream>
#include <stdexcept>
#if defined(_WIN32)
#include <rtc_base/win32_socket_init.h>
#endif
namespace {
void check(bool value, const char* what) { if (!value) throw std::runtime_error(what); }
template <typename F> void rejected(F f, int status) {
  try { f(); } catch (const lk::Error& e) { check(e.status == status, "Unexpected error status"); return; }
  throw std::runtime_error("Expected operation to be rejected");
}
struct Sink : lk::SignalSink {
  std::vector<livekit::SignalResponse> messages;
  bool closed = false;
  void send(const livekit::SignalResponse& response) override { messages.push_back(response); }
  void close() override { closed = true; }
};
}
int main() {
  try {
    lk::Auth auth({{"key", "secret"}});
    lk::Json payload = {{"iss", "key"}, {"sub", "alice"}, {"exp", 2000}, {"nbf", 1000}, {"video", {{"roomJoin", true}, {"room", "demo"}, {"canPublish", false}}}};
    auto token = lk::Auth::sign(payload, "secret"); auto claims = auth.verify(token, 1500);
    check(claims.join && !claims.publish && claims.subscribe && claims.room == "demo", "Grants differ from token");
    rejected([&] { auth.verify(token, 2000); }, 401);
    rejected([&] { auth.verify(token, 999); }, 401);
    rejected([&] { auth.verify(lk::Auth::sign(payload, "wrong"), 1500); }, 401);
    rejected([&] { auth.verify("eyJhbGciOiJub25lIn0.e30.", 1500); }, 401);
    rejected([&] { auth.verify(token + ".extra", 1500); }, 401);
    payload["video"]["canPublishData"] = false;
    check(!auth.verify(lk::Auth::sign(payload, "secret"), 1500).publish_data, "Data permission ignored");
    livekit::SignalRequest request; request.mutable_ping_req()->set_timestamp(123456);
    livekit::SignalRequest decoded; check(decoded.ParseFromString(request.SerializeAsString()) && decoded.ping_req().timestamp() == 123456, "Protobuf roundtrip failed");
#if defined(_WIN32)
    webrtc::WinsockInitializer winsock;
#endif
    boost::asio::io_context io; lk::RtcRuntime rtc; lk::Config config; config.keys = {{"key", "secret"}};
    lk::Rooms rooms(io, rtc, config);
    lk::Claims admin; admin.create = true; admin.list = true; admin.admin = true; admin.room = "demo";
    auto room = rooms.api("CreateRoom", {{"name", "demo"}, {"maxParticipants", 1}}, admin);
    check(room["sid"].get<std::string>().starts_with("RM_"), "Room SID missing");
    check(rooms.api("CreateRoom", {{"name", "demo"}}, admin)["sid"] == room["sid"], "CreateRoom not idempotent");
    rejected([&] { rooms.api("ListRooms", lk::Json::object(), claims); }, 403);
    auto sink = std::make_shared<Sink>(); auto p = rooms.join(claims, true, sink);
    check(sink->messages.front().has_join() && sink->messages.front().join().subscriber_primary(), "Join response invalid");
    auto has_offer = [&] { for (const auto& message : sink->messages) if (message.has_offer()) return true; return false; };
    const auto deadline = lk::now_ms() + 5000;
    while (!has_offer() && !p->closed && lk::now_ms() < deadline) io.run_for(std::chrono::milliseconds(100));
    check(has_offer(), "Native subscriber did not produce a data-channel SDP offer");
    lk::Claims bob = claims; bob.identity = "bob";
    rejected([&] { rooms.join(bob, true, std::make_shared<Sink>()); }, 429);
    request.Clear(); auto* track = request.mutable_add_track(); track->set_cid("camera"); track->set_type(livekit::VIDEO); track->set_source(livekit::CAMERA);
    rooms.signal(p, request);
    check(p->tracks.empty() && sink->messages.back().request_response().reason() == livekit::RequestResponse::NOT_ALLOWED, "Publish denial not enforced");
    lk::Claims wrong_room = admin; wrong_room.room = "other";
    rejected([&] { rooms.api("ListParticipants", {{"room", "demo"}}, wrong_room); }, 403);
    auto replacement_sink = std::make_shared<Sink>(); auto replacement = rooms.join(claims, true, replacement_sink);
    check(p->closed && sink->closed && replacement->info.sid() != p->info.sid(), "Duplicate identity not replaced");
    check(rooms.api("ListParticipants", {{"room", "demo"}}, admin)["participants"].size() == 1, "Wrong participant count");
    rooms.api("DeleteRoom", {{"room", "demo"}}, admin);
    check(replacement->closed && rooms.metrics()["rooms"] == 0, "DeleteRoom did not disconnect participants");
    rooms.shutdown(); io.poll();
    std::cout << "PASS: JWT, permissions, protobuf, native PeerConnection, room lifecycle, duplicate identity and cleanup\n";
  } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
