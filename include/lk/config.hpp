#pragma once
#include "lk/common.hpp"
#include <map>
#include <vector>
namespace lk {
struct IceServer { std::vector<std::string> urls; std::string username, credential; };
struct Config {
  std::string bind = "127.0.0.1";
  uint16_t port = 7880;
  uint16_t rtc_port_min = 50000, rtc_port_max = 50100;
  std::map<std::string, std::string> keys;
  std::vector<IceServer> ice_servers;
  size_t max_connections = 256;
  static Config load(const std::string& path);
  void validate() const;
};
}
