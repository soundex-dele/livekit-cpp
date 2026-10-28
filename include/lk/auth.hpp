#pragma once
#include "lk/config.hpp"
namespace lk {
struct Claims {
  std::string identity, name, metadata, room, destination_room, kind;
  Json attributes = Json::object();
  Json video_grant = Json::object(), room_config;
  std::string room_preset;
  std::string studio_grant;
  std::string conference_grant;
  int64_t expires_at = 0;
  bool join = false, create = false, list = false, admin = false;
  bool publish = true, subscribe = true, publish_data = true, update_metadata = false;
  bool recorder = false, hidden = false, agent = false;
  bool subscribe_metrics = false, manage_agent_session = false;
  bool is_recorder() const { return recorder || kind == "egress"; }
  bool is_dependent() const { return is_recorder() || agent || kind == "agent"; }
  std::vector<std::string> kind_details;
  std::vector<int> publish_sources;
};
class Auth {
 public:
  explicit Auth(std::map<std::string, std::string> keys) : keys_(std::move(keys)) {}
  Claims verify(const std::string& token, int64_t now = now_ms() / 1000) const;
  std::string refresh(const Claims&, int64_t now = now_ms() / 1000) const;
  static std::string sign(const Json& claims, const std::string& secret);
 private:
  std::map<std::string, std::string> keys_;
};
}
