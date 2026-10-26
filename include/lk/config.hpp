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
struct Config {
  std::string bind = "127.0.0.1";
  uint16_t port = 7880;
  uint16_t rtc_port_min = 50000, rtc_port_max = 50100;
  std::map<std::string, std::string> keys;
  std::vector<IceServer> ice_servers;
  std::optional<TurnRestAuth> turn_rest;
  size_t max_connections = 256;
  std::string media_mode = "sfu";
  static Config load(const std::string& path);
  void validate() const;
  std::vector<IceServer> session_ice_servers(const std::string& session_id, int64_t now = now_ms() / 1000) const;
};
}
