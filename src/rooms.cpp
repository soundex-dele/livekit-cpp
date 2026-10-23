#include "lk/rooms.hpp"
#include <boost/asio/post.hpp>
#include <algorithm>
#include <iostream>

namespace lk {
namespace {
void require(bool ok) { if (!ok) throw Error(403, "permission_denied", "Permission denied"); }
void name_limit(const std::string& s) { if (s.empty() || s.size() > 256) throw Error(400, "invalid_argument", "Name or identity must contain 1..256 bytes"); }
void metadata_limit(const std::string& s) { if (s.size() > 65536) throw Error(400, "invalid_argument", "Metadata exceeds 64 KiB"); }
template <typename T> bool contains(const T& values, const std::string& value) { return std::find(values.begin(), values.end(), value) != values.end(); }
void codecs(livekit::Room& room) { for (auto mime : {"audio/opus", "video/VP8"}) room.add_enabled_codecs()->set_mime(mime); }
void attributes(livekit::ParticipantInfo& info, const google::protobuf::Map<std::string, std::string>& attrs) {
  for (const auto& [k, v] : attrs) { if (v.empty()) info.mutable_attributes()->erase(k); else (*info.mutable_attributes())[k] = v; }
}
}
Rooms::Rooms(boost::asio::io_context& io, RtcRuntime& rtc, const Config& config) : io_(io), rtc_(rtc), config_(config), timer_(io) { tick(); }
Rooms::~Rooms() { shutdown(); }
Room& Rooms::room(const std::string& name) {
  auto it = rooms_.find(name); if (it == rooms_.end()) throw Error(404, "not_found", "Room not found"); return it->second;
}
std::shared_ptr<Participant> Rooms::participant(Room& room, const std::string& identity) {
  auto it = room.participants.find(identity); if (it == room.participants.end()) throw Error(404, "not_found", "Participant not found"); return it->second;
}
Room& Rooms::create(const livekit::CreateRoomRequest& req) {
  name_limit(req.name()); metadata_limit(req.metadata());
  if (req.has_egress() || !req.agents().empty() || !req.room_preset().empty()) throw Error(501, "unimplemented", "Egress, agents and room presets are not supported");
  auto [it, inserted] = rooms_.try_emplace(req.name());
  if (inserted) {
    auto& r = it->second.info;
    r.set_sid(random_id("RM_")); r.set_name(req.name()); r.set_creation_time(now_ms() / 1000); r.set_creation_time_ms(now_ms());
    r.set_empty_timeout(req.empty_timeout() ? req.empty_timeout() : 300);
    r.set_departure_timeout(req.departure_timeout() ? req.departure_timeout() : 20);
    r.set_max_participants(req.max_participants()); r.set_metadata(req.metadata()); codecs(r);
  }
  return it->second;
}
void Rooms::broadcast(Room& room, const livekit::SignalResponse& response) { for (auto& [_, p] : room.participants) p->send(response); }
void Rooms::updated(const std::shared_ptr<Participant>& p) {
  p->info.clear_tracks();
  for (const auto& [_, t] : p->tracks) *p->info.add_tracks() = t.info;
  p->info.set_is_publisher(!p->tracks.empty()); p->info.set_version(p->info.version() + 1);
  auto& r = room(p->claims.room);
  r.info.set_num_participants(static_cast<uint32_t>(r.participants.size()));
  uint32_t publishers = 0; for (const auto& [_, part] : r.participants) if (!part->tracks.empty()) ++publishers;
  r.info.set_num_publishers(publishers);
  livekit::SignalResponse response; *response.mutable_update()->add_participants() = p->info; broadcast(r, response);
}
std::shared_ptr<Participant> Rooms::join(const Claims& claims, bool auto_subscribe, std::shared_ptr<SignalSink> sink) {
  require(claims.join); name_limit(claims.room); name_limit(claims.identity); metadata_limit(claims.metadata); metadata_limit(claims.attributes.dump());
  if (stopping_) throw Error(503, "unavailable", "Server is stopping");
  livekit::CreateRoomRequest create_req; create_req.set_name(claims.room);
  auto& r = create(create_req);
  if (auto it = r.participants.find(claims.identity); it != r.participants.end()) leave(it->second, livekit::DUPLICATE_IDENTITY);
  if (r.info.max_participants() && r.participants.size() >= r.info.max_participants()) throw Error(429, "resource_exhausted", "Room is full");
  auto p = std::make_shared<Participant>(); p->claims = claims; p->sink = sink; p->auto_subscribe = auto_subscribe;
  p->info.set_sid(random_id("PA_")); p->info.set_identity(claims.identity); p->info.set_name(claims.name); p->info.set_metadata(claims.metadata);
  p->info.set_joined_at(now_ms() / 1000); p->info.set_joined_at_ms(now_ms()); p->info.set_state(livekit::ParticipantInfo::JOINED); p->info.set_version(1);
  for (const auto& item : claims.attributes.items()) (*p->info.mutable_attributes())[item.key()] = item.value().get<std::string>();
  auto* permission = p->info.mutable_permission();
  permission->set_can_publish(claims.publish); permission->set_can_subscribe(claims.subscribe); permission->set_can_publish_data(claims.publish_data); permission->set_can_update_metadata(claims.update_metadata);
  for (auto source : claims.publish_sources) permission->add_can_publish_sources(static_cast<livekit::TrackSource>(source));
  std::weak_ptr<Participant> weak = p;
  for (bool sub : {false, true}) {
    PeerEvents events;
    events.description = [weak](std::string type, std::string sdp) {
      if (auto p = weak.lock(); p && !p->closed) {
        livekit::SignalResponse res;
        auto* d = type == "offer" ? res.mutable_offer() : res.mutable_answer(); d->set_type(type); d->set_sdp(sdp); p->send(res);
      }
    };
    events.candidate = [weak, sub](std::string json) {
      if (auto p = weak.lock(); p && !p->closed) {
        livekit::SignalResponse res; auto* c = res.mutable_trickle(); c->set_candidateinit(json); c->set_target(sub ? livekit::SUBSCRIBER : livekit::PUBLISHER); p->send(res);
      }
    };
    events.connected = [this, weak, sub] {
      if (auto p = weak.lock(); p && !p->closed && sub && p->info.state() != livekit::ParticipantInfo::ACTIVE) {
        p->info.set_state(livekit::ParticipantInfo::ACTIVE); updated(p); subscriptions_ready(p);
      }
    };
    if (sub) events.negotiated = [this, weak] { if (auto p = weak.lock(); p && !p->closed) subscriptions_ready(p); };
    events.error = [this, weak](std::string error) {
      std::cerr << "RTC session failed: " << error << '\n';
      if (auto p = weak.lock(); p && !p->closed) leave(p, livekit::MEDIA_FAILURE);
    };
    if (!sub) {
      events.track = [this, weak](Track track, std::string mid) { if (auto p = weak.lock(); p && !p->closed) received_track(p, track, mid); };
      events.remove_track = [this, weak](std::string cid) {
        if (auto p = weak.lock(); p && !p->closed) {
          for (const auto& [sid, t] : p->tracks) if (t.cid == cid || (t.media && t.media->id() == cid)) { auto id = sid; unpublish(p, id); break; }
        }
      };
      events.data = [this, weak](std::string bytes, bool reliable) {
        if (auto p = weak.lock(); p && !p->closed && p->claims.publish_data) {
          livekit::DataPacket packet;
          if (packet.ParseFromString(bytes) && !packet.has_speaker() && packet.value_case() != livekit::DataPacket::VALUE_NOT_SET)
            route_data(room(p->claims.room), p, std::move(packet), reliable);
        }
      };
    }
    auto peer = Peer::create(io_, rtc_, config_, sub, std::move(events));
    if (sub) p->subscriber = peer; else p->publisher = peer;
  }
  r.participants[claims.identity] = p; r.used = true; r.info.set_num_participants(static_cast<uint32_t>(r.participants.size()));
  livekit::SignalResponse response; auto* join = response.mutable_join();
  *join->mutable_room() = r.info; *join->mutable_participant() = p->info;
  for (const auto& [_, other] : r.participants) if (other != p) *join->add_other_participants() = other->info;
  join->set_subscriber_primary(true); join->set_ping_interval(5); join->set_ping_timeout(15);
  join->mutable_server_info()->set_version("1.9.0"); join->mutable_server_info()->set_protocol(15); join->mutable_server_info()->set_node_id("ND_cpp");
  join->mutable_server_info()->set_debug_info("livekit-cpp/0.1.0 native media relay");
  join->mutable_client_configuration()->set_resume_connection(livekit::DISABLED);
  for (const auto& codec : r.info.enabled_codecs()) *join->add_enabled_publish_codecs() = codec;
  for (const auto& ice : config_.ice_servers) {
    auto* server = join->add_ice_servers(); for (const auto& url : ice.urls) server->add_urls(url);
    server->set_username(ice.username); server->set_credential(ice.credential);
  }
  p->send(response); updated(p);
  p->subscriber->negotiate();
  if (claims.subscribe && auto_subscribe) for (const auto& [_, other] : r.participants) if (other != p)
    for (const auto& [sid, t] : other->tracks) if (t.media) subscribe(p, sid, true);
  return p;
}
void Rooms::leave(const std::shared_ptr<Participant>& input, livekit::DisconnectReason reason, bool reconnect) {
  // Copy the shared_ptr because input may refer to the map element erased below.
  auto p = input;
  if (!p || p->closed) return;
  p->closed = true;
  auto it = rooms_.find(p->claims.room);
  if (it != rooms_.end()) {
    auto& r = it->second;
    std::vector<std::string> tracks; for (const auto& [sid, _] : p->tracks) tracks.push_back(sid);
    for (const auto& sid : tracks) for (auto& [_, other] : r.participants) if (other != p) other->subscriber->remove_track(sid);
    r.participants.erase(p->claims.identity); r.info.set_num_participants(static_cast<uint32_t>(r.participants.size()));
    if (r.participants.empty()) r.empty_since = now_ms();
    p->info.set_state(livekit::ParticipantInfo::DISCONNECTED); updated(p);
  }
  livekit::SignalResponse res; auto* leave = res.mutable_leave(); leave->set_reason(reason); leave->set_can_reconnect(reconnect); leave->set_action(reconnect ? livekit::LeaveRequest::RECONNECT : livekit::LeaveRequest::DISCONNECT);
  p->send(res); if (auto sink = p->sink.lock()) sink->close();
  p->publisher->close(); p->subscriber->close();
  p->tracks.clear();
}
void Rooms::received_track(const std::shared_ptr<Participant>& p, Track track, const std::string& mid) {
  for (auto& [sid, t] : p->tracks) {
    if (t.cid != track->id() && t.mid != mid) continue;
    t.media = track; t.mid = mid; t.info.set_mid(mid);
    t.info.set_mime_type(track->kind() == "audio" ? "audio/opus" : "video/VP8"); updated(p);
    for (auto& [_, other] : room(p->claims.room).participants) if (other != p && other->claims.subscribe) {
      bool desired = other->subscriptions.contains(sid) ? other->subscriptions[sid] : other->auto_subscribe;
      if (desired) subscribe(other, sid, true);
    }
    return;
  }
  // Never forward media that bypasses add_track authorization.
  leave(p, livekit::JOIN_FAILURE);
}
void Rooms::unpublish(const std::shared_ptr<Participant>& p, const std::string& sid) {
  auto it = p->tracks.find(sid); if (it == p->tracks.end()) return;
  for (auto& [_, other] : room(p->claims.room).participants) if (other != p) other->subscriber->remove_track(sid);
  p->tracks.erase(it); updated(p);
  livekit::SignalResponse res; res.mutable_track_unpublished()->set_track_sid(sid); p->send(res);
}
void Rooms::subscribe(const std::shared_ptr<Participant>& p, const std::string& sid, bool enabled) {
  if (enabled) require(p->claims.subscribe);
  p->subscriptions[sid] = enabled;
  if (!enabled) { p->subscriber->remove_track(sid); return; }
  for (auto& [_, other] : room(p->claims.room).participants) if (other != p) {
    auto t = other->tracks.find(sid); if (t == other->tracks.end() || !t->second.media) continue;
    p->subscriber->add_track(sid, other->info.sid(), t->second.media);
    p->subscriber->enable_track(sid, !t->second.info.muted() && !p->disabled_tracks.contains(sid));
    return;
  }
}
void Rooms::subscriptions_ready(const std::shared_ptr<Participant>& subscriber) {
  if (!subscriber->claims.subscribe || subscriber->info.state() != livekit::ParticipantInfo::ACTIVE) return;
  for (auto& [_, publisher] : room(subscriber->claims.room).participants) if (publisher != subscriber) {
    for (auto& [sid, track] : publisher->tracks) {
      if (track.subscribed_once || !track.media || !subscriber->subscriptions.contains(sid) || !subscriber->subscriptions[sid]) continue;
      track.subscribed_once = true;
      livekit::SignalResponse res; res.mutable_track_subscribed()->set_track_sid(sid); publisher->send(res);
    }
  }
}
void Rooms::mute(const std::shared_ptr<Participant>& p, const std::string& sid, bool muted) {
  auto it = p->tracks.find(sid); if (it == p->tracks.end()) throw Error(404, "not_found", "Track not found");
  it->second.info.set_muted(muted);
  for (auto& [_, other] : room(p->claims.room).participants) if (other != p) other->subscriber->enable_track(sid, !muted && !other->disabled_tracks.contains(sid));
  updated(p);
}
void Rooms::route_data(Room& r, const std::shared_ptr<Participant>& sender, livekit::DataPacket packet, bool reliable) {
  packet.set_participant_identity(sender ? sender->info.identity() : ""); packet.set_participant_sid(sender ? sender->info.sid() : "");
  packet.set_kind(reliable ? livekit::DataPacket::RELIABLE : livekit::DataPacket::LOSSY);
  if (packet.has_user()) {
    packet.mutable_user()->set_participant_identity(packet.participant_identity()); packet.mutable_user()->set_participant_sid(packet.participant_sid());
  }
  for (auto& [identity, p] : r.participants) {
    if (p == sender || !p->claims.subscribe) continue;
    if (!packet.destination_identities().empty() && !contains(packet.destination_identities(), identity)) continue;
    if (packet.has_user()) {
      if (!packet.user().destination_identities().empty() && !contains(packet.user().destination_identities(), identity)) continue;
      if (!packet.user().destination_sids().empty() && !contains(packet.user().destination_sids(), p->info.sid())) continue;
    }
    packet.set_sequence(reliable ? ++p->data_sequence : 0);
    if (!p->subscriber->send_data(packet.SerializeAsString(), reliable) && reliable) {
      // Schedule removal outside the participant map traversal.
      boost::asio::post(io_, [this, p] { leave(p, livekit::CONNECTION_TIMEOUT); });
    }
  }
}
void Rooms::signal(const std::shared_ptr<Participant>& p, const livekit::SignalRequest& req) {
  if (!p || p->closed) return;
  p->last_seen = now_ms();
  livekit::SignalResponse res;
  switch (req.message_case()) {
    case livekit::SignalRequest::kOffer:
      for (const auto& [mid, cid] : req.offer().mid_to_track_id()) for (auto& [_, t] : p->tracks) if (t.cid == cid) t.mid = mid;
      p->publisher->remote_description(req.offer().type(), req.offer().sdp()); break;
    case livekit::SignalRequest::kAnswer: p->subscriber->remote_description(req.answer().type(), req.answer().sdp()); break;
    case livekit::SignalRequest::kTrickle:
      if (!req.trickle().final()) (req.trickle().target() == livekit::SUBSCRIBER ? p->subscriber : p->publisher)->ice_candidate(req.trickle().candidateinit()); break;
    case livekit::SignalRequest::kPing: res.set_pong(req.ping()); p->send(res); break;
    case livekit::SignalRequest::kPingReq: res.mutable_pong_resp()->set_last_ping_timestamp(req.ping_req().timestamp()); res.mutable_pong_resp()->set_timestamp(now_ms()); p->send(res); break;
    case livekit::SignalRequest::kLeave: leave(p); break;
    case livekit::SignalRequest::kAddTrack: {
      const auto& a = req.add_track();
      bool allowed = p->claims.publish && (p->claims.publish_sources.empty() || std::find(p->claims.publish_sources.begin(), p->claims.publish_sources.end(), int(a.source())) != p->claims.publish_sources.end());
      if (!allowed || a.encryption() != livekit::Encryption::NONE || (a.type() != livekit::AUDIO && a.type() != livekit::VIDEO) || p->tracks.size() >= 16) {
        auto* error = res.mutable_request_response(); error->set_reason(livekit::RequestResponse::NOT_ALLOWED); error->set_message("Track permission, encryption or track limit rejected"); *error->mutable_add_track() = a; p->send(res); break;
      }
      name_limit(a.cid());
      auto* published = res.mutable_track_published(); published->set_cid(a.cid());
      for (const auto& [_, t] : p->tracks) if (t.cid == a.cid()) { *published->mutable_track() = t.info; p->send(res); return; }
      PublishedTrack track; track.cid = a.cid(); auto& t = track.info;
      t.set_sid(random_id("TR_")); t.set_name(a.name()); t.set_type(a.type()); t.set_source(a.source()); t.set_muted(a.muted());
      t.set_width(a.width()); t.set_height(a.height()); t.set_stream(a.stream()); t.set_encryption(a.encryption());
      t.set_disable_red(true); *t.mutable_audio_features() = a.audio_features();
      // This relay produces one encoding per subscriber. Do not advertise simulcast layers.
      *published->mutable_track() = t; p->tracks[t.sid()] = std::move(track); p->send(res); updated(p); break;
    }
    case livekit::SignalRequest::kMute: mute(p, req.mute().sid(), req.mute().muted()); break;
    case livekit::SignalRequest::kSubscription: {
      const auto& s = req.subscription(); for (const auto& sid : s.track_sids()) subscribe(p, sid, s.subscribe());
      for (const auto& part : s.participant_tracks()) for (const auto& sid : part.track_sids()) subscribe(p, sid, s.subscribe()); break;
    }
    case livekit::SignalRequest::kTrackSetting:
      for (const auto& sid : req.track_setting().track_sids()) {
        if (req.track_setting().disabled()) { p->disabled_tracks.insert(sid); p->subscriber->enable_track(sid, false); }
        else { p->disabled_tracks.erase(sid); if (p->subscriptions.contains(sid) && p->subscriptions[sid]) subscribe(p, sid, true); }
      } break;
    case livekit::SignalRequest::kUpdateMetadata: {
      auto* response = res.mutable_request_response(); response->set_request_id(req.update_metadata().request_id()); *response->mutable_update_metadata() = req.update_metadata();
      if (!p->claims.update_metadata) { response->set_reason(livekit::RequestResponse::NOT_ALLOWED); p->send(res); break; }
      metadata_limit(req.update_metadata().metadata()); metadata_limit(req.update_metadata().SerializeAsString());
      p->info.set_metadata(req.update_metadata().metadata()); p->info.set_name(req.update_metadata().name()); attributes(p->info, req.update_metadata().attributes());
      updated(p); response->set_reason(livekit::RequestResponse::OK); p->send(res); break;
    }
    case livekit::SignalRequest::kSyncState: leave(p, livekit::STATE_MISMATCH, true); break;
    case livekit::SignalRequest::kSubscriptionPermission:
      // Per-publisher ACLs must never silently allow a denied subscription.
      if (!req.subscription_permission().all_participants()) leave(p, livekit::JOIN_FAILURE);
      break;
    case livekit::SignalRequest::kUpdateLayers:
    case livekit::SignalRequest::kUpdateAudioTrack:
    case livekit::SignalRequest::kUpdateVideoTrack: break;
    default:
      res.mutable_request_response()->set_reason(livekit::RequestResponse::UNSUPPORTED_TYPE);
      res.mutable_request_response()->set_message("Signal operation is not implemented"); p->send(res); break;
  }
}
Json Rooms::api(const std::string& method, const Json& json, const Claims& claims) {
  static const std::set<std::string> supported = {"CreateRoom", "ListRooms", "DeleteRoom", "ListParticipants", "GetParticipant", "RemoveParticipant", "MutePublishedTrack", "UpdateParticipant", "UpdateSubscriptions", "SendData", "UpdateRoomMetadata"};
  if (!supported.contains(method)) throw Error(501, "unimplemented", "RoomService method is not implemented");
  if (method == "CreateRoom") { require(claims.create); return to_json(create(from_json<livekit::CreateRoomRequest>(json)).info); }
  if (method == "ListRooms") {
    require(claims.list); auto req = from_json<livekit::ListRoomsRequest>(json); livekit::ListRoomsResponse response;
    for (const auto& [name, r] : rooms_) if (req.names().empty() || contains(req.names(), name)) *response.add_rooms() = r.info;
    return to_json(response);
  }
  if (method == "DeleteRoom") {
    require(claims.create); auto req = from_json<livekit::DeleteRoomRequest>(json); auto& r = room(req.room());
    while (!r.participants.empty()) leave(r.participants.begin()->second, livekit::ROOM_DELETED);
    rooms_.erase(req.room()); return Json::object();
  }
  auto name = json.value("room", ""); require(claims.admin && claims.room == name && !name.empty()); auto& r = room(name);
  if (method == "ListParticipants") {
    from_json<livekit::ListParticipantsRequest>(json); livekit::ListParticipantsResponse res;
    for (const auto& [_, p] : r.participants) *res.add_participants() = p->info; return to_json(res);
  }
  if (method == "UpdateRoomMetadata") {
    auto req = from_json<livekit::UpdateRoomMetadataRequest>(json); metadata_limit(req.metadata()); r.info.set_metadata(req.metadata());
    livekit::SignalResponse res; *res.mutable_room_update()->mutable_room() = r.info; broadcast(r, res); return to_json(r.info);
  }
  if (method == "SendData") {
    auto req = from_json<livekit::SendDataRequest>(json); if (req.data().size() > 60000) throw Error(400, "invalid_argument", "Data payload too large");
    livekit::DataPacket packet; auto* user = packet.mutable_user(); user->set_payload(req.data()); if (req.has_topic()) user->set_topic(req.topic()); user->set_nonce(req.nonce());
    *user->mutable_destination_sids() = req.destination_sids(); *packet.mutable_destination_identities() = req.destination_identities();
    route_data(r, nullptr, packet, req.kind() == livekit::DataPacket::RELIABLE); return Json::object();
  }
  auto p = participant(r, json.value("identity", ""));
  if (method == "GetParticipant") { from_json<livekit::RoomParticipantIdentity>(json); return to_json(p->info); }
  if (method == "RemoveParticipant") { from_json<livekit::RoomParticipantIdentity>(json); leave(p, livekit::PARTICIPANT_REMOVED); return Json::object(); }
  if (method == "MutePublishedTrack") {
    auto req = from_json<livekit::MuteRoomTrackRequest>(json); mute(p, req.track_sid(), req.muted());
    livekit::SignalResponse signal; signal.mutable_mute()->set_sid(req.track_sid()); signal.mutable_mute()->set_muted(req.muted()); p->send(signal);
    livekit::MuteRoomTrackResponse res; *res.mutable_track() = p->tracks.at(req.track_sid()).info; return to_json(res);
  }
  if (method == "UpdateSubscriptions") {
    auto req = from_json<livekit::UpdateSubscriptionsRequest>(json); for (const auto& sid : req.track_sids()) subscribe(p, sid, req.subscribe());
    for (const auto& part : req.participant_tracks()) for (const auto& sid : part.track_sids()) subscribe(p, sid, req.subscribe()); return Json::object();
  }
  if (method == "UpdateParticipant") {
    auto req = from_json<livekit::UpdateParticipantRequest>(json); metadata_limit(req.metadata()); metadata_limit(req.SerializeAsString());
    if (req.has_permission()) {
      const auto& permission = req.permission();
      if (permission.hidden()) throw Error(501, "unimplemented", "Hidden participants are not supported");
      p->claims.publish = permission.can_publish(); p->claims.subscribe = permission.can_subscribe();
      p->claims.publish_data = permission.can_publish_data(); p->claims.update_metadata = permission.can_update_metadata();
      p->claims.publish_sources.assign(permission.can_publish_sources().begin(), permission.can_publish_sources().end());
      *p->info.mutable_permission() = permission;
      std::vector<std::string> revoke;
      for (const auto& [sid, t] : p->tracks) if (!p->claims.publish || (!p->claims.publish_sources.empty() && std::find(p->claims.publish_sources.begin(), p->claims.publish_sources.end(), int(t.info.source())) == p->claims.publish_sources.end())) revoke.push_back(sid);
      for (const auto& sid : revoke) unpublish(p, sid);
      for (const auto& [_, other] : r.participants) if (other != p) for (const auto& [sid, t] : other->tracks) {
        if (!p->claims.subscribe) p->subscriber->remove_track(sid);
        else if (p->subscriptions.contains(sid) ? p->subscriptions[sid] : p->auto_subscribe) subscribe(p, sid, true);
      }
    }
    if (!req.metadata().empty()) p->info.set_metadata(req.metadata()); if (!req.name().empty()) p->info.set_name(req.name());
    attributes(p->info, req.attributes()); updated(p); return to_json(p->info);
  }
  throw Error(501, "unimplemented", "RoomService method is not implemented");
}
Json Rooms::metrics() const {
  size_t participants = 0, tracks = 0;
  for (const auto& [_, r] : rooms_) for (const auto& [__, p] : r.participants) { ++participants; tracks += p->tracks.size(); }
  return {{"rooms", rooms_.size()}, {"participants", participants}, {"tracks", tracks}};
}
void Rooms::tick() {
  timer_.expires_after(std::chrono::seconds(1));
  timer_.async_wait([this](boost::system::error_code error) {
    if (error || stopping_) return;
    std::vector<std::shared_ptr<Participant>> expired;
    for (const auto& [_, r] : rooms_) for (const auto& [__, p] : r.participants) if (now_ms() - p->last_seen > 30000) expired.push_back(p);
    for (auto& p : expired) leave(p, livekit::CONNECTION_TIMEOUT);
    for (auto it = rooms_.begin(); it != rooms_.end();) {
      auto& r = it->second;
      if (r.participants.empty() && now_ms() - r.empty_since >= 1000LL * (r.used ? r.info.departure_timeout() : r.info.empty_timeout())) it = rooms_.erase(it);
      else ++it;
    }
    tick();
  });
}
void Rooms::shutdown() {
  if (stopping_) return;
  stopping_ = true; timer_.cancel();
  for (auto& [_, r] : rooms_) while (!r.participants.empty()) leave(r.participants.begin()->second, livekit::SERVER_SHUTDOWN, true);
  rooms_.clear();
}
}
