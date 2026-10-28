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
    check(auth.verify(token, 2059).join && auth.verify(token, 940).join, "JWT one minute leeway missing");
    rejected([&] { auth.verify(token, 2060); }, 401);
    rejected([&] { auth.verify(token, 939); }, 401);
    auto missing_exp = payload; missing_exp.erase("exp");
    rejected([&] { auth.verify(lk::Auth::sign(missing_exp, "secret"), 1500); }, 401);
    check(!claims.publish_data, "Unspecified data permission did not inherit canPublish");
    const auto refreshed = auth.verify(auth.refresh(claims, 1800), 1800);
    check(refreshed.expires_at == 2400 && !refreshed.publish_data && refreshed.identity == claims.identity,
          "Refresh lost identity, permissions, or ten minute minimum lifetime");
    auto long_lived = claims; long_lived.expires_at = 9000;
    check(auth.verify(auth.refresh(long_lived, 1800), 1800).expires_at == 9000, "Refresh shortened original lifetime");
    rejected([&] { auth.verify(lk::Auth::sign(payload, "wrong"), 1500); }, 401);
    rejected([&] { auth.verify("eyJhbGciOiJub25lIn0.e30.", 1500); }, 401);
    rejected([&] { auth.verify(token + ".extra", 1500); }, 401);
    payload["video"]["canPublishData"] = false;
    check(!auth.verify(lk::Auth::sign(payload, "secret"), 1500).publish_data, "Data permission ignored");
    payload["video"]["destinationRoom"] = "target";
    check(auth.verify(lk::Auth::sign(payload, "secret"), 1500).destination_room == "target", "Destination room grant ignored");
    payload["video"]["recorder"] = true;
    check(auth.verify(lk::Auth::sign(payload, "secret"), 1500).is_recorder(), "Recorder grant ignored");
    payload["video"]["recorder"] = false; payload["kind"] = "EGRESS";
    check(auth.verify(lk::Auth::sign(payload, "secret"), 1500).is_recorder(), "Case-insensitive egress kind ignored");
    payload["kind"] = "AgEnT"; payload["video"]["hidden"] = true;
    auto hidden_agent = auth.verify(lk::Auth::sign(payload, "secret"), 1500);
    check(hidden_agent.hidden && hidden_agent.is_dependent() && !hidden_agent.is_recorder(), "Hidden agent grants differ from token");
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
    rejected([&] { rooms.api("ListRooms", lk::Json::object(), claims); }, 401);
    const std::string large_metadata(512 * 1024, 'm');
    check(rooms.api("UpdateRoomMetadata", {{"room", "demo"}, {"metadata", large_metadata}}, admin)["metadata"] == large_metadata,
          "Go default metadata limit rejected a valid 512 KiB value");
    rejected([&] { rooms.api("UpdateRoomMetadata", {{"room", "demo"}, {"metadata", large_metadata + "x"}}, admin); }, 400);
    check(rooms.api("ListRooms", {{"names", {"demo"}}}, admin)["rooms"][0]["metadata"] == large_metadata,
          "Rejected metadata update changed room state");
    rooms.api("UpdateRoomMetadata", {{"room", "demo"}, {"metadata", ""}}, admin);
    rooms.api("SendData", {{"room", "demo"}, {"data", "dGVzdA=="}, {"nonce", "AAAAAAAAAAAAAAAAAAAAAA=="}}, admin);
    rejected([&] { rooms.api("SendData", {{"room", "demo"}, {"nonce", "YmFk"}}, admin); }, 400);
    lk::Claims absent_admin = admin; absent_admin.room = "absent";
    auto absent = rooms.api("ListParticipants", {{"room", "absent"}}, absent_admin);
    check(!absent.contains("participants") || absent["participants"].empty(), "Missing room did not return an empty participant list");
    check(rooms.metrics()["rooms"] == 1, "Listing a missing room created it");
    auto empty_name_room = rooms.api("CreateRoom", lk::Json::object(), admin);
    check(empty_name_room["sid"].get<std::string>().starts_with("RM_"), "Go-compatible empty room name rejected by management API");
    rooms.api("DeleteRoom", {{"room", ""}}, admin);
    check(rooms.metrics()["rooms"] == 1, "Empty-name room was not removed");
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
    p->ice_connected = true; // Transport establishment is exercised by the headless differential suite.
    rooms.signal_closed(p, sink.get());
    check(!p->closed && p->sink.expired(), "Signal close destroyed established participant");
    auto resumed_sink = std::make_shared<Sink>();
    const auto original_sid = p->info.sid();
    auto stale_claims = claims; stale_claims.publish = true;
    check(rooms.resume(stale_claims, resumed_sink) == p && p->info.sid() == original_sid && !p->claims.publish,
          "Resume replaced participant or restored stale token grants");
    check(resumed_sink->messages.front().has_reconnect(), "Resume did not send reconnect response first");
    rooms.signal_closed(p, sink.get());
    check(!p->closed && p->sink.lock() == resumed_sink, "Old signal close detached resumed session");
    sink = resumed_sink;
    request.Clear(); request.mutable_sync_state(); rooms.signal(p, request);
    check(!p->closed, "Valid empty SyncState forced full reconnect");
    auto missing_claims = claims; missing_claims.identity = "missing-resume";
    check(!rooms.resume(missing_claims, std::make_shared<Sink>()), "Resume created an unknown participant");
    auto handover_sink = std::make_shared<Sink>();
    check(rooms.resume(claims, handover_sink) == p && sink->closed, "Live signal handover did not retire old sink");
    rooms.signal_closed(p, sink.get());
    check(p->sink.lock() == handover_sink && !p->closed, "Retired live socket closed new session");
    sink = handover_sink;
    lk::Participant cache_probe;
    livekit::SignalResponse cache_update;
    auto* cached_info = cache_update.mutable_update()->add_participants();
    cached_info->set_metadata(std::string(8192, 'x')); cached_info->set_state(livekit::ParticipantInfo::DISCONNECTED);
    for (int i = 0; i < 200; ++i) {
      cached_info->set_sid("PA_cache_" + std::to_string(i)); cache_probe.send(cache_update);
    }
    check(cache_probe.update_cache.size() == 128 && !cache_probe.update_cache.contains("PA_cache_0") &&
          cache_probe.update_cache.at("PA_cache_199").info.metadata().empty(), "Resume cache unbounded or retained peer metadata");
    lk::Claims bob = claims; bob.identity = "bob";
    rejected([&] { rooms.join(bob, true, std::make_shared<Sink>()); }, 429);
    request.Clear(); auto* track = request.mutable_add_track(); track->set_cid("camera"); track->set_type(livekit::VIDEO); track->set_source(livekit::CAMERA);
    rooms.signal(p, request);
    check(p->tracks.empty() && sink->messages.back().request_response().reason() == livekit::RequestResponse::NOT_ALLOWED, "Publish denial not enforced");
    lk::Claims wrong_room = admin; wrong_room.room = "other";
    rejected([&] { rooms.api("ListParticipants", {{"room", "demo"}}, wrong_room); }, 401);
    const std::string attribute_value(65535, 'a');
    auto participant_update = lk::Json{{"room", "demo"}, {"identity", claims.identity},
      {"metadata", large_metadata}, {"attributes", {{"k", attribute_value}}}};
    rooms.api("UpdateParticipant", participant_update, admin);
    check(p->info.metadata() == large_metadata && p->info.attributes().at("k") == attribute_value,
          "Metadata and attribute limits were incorrectly combined");
    participant_update["metadata"] = "must not apply";
    participant_update["attributes"]["k"] = attribute_value + "x";
    rejected([&] { rooms.api("UpdateParticipant", participant_update, admin); }, 400);
    check(p->info.metadata() == large_metadata && p->info.attributes().at("k") == attribute_value,
          "Rejected attribute update partially changed participant state");
    participant_update["attributes"].clear();
    participant_update["name"] = std::string(257, 'n');
    rejected([&] { rooms.api("UpdateParticipant", participant_update, admin); }, 400);
    check(p->info.metadata() == large_metadata, "Rejected participant name partially applied metadata");
    livekit::SendDataRequest oversized; oversized.set_room("demo"); oversized.set_data(std::string(2 * 1024 * 1024, 'x'));
    rooms.api("SendData", lk::to_json(oversized), admin); io.poll();
    check(!p->closed, "Oversized API data disconnected subscriber");
    // A rejected self-update must reply with the original request and leave
    // both state and session alive, even when another supplied field is valid.
    p->client_info.set_protocol(15); p->client_info.set_sdk(livekit::ClientInfo::JS);
    p->claims.update_metadata = true;
    const auto unchanged = p->info.SerializeAsString();
    auto self_rejected = [&](const lk::Json& payload, const std::string& message, livekit::RequestResponse::Reason reason = livekit::RequestResponse::LIMIT_EXCEEDED) {
      livekit::SignalRequest request;
      *request.mutable_update_metadata() = lk::from_json<livekit::UpdateParticipantMetadata>(payload);
      request.mutable_update_metadata()->set_request_id(44);
      const auto count = sink->messages.size(); rooms.signal(p, request);
      check(!p->closed && !sink->closed && p->info.SerializeAsString() == unchanged, "Rejected self-update changed state or closed session");
      check(sink->messages.size() == count + 1, "Rejected self-update emitted state changes or token refresh");
      const auto& response = sink->messages.back().request_response();
      check(response.request_id() == 44 && response.reason() == reason && response.message() == message &&
            response.update_metadata().SerializeAsString() == request.update_metadata().SerializeAsString(), "Self-update response contract differs");
    };
    self_rejected({{"name", std::string(257, 'x')}, {"metadata", "must-not-apply"}}, "exceeds name length limit");
    self_rejected({{"name", "must-not-apply"}, {"metadata", std::string(524289, 'x')}}, "exceeds metadata size limit");
    self_rejected({{"attributes", {{"k", std::string(65536, 'x')}}}}, "exceeds attributes size limit");
    p->claims.update_metadata = false;
    self_rejected({{"name", std::string(257, 'x')}}, "does not have permission to update own metadata", livekit::RequestResponse::NOT_ALLOWED);
    p->claims.update_metadata = true;
    request.Clear(); request.mutable_update_metadata()->set_name("valid-after-errors"); rooms.signal(p, request);
    check(p->info.name() == "valid-after-errors" && sink->messages.back().request_response().reason() == livekit::RequestResponse::OK, "Valid self-update did not recover");
    auto legacy_sink = std::make_shared<Sink>(); lk::Participant legacy; legacy.sink = legacy_sink;
    livekit::SignalResponse response; response.mutable_request_response()->set_reason(livekit::RequestResponse::OK);
    legacy.client_info.set_protocol(14); legacy.send(response); check(legacy_sink->messages.empty(), "Pre-v15 client received success response");
    response.mutable_request_response()->set_reason(livekit::RequestResponse::NOT_ALLOWED); legacy.send(response);
    check(legacy_sink->messages.size() == 1, "Pre-v15 client lost error response");
    legacy.client_info.set_sdk(livekit::ClientInfo::RUST); legacy.client_info.set_protocol(9); legacy.send(response);
    response.Clear(); response.mutable_track_subscribed()->set_track_sid("track"); legacy.send(response);
    check(legacy_sink->messages.size() == 1, "Old Rust client received unknown response variant");
    legacy.client_info.set_protocol(10); legacy.send(response); check(legacy_sink->messages.size() == 2, "Modern Rust client lost supported event");
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
    auto mute_admin=admin;mute_admin.room="layers";
    auto mute_request=lk::Json{{"room","layers"},{"identity",pub->info.identity()},{"trackSid",sid},{"muted",true}};
    rooms.api("MutePublishedTrack",mute_request,mute_admin);
    check(pub->tracks.at(sid).info.muted(),"Admin mute did not update pending publication");
    const auto subscriber_updates=sub_sink->messages.size();
    rooms.api("MutePublishedTrack",mute_request,mute_admin);
    check(sub_sink->messages.size()==subscriber_updates,"Repeated mute emitted redundant participant updates");
    mute_request["muted"]=false;const auto publisher_messages=pub_sink->messages.size();
    rejected([&]{rooms.api("MutePublishedTrack",mute_request,mute_admin);},412);
    check(pub->tracks.at(sid).info.muted() && pub_sink->messages.size()==publisher_messages,"Rejected remote unmute changed state or sent a signal");
    config.enable_remote_unmute=true;rooms.api("MutePublishedTrack",mute_request,mute_admin);
    check(!pub->tracks.at(sid).info.muted(),"Configured remote unmute did not update publication");
    mute_request["trackSid"]="unknown";
    check(!rooms.api("MutePublishedTrack",mute_request,mute_admin).contains("track"),"Missing track fabricated a response track");
    check(pub_sink->messages.back().has_mute() && pub_sink->messages.back().mute().sid()=="unknown","Admin signal for an unknown track was omitted");
    config.enable_remote_unmute=false;
    rejected([&]{rooms.api("MutePublishedTrack",mute_request,mute_admin);},412);
    mute_request["identity"]="missing";
    rejected([&]{rooms.api("MutePublishedTrack",mute_request,mute_admin);},503);
    mute_request["room"]="missing";mute_admin.room="missing";
    rejected([&]{rooms.api("MutePublishedTrack",mute_request,mute_admin);},503);
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
    admin.room = "visibility";
    rooms.api("CreateRoom", {{"name", "visibility"}, {"maxParticipants", 2}}, admin);
    auto hidden_claims = claims; hidden_claims.room = "visibility"; hidden_claims.hidden = true;
    auto hidden_sink = std::make_shared<Sink>(); auto hidden = rooms.join(hidden_claims, true, hidden_sink);
    auto observer_claims = hidden_claims; observer_claims.identity = "observer"; observer_claims.hidden = false;
    auto observer_sink = std::make_shared<Sink>(); auto observer = rooms.join(observer_claims, true, observer_sink);
    check(observer_sink->messages.front().join().other_participants_size() == 0, "Hidden participant leaked in join snapshot");
    auto recorder_claims = observer_claims; recorder_claims.identity = "recorder"; recorder_claims.kind = "egress";
    recorder_claims.kind_details = {"CLOUD_AGENT", "unknown", "forwarded"};
    auto recorder_sink = std::make_shared<Sink>(); auto recorder = rooms.join(recorder_claims, true, recorder_sink);
    check(recorder->info.kind() == livekit::ParticipantInfo::EGRESS && recorder->info.kind_details_size() == 2,
          "Participant kind/details were not normalized");
    check(recorder_sink->messages.front().join().room().num_participants() == 2 &&
          recorder_sink->messages.front().join().room().active_recording(), "Dependent recorder consumed capacity or recording state missing");
    check(rooms.api("ListParticipants", {{"room", "visibility"}}, admin)["participants"].size() == 3,
          "Management API hid hidden or dependent participant");
    auto over_capacity = observer_claims; over_capacity.identity = "extra";
    rejected([&] { rooms.join(over_capacity, true, std::make_shared<Sink>()); }, 429);
    rooms.api("UpdateParticipant", {{"room", "visibility"}, {"identity", hidden->claims.identity}, {"metadata", "private"}}, admin);
    check(hidden->info.metadata() == "private", "Hidden metadata did not update");
    rooms.leave(hidden);
    for (const auto& message : observer_sink->messages) if (message.has_update())
      for (const auto& participant : message.update().participants())
        check(participant.identity() != hidden->claims.identity, "Hidden update/departure leaked to observer");
    rooms.leave(recorder);
    auto info = rooms.api("ListRooms", {{"names", {"visibility"}}}, admin)["rooms"][0];
    check(info["numParticipants"] == 1 && !info.value("activeRecording", false), "Recorder exit failed to update room state");
    rooms.shutdown(); io.poll();
    std::cout << "PASS: JWT, permissions, protobuf, native PeerConnection, room lifecycle, duplicate identity and cleanup\n";
  } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
