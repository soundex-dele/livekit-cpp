#pragma once
#include "lk/common.hpp"
#include <map>
#include <optional>
#include <vector>
namespace lk {
struct IceServer { std::vector<std::string> urls; std::string username, credential; };
struct TurnRestAuth {
  std::vector<std::string> urls;
  std::string shared_secret;
  int credential_ttl_seconds = 86400;
  void validate() const;
  IceServer issue(const std::string& session_id, int64_t now) const;
};
struct Limits {
  int64_t max_room_name_length = 256;
  int64_t max_participant_identity_length = 256;
  int64_t max_participant_name_length = 256;
  uint32_t max_metadata_size = 512 * 1024;
  uint32_t max_attributes_size = 64 * 1024;
  // Go treats zero as unlimited and negative signed length limits as rejecting
  // every value. Compare without converting a negative limit to size_t.
  static bool exceeded(size_t bytes, int64_t limit) {
    return limit < 0 || (limit != 0 && bytes > static_cast<uint64_t>(limit));
  }
};
struct Config {
  std::string bind = "127.0.0.1";
  uint16_t port = 7880;
  uint16_t rtc_port_min = 50000, rtc_port_max = 50100;
  std::map<std::string, std::string> keys;
  std::vector<IceServer> ice_servers;
  std::optional<TurnRestAuth> turn_rest;
  size_t max_connections = 256;
  bool enable_remote_unmute = false;
  bool congestion_control = true, allow_video_pause = false;
  Limits limits;
  std::string media_mode = "sfu";
  static Config load(const std::string& path, bool complete = true);
  static Config parse(const std::string& text, bool complete = true);
  static std::map<std::string,std::string> parse_keys(const std::string& text);
  void validate() const;
  std::vector<IceServer> session_ice_servers(const std::string& session_id, int64_t now = now_ms() / 1000) const;
};
}
