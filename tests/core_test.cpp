#include "lk/rooms.hpp"
#include "lk/video_allocation.hpp"
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
void allocation_test() {
  std::vector<lk::VideoBandwidthRequest> startup{{"new",0,{}}};
  lk::allocate_video_bandwidth(300000,64000,0,startup);
  startup[0].layers={{11,300000}};
  lk::allocate_video_bandwidth(300000,64000,200,startup);
  check(startup[0].state.selected==0 && !startup[0].state.initialized,"Undiscovered/unaffordable source incorrectly marked started");
  lk::allocate_video_bandwidth(1000000,64000,400,startup);
  check(startup[0].state.selected==11,"First affordable base layer was delayed by recovery hysteresis");
  lk::allocate_video_bandwidth(300000,64000,500,startup);
  lk::allocate_video_bandwidth(1000000,64000,600,startup);
  lk::allocate_video_bandwidth(1000000,64000,799,startup);
  check(!startup[0].state.selected,"Fast resume skipped its sustained-headroom interval");
  lk::allocate_video_bandwidth(1000000,64000,800,startup);
  check(startup[0].state.selected==11,"Ample native bandwidth failed to resume video promptly");
  lk::allocate_video_bandwidth(300000,64000,900,startup);
  lk::allocate_video_bandwidth(600000,64000,1000,startup);
  lk::allocate_video_bandwidth(1000000,64000,1800,startup);
  lk::allocate_video_bandwidth(1000000,64000,1999,startup);
  check(!startup[0].state.selected,"Fast resume reused time accrued with insufficient headroom");
  lk::allocate_video_bandwidth(600000,64000,2000,startup);
  lk::allocate_video_bandwidth(600000,64000,2999,startup);
  check(!startup[0].state.selected,"Lost ample headroom failed to restore normal recovery hysteresis");
  lk::allocate_video_bandwidth(600000,64000,3000,startup);
  check(startup[0].state.selected==11,"Normal recovery failed after headroom stabilized");
  std::vector<lk::VideoBandwidthRequest> videos{
    {"default",0,{{11,100000},{12,300000},{13,600000}}},
    {"speaker",1,{{21,100000},{22,300000},{23,600000}}}};
  auto allocate=[&](int64_t target,int64_t now,int64_t audio=64000) {
    auto result=lk::allocate_video_bandwidth(target,audio,now,videos);
    check(result.audio_reserved_bps+result.video_allocated_bps<=result.media_budget_bps,"Allocator overspent shared target");
    return result;
  };
  allocate(1100000,0);
  check(videos[0].state.selected==11 && videos[1].state.selected==21,"Initial allocation must start with base layers");
  allocate(1100000,999);
  check(videos[1].state.selected==21,"Upgrade occurred before sustained headroom");
  allocate(1100000,1000);
  check(videos[0].state.selected==11 && videos[1].state.selected==23,"Priority did not select active speaker upgrade");
  auto constrained=allocate(250000,1100);
  check(constrained.paused==1 && videos[0].state.selected==0 && videos[1].state.selected==21,"Shortage must preserve audio then highest-priority base video");
  check(allocate(60000,1200).video_allocated_bps==0,"Video consumed audio-only budget");
  allocate(1100000,1300); allocate(60000,1800); allocate(1100000,2000); allocate(1100000,2500);
  check(videos[1].state.selected==0,"Oscillation did not reset upgrade hysteresis");
  allocate(1100000,3000);
  check(videos[1].state.selected==23,"Video did not resume after sustained headroom");
  videos[1].layers.resize(2); allocate(1100000,3050);
  check(videos[1].state.selected==22 && videos[1].state.pending==0,"Removed high layer caused redundant base downgrade and upgrade wait");
  videos[1].layers.resize(1); allocate(1100000,3100);
  check(videos[1].state.selected==21,"User quality cap retained an unavailable high layer");
  videos[0].layers.clear(); allocate(1100000,3200);
  check(videos[0].state.selected==0,"Removed source retained allocation");
  // Changing source rates and priorities must never create an aggregate budget
  // overrun, including equal/non-monotonic measured layer rates.
  for (int i=0;i<2000;++i) {
    videos[0].layers={{11,20000+(i*7919)%300000},{12,15000+(i*3571)%500000}};
    videos[1].layers={{21,40000+(i*1297)%200000},{22,50000+(i*5119)%700000}};
    videos[0].priority=uint32_t(i%3); videos[1].priority=uint32_t((i+1)%3);
    allocate((i*4567)%2000000,4000+200*i,(i%4)*64000);
  }
}
}
int main() {
  try {
    allocation_test();
    std::vector<lk::VideoLayerChoice> layers{{33,1280,720,2},{11,320,180,0},{22,640,360,1}};
    check(lk::select_video_layer(layers,{0,0,0})==11,"LOW subscription did not select base layer");
    check(lk::select_video_layer(layers,{1,0,0})==22,"MEDIUM subscription did not select middle layer");
    check(lk::select_video_layer(layers,{2,0,0})==33,"HIGH subscription did not select highest layer");
    check(lk::select_video_layer(layers,{0,600,300})==22,"Dimensions must override deprecated quality");
    check(lk::select_video_layer(layers,{2,0,100})==11,"Height-only subscription did not choose smallest sufficient layer");
    check(lk::select_video_layer(layers,{0,4000,2000})==33,"Oversized viewport did not select highest available layer");
    check(lk::select_video_layer({layers.front()},{0,0,0})==33,"Single-layer source cannot synthesize a lower layer");
    check(lk::select_video_layer({}, {})==0,"Empty source selected a layer");
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
    config.turn_rest = lk::TurnRestAuth{{"turn:127.0.0.1:9?transport=udp"},"0123456789abcdef0123456789abcdef",3600};
    lk::Rooms rooms(io, rtc, config);
    lk::Claims admin; admin.create = true; admin.list = true; admin.admin = true; admin.room = "demo";
    auto room = rooms.api("CreateRoom", {{"name", "demo"}, {"maxParticipants", 1}}, admin);
    check(room["sid"].get<std::string>().starts_with("RM_"), "Room SID missing");
    check(rooms.api("CreateRoom", {{"name", "demo"}}, admin)["sid"] == room["sid"], "CreateRoom not idempotent");
    rejected([&] { rooms.api("ListRooms", lk::Json::object(), claims); }, 403);
    auto sink = std::make_shared<Sink>(); auto p = rooms.join(claims, true, sink);
    check(sink->messages.front().has_join() && sink->messages.front().join().subscriber_primary(), "Join response invalid");
    const auto join_ice=sink->messages.front().join().ice_servers();
    check(join_ice.size()==1 && join_ice[0].username().ends_with(":"+p->info.sid()) &&
          join_ice[0].credential()!=config.turn_rest->shared_secret && join_ice[0].credential().size()==28,
          "Join response did not contain session-scoped TURN credentials");
    check(sink->messages.front().SerializeAsString().find(config.turn_rest->shared_secret)==std::string::npos,
          "Join response exposed the TURN signing secret");
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
    check(replacement_sink->messages.front().join().ice_servers(0).username()!=join_ice[0].username(),
          "Rejoining reused the previous TURN session credentials");
    check(rooms.api("ListParticipants", {{"room", "demo"}}, admin)["participants"].size() == 1, "Wrong participant count");
    rooms.api("DeleteRoom", {{"room", "demo"}}, admin);
    check(replacement->closed && rooms.metrics()["rooms"] == 0, "DeleteRoom did not disconnect participants");
    auto pub_claims=claims; pub_claims.room="layers"; pub_claims.publish=true;
    auto pub_sink=std::make_shared<Sink>(); auto pub=rooms.join(pub_claims,true,pub_sink);
    auto sub_claims=claims; sub_claims.room="layers"; sub_claims.identity="viewer";
    auto sub_sink=std::make_shared<Sink>(); auto sub=rooms.join(sub_claims,true,sub_sink);
    request.Clear(); auto* layered=request.mutable_add_track(); layered->set_cid("simulcast"); layered->set_type(livekit::VIDEO);
    for (int i=0;i<3;++i) { auto* layer=layered->add_layers(); layer->set_quality(livekit::VideoQuality(i)); layer->set_width(320u<<i); layer->set_height(180u<<i); }
    rooms.signal(pub,request);
    check(pub->tracks.size()==1 && std::any_of(pub_sink->messages.begin(),pub_sink->messages.end(),[](const auto& message){return message.has_track_published();}),"Layered publication rejected");
    auto sid=pub->tracks.begin()->first;
    check(pub->tracks.begin()->second.info.simulcast() && pub->tracks.begin()->second.info.codecs(0).layers_size()==3,"Layer metadata not preserved");
    layered->set_cid("bad-layers"); layered->mutable_layers(1)->set_quality(livekit::LOW);
    rooms.signal(pub,request);
    check(pub->tracks.size()==1 && pub_sink->messages.back().request_response().reason()==livekit::RequestResponse::NOT_ALLOWED,"Duplicate layer quality accepted");
    request.Clear(); auto* setting=request.mutable_track_setting(); setting->add_track_sids(sid); setting->set_width(600); setting->set_height(300);
    setting->set_priority(1); rooms.signal(sub,request);
    check(sub->track_settings.at(sid).width==600 && sub->track_settings.at(sid).priority==1,"Subscription dimensions/priority not retained before media arrives");
    setting->set_disabled(true); rooms.signal(sub,request);
    check(sub->disabled_tracks.contains(sid),"Subscription disable setting ignored");
    setting->set_disabled(false); setting->set_fps(10); rooms.signal(sub,request);
    check(sub_sink->messages.back().request_response().reason()==livekit::RequestResponse::UNSUPPORTED_TYPE && sub->disabled_tracks.contains(sid),"Unsupported fps partially applied settings");
    setting->set_fps(0); setting->set_width(16385);
    rejected([&]{rooms.signal(sub,request);},400);
    setting->set_width(600); setting->clear_track_sids(); setting->add_track_sids("unknown"); rooms.signal(sub,request);
    check(sub->track_settings.size()==1,"Unknown track grew subscription settings");
    rooms.leave(pub);
    check(sub->track_settings.empty() && sub->disabled_tracks.empty(),"Publisher departure retained subscription policy");
    rooms.shutdown(); io.poll();
    std::cout << "PASS: JWT, permissions, protobuf, native PeerConnection, room lifecycle, duplicate identity and cleanup\n";
  } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
