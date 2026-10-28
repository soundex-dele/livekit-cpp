#include "lk/auth.hpp"
#include "livekit_room.pb.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
namespace lk {
namespace {
std::string encode(const unsigned char* data, size_t size) {
  std::string out(4 * ((size + 2) / 3), '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(size));
  while (!out.empty() && out.back() == '=') out.pop_back();
  std::replace(out.begin(), out.end(), '+', '-'); std::replace(out.begin(), out.end(), '/', '_');
  return out;
}
std::string encode(const std::string& s) { return encode(reinterpret_cast<const unsigned char*>(s.data()), s.size()); }
std::string decode(std::string s) {
  if (s.empty() || s.size() % 4 == 1 || s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos)
    throw std::runtime_error("Invalid base64url");
  std::replace(s.begin(), s.end(), '-', '+'); std::replace(s.begin(), s.end(), '_', '/');
  size_t pad = (4 - s.size() % 4) % 4;
  s.append(pad, '=');
  std::string result(s.size() / 4 * 3, '\0');
  int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(result.data()), reinterpret_cast<const unsigned char*>(s.data()), static_cast<int>(s.size()));
  if (n < 0) throw std::runtime_error("Invalid base64url");
  result.resize(static_cast<size_t>(n) - pad); return result;
}
std::string mac(const std::string& data, const std::string& secret) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> out{}; unsigned int size = 0;
  if (!HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()), reinterpret_cast<const unsigned char*>(data.data()), data.size(), out.data(), &size))
    throw std::runtime_error("HMAC failed");
  return {reinterpret_cast<const char*>(out.data()), size};
}
}
std::string random_id(const std::string& prefix) {
  std::array<unsigned char, 12> data{};
  if (RAND_bytes(data.data(), static_cast<int>(data.size())) != 1) throw std::runtime_error("Random generator failed");
  return prefix + encode(data.data(), data.size());
}
std::string Auth::sign(const Json& claims, const std::string& secret) {
  auto content = encode(std::string("{\"alg\":\"HS256\",\"typ\":\"JWT\"}")) + "." + encode(claims.dump());
  return content + "." + encode(mac(content, secret));
}
Claims Auth::verify(const std::string& token, int64_t now) const {
  try {
    if (token.size() > 32768) throw std::runtime_error("Token too large");
    auto a = token.find('.'), b = token.find('.', a == std::string::npos ? 0 : a + 1);
    if (a == std::string::npos || b == std::string::npos || token.find('.', b + 1) != std::string::npos) throw std::runtime_error("Invalid token");
    auto header = Json::parse(decode(token.substr(0, a)));
    if (header.value("alg", "") != "HS256" || header.contains("crit")) throw std::runtime_error("Unsupported JWT header");
    auto claims = Json::parse(decode(token.substr(a + 1, b - a - 1)));
    auto key = keys_.find(claims.value("iss", ""));
    if (key == keys_.end()) throw std::runtime_error("Unknown issuer");
    auto expected = mac(token.substr(0, b), key->second), actual = decode(token.substr(b + 1));
    if (expected.size() != actual.size() || CRYPTO_memcmp(expected.data(), actual.data(), expected.size()) != 0) throw std::runtime_error("Invalid signature");
    // The pinned protocol verifier requires exp and allows one minute of clock
    // skew for exp/nbf. Compare as doubles to avoid overflowing signed integers.
    if (!claims.contains("exp") || !claims["exp"].is_number() ||
        double(now) >= std::floor(claims["exp"].get<double>()) + 60) throw std::runtime_error("Token expired");
    if (claims.contains("nbf") && !claims["nbf"].is_null() &&
        (!claims["nbf"].is_number() || double(now) + 60 < std::floor(claims["nbf"].get<double>()))) throw std::runtime_error("Token not active");
    Claims c;
    c.expires_at = claims.value("exp", int64_t(0));
    c.studio_grant = claims.value("studio_grant", "");
    c.conference_grant = claims.value("conference_grant", "");
    c.room_preset = claims.value("roomPreset", "");
    if (claims.contains("roomConfig") && !claims["roomConfig"].is_null())
      c.room_config = to_json(from_json<livekit::RoomConfiguration>(claims["roomConfig"]));
    c.identity = claims.value("sub", "");
    if (c.identity.empty()) c.identity = claims.value("jti", "");
    c.name = claims.value("name", ""); c.metadata = claims.value("metadata", "");
    c.attributes = claims.value("attributes", Json::object());
    if (!c.attributes.is_object()) throw std::runtime_error("Invalid attributes");
    for (auto& v : c.attributes) if (!v.is_string()) throw std::runtime_error("Invalid attribute value");
    auto v = claims.value("video", Json::object());
    // Retain known grants and optional-bool presence for refreshed JWTs. Unknown
    // fields are discarded by Go's typed grant decoder, not signed back out.
    for (auto field : {"roomCreate", "roomList", "roomRecord", "roomAdmin", "roomJoin", "room", "canPublish",
                       "canSubscribe", "canPublishData", "canPublishSources", "canUpdateOwnMetadata", "ingressAdmin",
                       "hidden", "recorder", "agent", "canSubscribeMetrics", "canManageAgentSession", "destinationRoom"})
      if (v.contains(field) && !v[field].is_null()) c.video_grant[field] = v[field];
    v = c.video_grant;
    for (auto field : {"roomCreate", "roomList", "roomRecord", "roomAdmin", "roomJoin", "canPublish", "canSubscribe",
                       "canPublishData", "canUpdateOwnMetadata", "ingressAdmin", "hidden", "recorder", "agent",
                       "canSubscribeMetrics", "canManageAgentSession"})
      if (v.contains(field) && !v[field].is_boolean()) throw std::runtime_error("Invalid video grant");
    c.room = v.value("room", ""); c.join = v.value("roomJoin", false); c.create = v.value("roomCreate", false);
    c.destination_room = v.value("destinationRoom", "");
    c.recorder = v.value("recorder", false); c.kind = claims.value("kind", "");
    c.hidden = v.value("hidden", false); c.agent = v.value("agent", false);
    c.kind_details = claims.value("kindDetails", std::vector<std::string>{});
    std::transform(c.kind.begin(), c.kind.end(), c.kind.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    c.list = v.value("roomList", false); c.admin = v.value("roomAdmin", false);
    c.publish = v.value("canPublish", true); c.subscribe = v.value("canSubscribe", true);
    c.publish_data = v.value("canPublishData", c.publish); c.update_metadata = v.value("canUpdateOwnMetadata", false);
    c.subscribe_metrics = v.value("canSubscribeMetrics", false); c.manage_agent_session = v.value("canManageAgentSession", false);
    for (const auto& source : v.value("canPublishSources", Json::array())) {
      if (source.is_number_integer()) c.publish_sources.push_back(source.get<int>());
      else if (source == "camera") c.publish_sources.push_back(1);
      else if (source == "microphone") c.publish_sources.push_back(2);
      else if (source == "screen_share") c.publish_sources.push_back(3);
      else if (source == "screen_share_audio") c.publish_sources.push_back(4);
      else throw std::runtime_error("Invalid publish source");
    }
    return c;
  } catch (const std::exception&) { throw Error(401, "unauthenticated", "Invalid or expired access token"); }
}
std::string Auth::refresh(const Claims& c, int64_t now) const {
  if (keys_.empty()) throw std::runtime_error("No signing key configured");
  auto video = c.video_grant;
  auto flag = [&](const char* field, bool value) { if (value) video[field] = true; else video.erase(field); };
  auto optional = [&](const char* field, bool value, bool fallback) {
    if (video.contains(field) || value != fallback) video[field] = value;
  };
  flag("roomJoin", c.join); flag("roomCreate", c.create); flag("roomList", c.list); flag("roomAdmin", c.admin);
  flag("hidden", c.hidden); flag("recorder", c.recorder); flag("agent", c.agent);
  optional("canPublish", c.publish, true); optional("canSubscribe", c.subscribe, true);
  optional("canPublishData", c.publish_data, c.publish); optional("canUpdateOwnMetadata", c.update_metadata, false);
  optional("canSubscribeMetrics", c.subscribe_metrics, false); optional("canManageAgentSession", c.manage_agent_session, false);
  if (!c.room.empty()) video["room"] = c.room; else video.erase("room");
  if (!c.destination_room.empty()) video["destinationRoom"] = c.destination_room; else video.erase("destinationRoom");
  if (video.contains("canPublishSources") && video["canPublishSources"].empty()) video.erase("canPublishSources");
  for (auto field : {"roomRecord", "ingressAdmin"}) if (video.contains(field) && video[field] == false) video.erase(field);
  Json payload{{"iss", keys_.begin()->first}, {"sub", c.identity}, {"identity", c.identity},
               {"iat", now}, {"nbf", now}, {"exp", std::max(now + 600, c.expires_at)}, {"video", video}};
  const auto kind = c.kind;
  if (!c.studio_grant.empty()) payload["studio_grant"] = c.studio_grant;
  if (!c.conference_grant.empty()) payload["conference_grant"] = c.conference_grant;
  payload["kind"] = (kind == "ingress" || kind == "egress" || kind == "sip" || kind == "agent" ||
                     kind == "connector" || kind == "bridge") ? kind : "standard";
  if (!c.name.empty()) payload["name"] = c.name;
  if (!c.metadata.empty()) payload["metadata"] = c.metadata;
  if (!c.attributes.empty()) payload["attributes"] = c.attributes;
  if (!c.room_preset.empty()) payload["roomPreset"] = c.room_preset;
  if (!c.room_config.is_null()) {
    // Match protocol/auth RoomConfiguration.CheckCredentials: refresh must not
    // expose worker output credentials or stream keys in a client JWT.
    auto credentials = [](const Json& output) {
      for (auto pair : {std::pair{"s3", "secret"}, {"gcp", "credentials"}, {"azure", "accountKey"}, {"aliOSS", "secret"}})
        if (output.contains(pair.first) && !output[pair.first].value(pair.second, std::string()).empty()) return true;
      return false;
    };
    if (c.room_config.contains("egress")) {
      const auto& egress = c.room_config["egress"];
      if (egress.contains("tracks") && credentials(egress["tracks"])) throw std::runtime_error("Room output contains credentials");
      for (auto type : {"participant", "room"}) if (egress.contains(type)) {
        const auto& output = egress[type];
        for (auto field : {"fileOutputs", "segmentOutputs"}) if (output.contains(field))
          for (const auto& file : output[field]) if (credentials(file)) throw std::runtime_error("Room output contains credentials");
        if (std::string(type) == "room" && output.contains("streamOutputs") && !output["streamOutputs"].empty())
          throw std::runtime_error("Room output contains stream keys");
      }
    }
    payload["roomConfig"] = c.room_config;
  }
  // The pinned Go refresh intentionally carries neither kindDetails nor the
  // independent SIP/Agent/etc grants, audience, jti, or arbitrary JWT claims.
  return sign(payload, keys_.begin()->second);
}
}
