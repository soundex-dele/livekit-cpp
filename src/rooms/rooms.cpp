#include "lk/rooms.hpp"
#include "lk/media_codec.hpp"
#include <boost/asio/post.hpp>
#include <algorithm>
#include <cctype>
#include <iostream>

namespace lk {
namespace {
void require(bool ok) { if (!ok) throw Error(403, "permission_denied", "Permission denied"); }
// Go's RoomService wraps grant failures in twirp.Unauthenticated. Keep the
// signaling permission contract separate from the management API contract.
void require_api(bool ok) { if (!ok) throw Error(401, "unauthenticated", "permissions denied"); }
void name_limit(const std::string& s) { if (s.empty() || s.size() > 256) throw Error(400, "invalid_argument", "Name or identity must contain 1..256 bytes"); }
void metadata_limit(const std::string& s) { if (s.size() > 512 * 1024) throw Error(400, "invalid_argument", "Metadata exceeds 512 KiB"); }
void attributes_limit(const google::protobuf::Map<std::string, std::string>& attrs) {
  size_t bytes = 0;
  for (const auto& [key, value] : attrs) bytes += key.size() + value.size();
  if (bytes > 65536) throw Error(400, "invalid_argument", "Attributes exceed 64 KiB");
}
void attributes_limit(const Json& attrs) {
  size_t bytes = 0;
  for (const auto& item : attrs.items()) bytes += item.key().size() + item.value().get_ref<const std::string&>().size();
  if (bytes > 65536) throw Error(400, "invalid_argument", "Attributes exceed 64 KiB");
}
bool valid_layers(const google::protobuf::RepeatedPtrField<livekit::VideoLayer>& layers) {
  if (layers.size()>3) return false;
  std::set<int> qualities;
  for (const auto& layer:layers)
    if (layer.quality()<livekit::LOW || layer.quality()>livekit::HIGH || !qualities.insert(int(layer.quality())).second ||
        layer.width()>16384 || layer.height()>16384 || layer.rid().size()>16) return false;
  return true;
}
template <typename T> bool contains(const T& values, const std::string& value) { return std::find(values.begin(), values.end(), value) != values.end(); }
void codecs(livekit::Room& room,bool forwarding) {
  for (auto mime : {"audio/opus", "video/VP8"}) room.add_enabled_codecs()->set_mime(mime);
  if (forwarding) for (auto mime:{"audio/PCMU","audio/PCMA","video/H264"}) room.add_enabled_codecs()->set_mime(mime);
}
void attributes(livekit::ParticipantInfo& info, const google::protobuf::Map<std::string, std::string>& attrs) {
  for (const auto& [k, v] : attrs) { if (v.empty()) info.mutable_attributes()->erase(k); else (*info.mutable_attributes())[k] = v; }
}
}
Rooms::Rooms(boost::asio::io_context& io, RtcRuntime& rtc, const Config& config) : io_(io), rtc_(rtc), config_(config), timer_(io), rpc_(io) { tick(); }
Rooms::~Rooms() { shutdown(); }
Room& Rooms::room(const std::string& name) {
  auto it = rooms_.find(name); if (it == rooms_.end()) throw Error(404, "not_found", "Room not found"); return it->second;
}
std::shared_ptr<Participant> Rooms::participant(Room& room, const std::string& identity) {
  auto it = room.participants.find(identity); if (it == room.participants.end()) throw Error(404, "not_found", "Participant not found"); return it->second;
}
Room& Rooms::create(const livekit::CreateRoomRequest& req) {
  // The pinned Go CreateRoom API accepts an empty name; the RTC join path
  // separately requires a non-empty room and identity.
  if (req.name().size() > 256) throw Error(400, "invalid_argument", "Room name exceeds 256 bytes");
  metadata_limit(req.metadata());
  if (req.has_egress() || !req.agents().empty() || !req.room_preset().empty()) throw Error(501, "unimplemented", "Egress, agents and room presets are not supported");
  auto [it, inserted] = rooms_.try_emplace(req.name());
  if (inserted) {
    auto& r = it->second.info;
    r.set_sid(random_id("RM_")); r.set_name(req.name()); r.set_creation_time(now_ms() / 1000); r.set_creation_time_ms(now_ms());
    r.set_empty_timeout(req.empty_timeout() ? req.empty_timeout() : 300);
    r.set_departure_timeout(req.departure_timeout() ? req.departure_timeout() : 20);
    r.set_max_participants(req.max_participants()); r.set_metadata(req.metadata()); codecs(r,config_.media_mode=="sfu");
  }
  return it->second;
}
void Rooms::broadcast(Room& room, const livekit::SignalResponse& response) { for (auto& [_, p] : room.participants) p->send(response); }
void Rooms::refresh_room(Room& r, std::optional<bool> recording, bool notify) {
  const auto before = r.info.SerializeAsString();
  uint32_t participants = 0, publishers = 0;
  for (const auto& [_, p] : r.participants) {
    if (!p->claims.is_dependent()) ++participants;
    if (!p->tracks.empty()) ++publishers;
  }
  r.info.set_num_participants(participants); r.info.set_num_publishers(publishers);
  if (recording) r.info.set_active_recording(*recording);
  if (notify && before != r.info.SerializeAsString()) {
    livekit::SignalResponse response; *response.mutable_room_update()->mutable_room() = r.info; broadcast(r, response);
  }
}
void Rooms::updated(const std::shared_ptr<Participant>& p) {
  p->info.clear_tracks();
  for (const auto& [_, t] : p->tracks) *p->info.add_tracks() = t.info;
  p->info.set_is_publisher(!p->tracks.empty()); p->info.set_version(p->info.version() + 1);
  auto& r = room(p->claims.room);
  refresh_room(r);
  livekit::SignalResponse response; *response.mutable_update()->add_participants() = p->info;
  if (p->claims.hidden) p->send(response); else broadcast(r, response);
}
void Rooms::refresh_token(const std::shared_ptr<Participant>& p) {
  if (p->closed) return;
  try {
    livekit::SignalResponse response; response.set_refresh_token(Auth(config_.keys).refresh(p->claims)); p->send(response);
  } catch (const std::exception&) { std::cerr << "Could not refresh participant token\n"; }
}
void Rooms::update_metadata(const std::shared_ptr<Participant>& p, const std::string& name, const std::string& metadata,
                            const google::protobuf::Map<std::string, std::string>& attrs) {
  // Go emits a claims change for each changed field, in this order. Empty
  // name/metadata mean no change; empty attribute values delete individual keys.
  if (!name.empty() && name != p->info.name()) {
    p->info.set_name(name); p->claims.name = name; updated(p); refresh_token(p);
  }
  if (!metadata.empty() && metadata != p->info.metadata()) {
    p->info.set_metadata(metadata); p->claims.metadata = metadata; updated(p); refresh_token(p);
  }
  if (!attrs.empty()) {
    attributes(p->info, attrs);
    p->claims.attributes = Json::object();
    for (const auto& [key, value] : p->info.attributes()) p->claims.attributes[key] = value;
    updated(p); refresh_token(p);
  }
}
std::shared_ptr<Participant> Rooms::join(const Claims& claims, bool auto_subscribe, std::shared_ptr<SignalSink> sink, int single_pc_protocol) {
  require(claims.join); name_limit(claims.room); name_limit(claims.identity); metadata_limit(claims.metadata); attributes_limit(claims.attributes);
  if (stopping_) throw Error(503, "unavailable", "Server is stopping");
  livekit::CreateRoomRequest create_req; create_req.set_name(claims.room);
  auto& r = create(create_req);
  if (auto it = r.participants.find(claims.identity); it != r.participants.end()) leave(it->second, livekit::DUPLICATE_IDENTITY);
  if (!claims.is_dependent() && r.info.max_participants() && r.info.num_participants() >= r.info.max_participants())
    throw Error(429, "resource_exhausted", "Room is full");
  auto p = std::make_shared<Participant>(); p->claims = claims; p->sink = sink; p->auto_subscribe = auto_subscribe; p->single_pc_protocol = single_pc_protocol;
  p->info.set_sid(random_id("PA_")); p->info.set_identity(claims.identity); p->info.set_name(claims.name); p->info.set_metadata(claims.metadata);
  p->info.set_joined_at(now_ms() / 1000); p->info.set_joined_at_ms(now_ms()); p->info.set_state(livekit::ParticipantInfo::JOINED); p->info.set_version(1);
  for (const auto& item : claims.attributes.items()) (*p->info.mutable_attributes())[item.key()] = item.value().get<std::string>();
  auto* permission = p->info.mutable_permission();
  permission->set_recorder(claims.recorder);
  permission->set_hidden(claims.hidden); permission->set_agent(claims.agent);
  permission->set_can_subscribe_metrics(claims.subscribe_metrics); permission->set_can_manage_agent_session(claims.manage_agent_session);
  auto upper = [](std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); }); return value;
  };
  livekit::ParticipantInfo::Kind kind;
  if (livekit::ParticipantInfo::Kind_Parse(upper(claims.kind), &kind)) p->info.set_kind(kind);
  for (const auto& detail : claims.kind_details) {
    livekit::ParticipantInfo::KindDetail value;
    if (livekit::ParticipantInfo::KindDetail_Parse(upper(detail), &value)) p->info.add_kind_details(value);
  }
  permission->set_can_publish(claims.publish); permission->set_can_subscribe(claims.subscribe); permission->set_can_publish_data(claims.publish_data); permission->set_can_update_metadata(claims.update_metadata);
  for (auto source : claims.publish_sources) permission->add_can_publish_sources(static_cast<livekit::TrackSource>(source));
  auto session_config = config_;
  session_config.ice_servers = config_.session_ice_servers(p->info.sid());
  session_config.turn_rest.reset(); // Only issued credentials reach native peers.
  std::weak_ptr<Participant> weak = p;
  for (bool sub : {false, true}) {
    if (sub && single_pc_protocol >= 0) { p->subscriber = p->publisher; break; }
    PeerEvents events;
    events.description = [weak](std::string type, std::string sdp, uint32_t id) {
      if (auto p = weak.lock(); p && !p->closed) {
        livekit::SignalResponse res;
        auto* d = type == "offer" ? res.mutable_offer() : res.mutable_answer(); d->set_type(type); d->set_sdp(sdp); d->set_id(id); p->send(res);
      }
    };
    events.candidate = [weak, sub](std::string json) {
      if (auto p = weak.lock(); p && !p->closed) {
        livekit::SignalResponse res; auto* c = res.mutable_trickle(); c->set_candidateinit(json); c->set_target(sub ? livekit::SUBSCRIBER : livekit::PUBLISHER); p->send(res);
      }
    };
    events.connected = [this, weak, sub] {
      if (auto p = weak.lock(); p && !p->closed) {
        p->ice_connected = true;
        if ((sub || p->single_pc_protocol >= 0) && p->info.state() != livekit::ParticipantInfo::ACTIVE) {
          p->info.set_state(livekit::ParticipantInfo::ACTIVE); updated(p); subscriptions_ready(p);
        }
      }
    };
    if (sub || single_pc_protocol >= 0) events.negotiated = [this, weak] { if (auto p = weak.lock(); p && !p->closed) subscriptions_ready(p); };
    events.error = [this, weak](std::string error) {
      std::cerr << "RTC session failed: " << error << '\n';
      if (auto p = weak.lock(); p && !p->closed) leave(p, livekit::MEDIA_FAILURE);
    };
    if (!sub) {
      events.track = [this, weak](Track track, std::string mid,webrtc::RtpParameters parameters) { if (auto p = weak.lock(); p && !p->closed) received_track(p, track, mid,parameters); };
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
    events.media_sections = [weak](uint32_t audio, uint32_t video) {
      if (auto p = weak.lock(); p && !p->closed) {
        livekit::SignalResponse response;
        auto* requirement = response.mutable_media_sections_requirement();
        requirement->set_num_audios(audio); requirement->set_num_videos(video); p->send(response);
      }
    };
    auto peer = Peer::create(io_, rtc_, session_config, sub, std::move(events), single_pc_protocol);
    if (sub) p->subscriber = peer; else p->publisher = peer;
  }
  r.participants[claims.identity] = p;
  if (!claims.is_dependent()) r.used = true;
  refresh_room(r, claims.is_recorder() ? std::optional<bool>(true) : std::nullopt, false);
  livekit::SignalResponse response; auto* join = response.mutable_join();
  *join->mutable_room() = r.info; *join->mutable_participant() = p->info;
  for (const auto& [_, other] : r.participants) if (other != p && !other->claims.hidden) *join->add_other_participants() = other->info;
  join->set_subscriber_primary(single_pc_protocol < 0); join->set_ping_interval(5); join->set_ping_timeout(15);
  join->mutable_server_info()->set_version("1.9.0"); join->mutable_server_info()->set_protocol(15); join->mutable_server_info()->set_node_id("ND_cpp");
  join->mutable_server_info()->set_debug_info("livekit-cpp/0.1.0 native " + config_.media_mode);
  for (const auto& codec : r.info.enabled_codecs()) *join->add_enabled_publish_codecs() = codec;
  for (const auto& ice : session_config.ice_servers) {
    auto* server = join->add_ice_servers(); for (const auto& url : ice.urls) server->add_urls(url);
    server->set_username(ice.username); server->set_credential(ice.credential);
  }
  p->send(response); updated(p);
  livekit::SignalResponse room_update; *room_update.mutable_room_update()->mutable_room() = r.info; broadcast(r, room_update);
  if (single_pc_protocol < 0) p->subscriber->negotiate();
  if (claims.subscribe && auto_subscribe) for (const auto& [_, other] : r.participants) if (other != p)
    for (const auto& [sid, t] : other->tracks) if (t.media) subscribe(p, sid, true);
  refresh_token(p);
  return p;
}
std::shared_ptr<Participant> Rooms::resume(const Claims& claims, std::shared_ptr<SignalSink> sink) {
  require(claims.join);
  auto room_it = rooms_.find(claims.room);
  if (room_it == rooms_.end()) return {};
  auto& r = room_it->second;
  auto it = r.participants.find(claims.identity);
  if (it == r.participants.end() || it->second->closed) return {};
  auto p = it->second;
  const auto last_signal = p->last_seen;
  auto previous = p->sink.lock();
  p->sink = sink; p->last_seen = now_ms();
  // Existing server-side claims are authoritative: a stale reconnect JWT must
  // not restore a permission an administrator has revoked during the gap.
  if (previous && previous != sink) previous->close();
  livekit::SignalResponse response;
  auto* reconnect = response.mutable_reconnect();
  reconnect->set_last_message_seq(p->last_published_data_sequence);
  reconnect->mutable_server_info()->set_version("1.9.0");
  reconnect->mutable_server_info()->set_protocol(15); reconnect->mutable_server_info()->set_node_id("ND_cpp");
  for (const auto& ice : config_.session_ice_servers(p->info.sid())) {
    auto* server = reconnect->add_ice_servers();
    for (const auto& url : ice.urls) server->add_urls(url);
    server->set_username(ice.username); server->set_credential(ice.credential);
  }
  p->send(response); // This must precede all replay and negotiation messages.
  response.Clear();
  for (const auto& [_, cached] : p->update_cache)
    if (cached.at >= last_signal - 3000 && cached.info.state() == livekit::ParticipantInfo::DISCONNECTED)
      *response.mutable_update()->add_participants() = cached.info;
  if (response.has_update()) p->send(response);
  response.Clear();
  for (const auto& [_, other] : r.participants) if (!other->claims.hidden)
    *response.mutable_update()->add_participants() = other->info;
  if (response.has_update()) p->send(response);
  response.Clear(); *response.mutable_room_update()->mutable_room() = r.info; p->send(response);
  p->subscriber->restart_ice();
  refresh_token(p);
  p->next_token_refresh = std::chrono::steady_clock::now() + std::chrono::minutes(5);
  return p;
}
void Rooms::signal_closed(const std::shared_ptr<Participant>& p, const SignalSink* source) {
  if (!p || p->closed || p->sink.lock().get() != source) return;
  p->sink.reset();
  // Go preserves an ICE-connected participant on signal-only failure. Native
  // transport failure, explicit Leave, admin removal and shutdown still close it.
  if (!p->ice_connected) leave(p, livekit::SIGNAL_CLOSE);
}
void Rooms::leave(const std::shared_ptr<Participant>& input, livekit::DisconnectReason reason, bool reconnect) {
  // Copy the shared_ptr because input may refer to the map element erased below.
  auto p = input;
  if (!p || p->closed) return;
  p->closed = true;
  rpc_.disconnected(p->info.sid());
  auto it = rooms_.find(p->claims.room);
  if (it != rooms_.end()) {
    auto& r = it->second;
    std::vector<std::string> tracks; for (const auto& [sid, _] : p->tracks) tracks.push_back(sid);
    for (const auto& sid : tracks) for (auto& [_, other] : r.participants) if (other != p) {
      other->subscriber->remove_track(sid); other->track_settings.erase(sid);
      other->disabled_tracks.erase(sid); other->subscriptions.erase(sid);
      other->subscription_allowed.erase(sid);
    }
    r.participants.erase(p->claims.identity); r.last_left_ms = now_ms();
    std::optional<bool> recording;
    if (p->claims.is_recorder()) recording = std::any_of(r.participants.begin(), r.participants.end(),
      [](const auto& entry) { return entry.second->claims.is_recorder(); });
    refresh_room(r, recording);
    p->info.set_state(livekit::ParticipantInfo::DISCONNECTED); updated(p);
  }
  livekit::SignalResponse res; auto* leave = res.mutable_leave(); leave->set_reason(reason); leave->set_can_reconnect(reconnect); leave->set_action(reconnect ? livekit::LeaveRequest::RECONNECT : livekit::LeaveRequest::DISCONNECT);
  p->send(res); if (auto sink = p->sink.lock()) sink->close();
  p->publisher->close(); p->subscriber->close();
  p->tracks.clear();
}
void Rooms::received_track(const std::shared_ptr<Participant>& p, Track track, const std::string& mid,const webrtc::RtpParameters& parameters) {
  for (auto& [sid, t] : p->tracks) {
    if (t.cid != track->id() && t.mid != mid) continue;
    t.media = track; t.mid = mid; t.info.set_mid(mid);
    t.codec=primary_media_codec(parameters,track->kind()=="audio");
    if (config_.media_mode=="sfu" && !t.codec) { leave(p,livekit::MEDIA_FAILURE); return; }
    t.info.set_mime_type(track->kind()+"/"+(t.codec ? t.codec->name : (track->kind()=="audio" ? "opus" : "VP8")));
    for (auto& codec:*t.info.mutable_codecs()) { codec.set_mid(mid); codec.set_mime_type(t.info.mime_type()); }
    updated(p);
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
  p->publisher->publication_codec(it->second.cid,"");
  for (auto& [_, other] : room(p->claims.room).participants) if (other != p) {
    other->subscriber->remove_track(sid); other->track_settings.erase(sid);
    other->disabled_tracks.erase(sid); other->subscriptions.erase(sid);
    other->subscription_allowed.erase(sid);
  }
  p->tracks.erase(it); updated(p);
  livekit::SignalResponse res; res.mutable_track_unpublished()->set_track_sid(sid); p->send(res);
}
void Rooms::subscribe(const std::shared_ptr<Participant>& p, const std::string& sid, bool enabled) {
  if (enabled) require(p->claims.subscribe);
  p->subscriptions[sid] = enabled;
  if (!enabled) { p->subscriber->remove_track(sid); return; }
  for (auto& [_, other] : room(p->claims.room).participants) if (other != p) {
    auto t = other->tracks.find(sid); if (t == other->tracks.end() || !t->second.media) continue;
    const bool allowed = p->claims.is_recorder() || other->subscription_permissions.allows(p->claims.identity, sid);
    auto previous = p->subscription_allowed.find(sid);
    if (previous == p->subscription_allowed.end() || previous->second != allowed) {
      p->subscription_allowed[sid] = allowed;
      livekit::SignalResponse response;
      auto* update = response.mutable_subscription_permission_update();
      update->set_participant_sid(other->info.sid()); update->set_track_sid(sid); update->set_allowed(allowed);
      p->send(response);
    }
    if (!allowed) { p->subscriber->remove_track(sid); return; }
    p->subscriber->add_track(sid, other->info.sid(), t->second.media,t->second.codec);
    if (auto settings=p->track_settings.find(sid);settings!=p->track_settings.end()) p->subscriber->track_settings(sid,settings->second);
    p->subscriber->enable_track(sid, !t->second.info.muted() && !p->disabled_tracks.contains(sid));
    return;
  }
}
void Rooms::update_subscription_permissions(const std::shared_ptr<Participant>& publisher, const livekit::SubscriptionPermission& policy) {
  auto& r = room(publisher->claims.room);
  if (!publisher->subscription_permissions.update(policy, [&](const std::string& sid) {
    for (const auto& [identity, p] : r.participants) if (p->info.sid() == sid) return identity;
    return std::string{};
  })) {
    // Go logs this malformed policy and keeps the participant and old grants.
    std::cerr << "Subscription permission needs participant identity or SID\n";
    return;
  }
  for (const auto& [_, p] : r.participants) if (p != publisher && p->claims.subscribe) {
    for (const auto& [sid, track] : publisher->tracks) {
      const bool desired = p->subscriptions.contains(sid) ? p->subscriptions[sid] : p->auto_subscribe;
      if (desired) subscribe(p, sid, true);
    }
  }
}
void Rooms::subscriptions_ready(const std::shared_ptr<Participant>& subscriber) {
  if (!subscriber->claims.subscribe || subscriber->info.state() != livekit::ParticipantInfo::ACTIVE) return;
  for (auto& [_, publisher] : room(subscriber->claims.room).participants) if (publisher != subscriber) {
    for (auto& [sid, track] : publisher->tracks) {
      if (track.subscribed_once || !track.media || !subscriber->subscriptions.contains(sid) || !subscriber->subscriptions[sid]) continue;
      if (!subscriber->claims.is_recorder() && !publisher->subscription_permissions.allows(subscriber->claims.identity, sid)) continue;
      track.subscribed_once = true;
      livekit::SignalResponse res; res.mutable_track_subscribed()->set_track_sid(sid); publisher->send(res);
    }
  }
}
void Rooms::mute(const std::shared_ptr<Participant>& p, const std::string& sid, bool muted) {
  auto it = p->tracks.find(sid); if (it == p->tracks.end() || it->second.info.muted() == muted) return;
  it->second.info.set_muted(muted);
  for (auto& [_, other] : room(p->claims.room).participants) if (other != p) other->subscriber->enable_track(sid, !muted && !other->disabled_tracks.contains(sid));
  updated(p);
}
void Rooms::route_data(Room& r, const std::shared_ptr<Participant>& sender, livekit::DataPacket packet, bool reliable) {
  if (sender && !prepare_client_data(packet, sender->info.sid(), sender->info.identity(),
        sender->claims.hidden, sender->claims.agent || sender->claims.kind == "agent", reliable,
        sender->last_published_data_sequence)) return;
  if (sender && rpc_.receive(sender->info.sid(), packet)) return;
  packet.set_kind(reliable ? livekit::DataPacket::RELIABLE : livekit::DataPacket::LOSSY);
  normalize_data_destinations(packet);
  if (packet.has_user() && r.user_packet_deduper.duplicate(packet.user().nonce())) return;
  for (auto& [identity, p] : r.participants) {
    if (p == sender || !data_destination_matches(packet, identity, p->info.sid())) continue;
    if (!p->subscriber->send_data(packet.SerializeAsString(), reliable) && reliable) {
      // Schedule removal outside the participant map traversal.
      boost::asio::post(io_, [this, participant = p] { leave(participant, livekit::CONNECTION_TIMEOUT); });
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
      p->publisher->remote_description(req.offer().type(), req.offer().sdp(), req.offer().id()); break;
    case livekit::SignalRequest::kAnswer: p->subscriber->remote_description(req.answer().type(), req.answer().sdp(), req.answer().id()); break;
    case livekit::SignalRequest::kTrickle:
      if (!req.trickle().final()) (req.trickle().target() == livekit::SUBSCRIBER ? p->subscriber : p->publisher)->ice_candidate(req.trickle().candidateinit()); break;
    case livekit::SignalRequest::kPing: res.set_pong(req.ping()); p->send(res); break;
    case livekit::SignalRequest::kPingReq:
      if (req.ping_req().rtt() >= 0 && req.ping_req().rtt() <= UINT32_MAX) p->last_rtt_ms = static_cast<uint32_t>(req.ping_req().rtt());
      res.mutable_pong_resp()->set_last_ping_timestamp(req.ping_req().timestamp()); res.mutable_pong_resp()->set_timestamp(now_ms()); p->send(res); break;
    case livekit::SignalRequest::kLeave: leave(p); break;
    case livekit::SignalRequest::kAddTrack: {
      const auto& a = req.add_track();
      bool layers_valid=valid_layers(a.layers()) && a.simulcast_codecs_size()<=3;
      for (const auto& codec:a.simulcast_codecs())
        layers_valid=layers_valid && valid_layers(codec.layers()) &&
          (codec.video_layer_mode()==livekit::VideoLayer::MODE_UNUSED || codec.video_layer_mode()==livekit::VideoLayer::ONE_SPATIAL_LAYER_PER_STREAM);
      bool allowed = p->claims.publish && (p->claims.publish_sources.empty() || std::find(p->claims.publish_sources.begin(), p->claims.publish_sources.end(), int(a.source())) != p->claims.publish_sources.end());
      if (!allowed || !layers_valid || a.encryption() != livekit::Encryption::NONE || (a.type() != livekit::AUDIO && a.type() != livekit::VIDEO) || p->tracks.size() >= 16) {
        auto* error = res.mutable_request_response(); error->set_reason(livekit::RequestResponse::NOT_ALLOWED); error->set_message("Track permission, encryption or track limit rejected"); *error->mutable_add_track() = a; p->send(res); break;
      }
      name_limit(a.cid());
      auto* published = res.mutable_track_published(); published->set_cid(a.cid());
      for (const auto& [_, t] : p->tracks) if (t.cid == a.cid()) { *published->mutable_track() = t.info; p->send(res); return; }
      PublishedTrack track; track.cid = a.cid(); auto& t = track.info;
      t.set_sid(random_id("TR_")); t.set_name(a.name()); t.set_type(a.type()); t.set_source(a.source()); t.set_muted(a.muted());
      t.set_width(a.width()); t.set_height(a.height()); t.set_stream(a.stream()); t.set_encryption(a.encryption());
      t.set_disable_red(true); *t.mutable_audio_features() = a.audio_features();
      if (config_.media_mode=="sfu" && a.type()==livekit::VIDEO) {
        *t.mutable_layers()=a.layers();
        auto* codec=t.add_codecs(); codec->set_mime_type("video/VP8"); codec->set_cid(a.cid());
        *codec->mutable_layers()=a.layers();
        for (const auto& input:a.simulcast_codecs()) if (codec_name(input.codec())=="vp8" || codec_name(input.codec())=="h264") {
          codec->set_mime_type(codec_name(input.codec())=="h264" ? "video/H264" : "video/VP8");
          codec->set_cid(input.cid());
          if (!input.layers().empty()) { *codec->mutable_layers()=input.layers(); *t.mutable_layers()=input.layers(); }
          break;
        }
        codec->set_video_layer_mode(livekit::VideoLayer::ONE_SPATIAL_LAYER_PER_STREAM);
        t.set_simulcast(codec->layers_size()>1);
        p->publisher->publication_codec(a.cid(),codec->mime_type());
      }
      *published->mutable_track() = t; p->tracks[t.sid()] = std::move(track); p->send(res); updated(p); break;
    }
    case livekit::SignalRequest::kMute: mute(p, req.mute().sid(), req.mute().muted()); break;
    case livekit::SignalRequest::kSubscription: {
      const auto& s = req.subscription(); for (const auto& sid : s.track_sids()) subscribe(p, sid, s.subscribe());
      for (const auto& part : s.participant_tracks()) for (const auto& sid : part.track_sids()) subscribe(p, sid, s.subscribe()); break;
    }
    case livekit::SignalRequest::kTrackSetting:
      require(p->claims.subscribe);
      if (req.track_setting().width()>16384 || req.track_setting().height()>16384 ||
          req.track_setting().quality()<livekit::LOW || req.track_setting().quality()>livekit::OFF)
        throw Error(400,"invalid_argument","Invalid video subscription dimensions or quality");
      if (req.track_setting().fps()) {
        res.mutable_request_response()->set_reason(livekit::RequestResponse::UNSUPPORTED_TYPE);
        res.mutable_request_response()->set_message("Per-subscription fps is not implemented"); p->send(res); break;
      }
      for (const auto& sid : req.track_setting().track_sids()) {
        bool exists=false;
        for (const auto& [_,publisher]:room(p->claims.room).participants)
          if (publisher!=p && publisher->tracks.contains(sid)) { exists=true; break; }
        if (!exists) continue;
        VideoSubscription settings{std::min(2,int(req.track_setting().quality())),req.track_setting().width(),req.track_setting().height(),req.track_setting().priority()};
        p->track_settings[sid]=settings; p->subscriber->track_settings(sid,settings);
        if (req.track_setting().disabled() || req.track_setting().quality()==livekit::OFF) { p->disabled_tracks.insert(sid); p->subscriber->enable_track(sid, false); }
        else { p->disabled_tracks.erase(sid); if (p->subscriptions.contains(sid) && p->subscriptions[sid]) subscribe(p, sid, true); }
      } break;
    case livekit::SignalRequest::kUpdateMetadata: {
      auto* response = res.mutable_request_response(); response->set_request_id(req.update_metadata().request_id()); *response->mutable_update_metadata() = req.update_metadata();
      if (!p->claims.update_metadata) { response->set_reason(livekit::RequestResponse::NOT_ALLOWED); p->send(res); break; }
      metadata_limit(req.update_metadata().metadata()); attributes_limit(req.update_metadata().attributes());
      update_metadata(p, req.update_metadata().name(), req.update_metadata().metadata(), req.update_metadata().attributes());
      response->set_reason(livekit::RequestResponse::OK); p->send(res); break;
    }
    case livekit::SignalRequest::kSyncState: {
      const auto& state = req.sync_state();
      for (const auto& published : state.publish_tracks()) if (!published.has_track() || !p->tracks.contains(published.track().sid())) {
        leave(p, livekit::STATE_MISMATCH, true); return;
      }
      // Data tracks are not implemented yet. An unknown publication cannot be
      // silently accepted as successfully resumed.
      if (!state.publish_data_tracks().empty()) { leave(p, livekit::STATE_MISMATCH, true); return; }
      for (const auto& sid : state.track_sids_disabled()) {
        const auto& participants = room(p->claims.room).participants;
        if (std::any_of(participants.begin(), participants.end(), [&](const auto& entry) {
              return entry.second != p && entry.second->tracks.contains(sid);
            })) { p->disabled_tracks.insert(sid); p->subscriber->enable_track(sid, false); }
      }
      const auto& subscription = state.subscription();
      for (const auto& sid : subscription.track_sids()) subscribe(p, sid, subscription.subscribe());
      for (const auto& part : subscription.participant_tracks())
        for (const auto& sid : part.track_sids()) subscribe(p, sid, subscription.subscribe());
      break;
    }
    case livekit::SignalRequest::kSubscriptionPermission:
      update_subscription_permissions(p, req.subscription_permission()); break;
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
  if (method == "CreateRoom") { require_api(claims.create); return to_json(create(from_json<livekit::CreateRoomRequest>(json)).info); }
  if (method == "ListRooms") {
    require_api(claims.list); auto req = from_json<livekit::ListRoomsRequest>(json); livekit::ListRoomsResponse response;
    for (const auto& [name, r] : rooms_) if (req.names().empty() || contains(req.names(), name)) *response.add_rooms() = r.info;
    return to_json(response);
  }
  if (method == "DeleteRoom") {
    require_api(claims.create); auto req = from_json<livekit::DeleteRoomRequest>(json); auto& r = room(req.room());
    while (!r.participants.empty()) leave(r.participants.begin()->second, livekit::ROOM_DELETED);
    rooms_.erase(req.room()); return Json::object();
  }
  auto name = json.value("room", ""); require_api(claims.admin && claims.room == name);
  if (method == "ListParticipants") {
    from_json<livekit::ListParticipantsRequest>(json); livekit::ListParticipantsResponse res;
    // The default Go local store returns an empty list for a missing room.
    if (auto it = rooms_.find(name); it != rooms_.end())
      for (const auto& [_, p] : it->second.participants) *res.add_participants() = p->info;
    return to_json(res);
  }
  if (method == "MutePublishedTrack") {
    auto req = from_json<livekit::MuteRoomTrackRequest>(json);
    auto found = rooms_.find(name);
    // Go routes this method directly to the participant RPC endpoint. An
    // absent endpoint yields unavailable, before the room's unmute policy.
    if (found == rooms_.end() || !found->second.participants.contains(req.identity()))
      throw Error(503, "unavailable", "no response from servers");
    auto p = found->second.participants.at(req.identity());
    if (!req.muted() && !config_.enable_remote_unmute)
      throw Error(412, "failed_precondition", "remote unmute not enabled");
    livekit::SignalResponse signal; signal.mutable_mute()->set_sid(req.track_sid()); signal.mutable_mute()->set_muted(req.muted()); p->send(signal);
    mute(p, req.track_sid(), req.muted());
    livekit::MuteRoomTrackResponse res;
    if (auto track = p->tracks.find(req.track_sid()); track != p->tracks.end()) *res.mutable_track() = track->second.info;
    return to_json(res);
  }
  auto& r = room(name);
  if (method == "UpdateRoomMetadata") {
    auto req = from_json<livekit::UpdateRoomMetadataRequest>(json); metadata_limit(req.metadata()); r.info.set_metadata(req.metadata());
    livekit::SignalResponse res; *res.mutable_room_update()->mutable_room() = r.info; broadcast(r, res); return to_json(r.info);
  }
  if (method == "SendData") {
    auto req = from_json<livekit::SendDataRequest>(json); if (req.data().size() > 60000) throw Error(400, "invalid_argument", "Data payload too large");
    if (!req.nonce().empty() && req.nonce().size() != 16) throw Error(400, "invalid_argument", "nonce should be 16-bytes or not present");
    livekit::DataPacket packet; auto* user = packet.mutable_user(); user->set_payload(req.data()); if (req.has_topic()) user->set_topic(req.topic()); user->set_nonce(req.nonce());
    *user->mutable_destination_sids() = req.destination_sids(); *packet.mutable_destination_identities() = req.destination_identities();
    route_data(r, nullptr, packet, req.kind() == livekit::DataPacket::RELIABLE); return Json::object();
  }
  auto p = participant(r, json.value("identity", ""));
  if (method == "GetParticipant") { from_json<livekit::RoomParticipantIdentity>(json); return to_json(p->info); }
  if (method == "RemoveParticipant") { from_json<livekit::RoomParticipantIdentity>(json); leave(p, livekit::PARTICIPANT_REMOVED); return Json::object(); }
  if (method == "UpdateSubscriptions") {
    auto req = from_json<livekit::UpdateSubscriptionsRequest>(json); for (const auto& sid : req.track_sids()) subscribe(p, sid, req.subscribe());
    for (const auto& part : req.participant_tracks()) for (const auto& sid : part.track_sids()) subscribe(p, sid, req.subscribe()); return Json::object();
  }
  if (method == "UpdateParticipant") {
    auto req = from_json<livekit::UpdateParticipantRequest>(json); metadata_limit(req.metadata()); attributes_limit(req.attributes());
    // The pinned Go manager applies metadata before changing permissions. In
    // particular, metadata supplied with visible->hidden is still broadcast
    // under the previous visibility; later hidden updates are self-only.
    update_metadata(p, req.name(), req.metadata(), req.attributes());
    const bool permission_changed = req.has_permission() && req.permission().SerializeAsString() != p->info.permission().SerializeAsString();
    if (permission_changed) {
      const auto& permission = req.permission();
      const bool was_subscribing = p->claims.subscribe;
      p->claims.publish = permission.can_publish(); p->claims.subscribe = permission.can_subscribe();
      p->claims.publish_data = permission.can_publish_data(); p->claims.update_metadata = permission.can_update_metadata();
      p->claims.recorder = permission.recorder();
      p->claims.hidden = permission.hidden(); p->claims.agent = permission.agent();
      p->claims.subscribe_metrics = permission.can_subscribe_metrics(); p->claims.manage_agent_session = permission.can_manage_agent_session();
      p->claims.publish_sources.assign(permission.can_publish_sources().begin(), permission.can_publish_sources().end());
      auto& video = p->claims.video_grant;
      video["canPublish"] = p->claims.publish; video["canSubscribe"] = p->claims.subscribe;
      video["canPublishData"] = p->claims.publish_data; video["canUpdateOwnMetadata"] = p->claims.update_metadata;
      video["canSubscribeMetrics"] = p->claims.subscribe_metrics; video["canManageAgentSession"] = p->claims.manage_agent_session;
      video["canPublishSources"] = Json::array();
      for (const auto source : permission.can_publish_sources()) {
        auto name = livekit::TrackSource_Name(source);
        if (name.empty()) name = std::to_string(source);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        video["canPublishSources"].push_back(name);
      }
      *p->info.mutable_permission() = permission;
      std::vector<std::string> revoke;
      for (const auto& [sid, t] : p->tracks) if (!p->claims.publish || (!p->claims.publish_sources.empty() && std::find(p->claims.publish_sources.begin(), p->claims.publish_sources.end(), int(t.info.source())) == p->claims.publish_sources.end())) revoke.push_back(sid);
      for (const auto& sid : revoke) unpublish(p, sid);
      for (const auto& [_, other] : r.participants) if (other != p) for (const auto& [sid, t] : other->tracks) {
        if (!p->claims.subscribe) p->subscriber->remove_track(sid);
        else if ((p->subscriptions.contains(sid) ? p->subscriptions[sid] : p->auto_subscribe) &&
                 (!was_subscribing || !p->subscription_allowed.contains(sid) || !p->subscription_allowed[sid])) {
          // Go ReconcileAll retries pending subscriptions; it does not recheck
          // established ones merely because Recorder changed. A publisher ACL
          // update re-evaluates all desired subscriptions, including these.
          subscribe(p, sid, true);
        }
      }
    }
    if (permission_changed) { updated(p); refresh_token(p); } return to_json(p->info);
  }
  throw Error(501, "unimplemented", "RoomService method is not implemented");
}
struct Rooms::ApiRpc {
  uint64_t id;
  int attempts = 0;
  std::string room, identity;
  std::optional<livekit::PerformRpcRequest> request;
  RpcRequests::Complete complete;
  RpcRequests::Cancel cancel;
  boost::asio::steady_timer discovery;
  ApiRpc(boost::asio::io_context& io, uint64_t key, RpcRequests::Complete done)
      : id(key), complete(std::move(done)), discovery(io) {}
};
RpcRequests::Cancel Rooms::participant_api(const std::string& method, const Json& json, const Claims& claims, RpcRequests::Complete complete) {
  auto pending = std::make_shared<ApiRpc>(io_, ++next_api_rpc_, std::move(complete));
  if (method == "PerformRpc") {
    auto req = from_json<livekit::PerformRpcRequest>(json);
    require_api(claims.admin && claims.room == req.room());
    if (req.destination_identity().empty()) throw Error(400, "invalid_argument", "destination identity is required");
    pending->room = req.room(); pending->identity = req.destination_identity();
    pending->request = std::move(req);
  } else if (method == "ForwardParticipant" || method == "MoveParticipant") {
    // Both requests have the same fields. HTTP has already validated against
    // the corresponding descriptor; retain the exact Go permission order.
    auto req = from_json<livekit::ForwardParticipantRequest>(json);
    require_api(claims.admin && claims.room == req.room() && claims.destination_room == req.destination_room());
    if (req.room() == req.destination_room()) {
      const std::string argument = "destination room cannot be the same as source room";
      throw Error(400, "invalid_argument", argument + " ", {{"argument", argument}});
    }
    pending->room = req.room(); pending->identity = req.identity();
  } else throw Error(501, "unimplemented", "RoomService method is not implemented");
  if (stopping_) throw Error(503, "unavailable", "Server is stopping");
  api_rpcs_.emplace(pending->id, pending);
  attempt_rpc(pending);
  return [this, alive = std::weak_ptr<int>(rpc_lifetime_), id = pending->id] {
    if (!alive.expired()) finish_rpc(id, std::nullopt);
  };
}
void Rooms::finish_rpc(uint64_t id, std::optional<RpcRequests::Result> result) {
  auto it = api_rpcs_.find(id);
  if (it == api_rpcs_.end()) return;
  auto pending = std::move(it->second);
  api_rpcs_.erase(it);
  pending->discovery.cancel();
  if (pending->cancel) pending->cancel();
  if (result) pending->complete(std::move(*result));
}
void Rooms::attempt_rpc(const std::shared_ptr<ApiRpc>& pending) {
  if (!api_rpcs_.contains(pending->id)) return;
  ++pending->attempts;
  std::shared_ptr<Participant> target;
  if (auto room_it = rooms_.find(pending->room); room_it != rooms_.end()) {
    if (auto it = room_it->second.participants.find(pending->identity);
        it != room_it->second.participants.end() && !it->second->closed) target = it->second;
  }
  if (!target) {
    // Match the pinned local PSRPC client's three attempts and one-second
    // discovery interval. A participant joining before the next attempt can
    // receive the request; an absent target does not create a room/session.
    pending->discovery.expires_after(std::chrono::seconds(1));
    pending->discovery.async_wait([this, alive = std::weak_ptr<int>(rpc_lifetime_), weak = std::weak_ptr<ApiRpc>(pending)](boost::system::error_code error) {
      if (error || alive.expired()) return;
      auto current = weak.lock();
      if (!current || !api_rpcs_.contains(current->id)) return;
      if (current->attempts < 3) attempt_rpc(current);
      else finish_rpc(current->id, Error(503, "unavailable", "no response from servers"));
    });
    return;
  }
  if (!pending->request) {
    // Pinned open-source Go RoomManager implements neither cross-room method.
    // Endpoint discovery still occurs first; do not fabricate a successful move.
    finish_rpc(pending->id, Error(500, "unknown", "not implemented"));
    return;
  }
  auto complete = [this, alive = std::weak_ptr<int>(rpc_lifetime_), weak = std::weak_ptr<ApiRpc>(pending)](RpcRequests::Result result) {
    if (alive.expired()) return;
    auto current = weak.lock();
    if (!current || !api_rpcs_.contains(current->id)) return;
    if (auto error = std::get_if<Error>(&result); error &&
        (error->code == "unavailable" || error->code == "deadline_exceeded") && current->attempts < 3) {
      attempt_rpc(current);
    } else finish_rpc(current->id, std::move(result));
  };
  try {
    const auto& req = *pending->request;
    auto cancel = rpc_.start(target->info.sid(), req.method(), req.payload(), req.response_timeout_ms(), target->last_rtt_ms,
        [weak = std::weak_ptr<Participant>(target)](const livekit::DataPacket& packet) {
          auto p = weak.lock();
          return p && !p->closed && p->subscriber->send_data(packet.SerializeAsString(), true);
        }, complete);
    if (api_rpcs_.contains(pending->id)) pending->cancel = std::move(cancel);
    else cancel();
  } catch (const Error& error) { complete(error); }
  catch (const std::exception&) { complete(Error(500, "internal", "Internal server error")); }
}
Json Rooms::metrics() const {
  size_t participants = 0, tracks = 0;
  for (const auto& [_, r] : rooms_) for (const auto& [__, p] : r.participants) { ++participants; tracks += p->tracks.size(); }
  auto result = rtc_.media_metrics();
  result["rooms"] = rooms_.size(); result["participants"] = participants; result["tracks"] = tracks;
  result["rpc_pending"] = api_rpcs_.size();
  return result;
}
void Rooms::tick() {
  timer_.expires_after(std::chrono::seconds(1));
  timer_.async_wait([this](boost::system::error_code error) {
    if (error || stopping_) return;
    std::vector<std::shared_ptr<Participant>> expired;
    for (const auto& [_, r] : rooms_) for (const auto& [__, p] : r.participants) if (now_ms() - p->last_seen > 30000) expired.push_back(p);
    for (auto& p : expired) {
      if (auto sink = p->sink.lock()) { signal_closed(p, sink.get()); sink->close(); }
      else if (!p->ice_connected) leave(p, livekit::CONNECTION_TIMEOUT);
    }
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [_, r] : rooms_) for (const auto& [__, p] : r.participants) if (now >= p->next_token_refresh) {
      refresh_token(p); p->next_token_refresh = now + std::chrono::minutes(5);
    }
    for (auto it = rooms_.begin(); it != rooms_.end();) {
      auto& r = it->second;
      const bool occupied = std::any_of(r.participants.begin(), r.participants.end(),
        [](const auto& entry) { return !entry.second->claims.is_dependent(); });
      const bool departed = r.used && r.last_left_ms != 0;
      const auto since = departed ? r.last_left_ms : r.created_at_ms;
      const auto timeout = departed ? r.info.departure_timeout() : r.info.empty_timeout();
      if (!occupied && now_ms() - since >= 1000LL * timeout) {
        while (!r.participants.empty()) leave(r.participants.begin()->second, livekit::ROOM_CLOSED);
        it = rooms_.erase(it);
      } else ++it;
    }
    tick();
  });
}
void Rooms::shutdown() {
  if (stopping_) return;
  stopping_ = true; timer_.cancel();
  while (!api_rpcs_.empty()) finish_rpc(api_rpcs_.begin()->first, Error(503, "unavailable", "no response from servers"));
  rpc_lifetime_.reset();
  rpc_.shutdown();
  for (auto& [_, r] : rooms_) while (!r.participants.empty()) leave(r.participants.begin()->second, livekit::SERVER_SHUTDOWN, true);
  rooms_.clear();
}
}
