#include "lk/config.hpp"
#include <fstream>
namespace lk {
Config Config::load(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("Cannot read configuration: " + path);
  Json j; f >> j;
  Config c;
  for (const auto& item : j.items()) {
    if (item.key() != "bind" && item.key() != "port" && item.key() != "rtc" && item.key() != "keys" && item.key() != "max_connections")
      throw std::runtime_error("Unknown configuration key: " + item.key());
  }
  c.bind = j.value("bind", c.bind);
  auto port = j.value("port", int(c.port));
  if (port < 1 || port > 65535) throw std::runtime_error("port must be 1..65535");
  c.port = static_cast<uint16_t>(port);
  c.keys = j.at("keys").get<decltype(c.keys)>();
  auto max_connections = j.value("max_connections", 256);
  if (max_connections < 1 || max_connections > 10000) throw std::runtime_error("max_connections must be 1..10000");
  c.max_connections = static_cast<size_t>(max_connections);
  if (j.contains("rtc")) {
    auto rtc = j["rtc"];
    auto min = rtc.value("port_range_start", int(c.rtc_port_min));
    auto max = rtc.value("port_range_end", int(c.rtc_port_max));
    if (min < 1 || max > 65535 || min > max) throw std::runtime_error("Invalid RTC port range");
    c.rtc_port_min = static_cast<uint16_t>(min); c.rtc_port_max = static_cast<uint16_t>(max);
    for (auto& s : rtc.value("ice_servers", Json::array()))
      c.ice_servers.push_back({s.at("urls").get<std::vector<std::string>>(), s.value("username", ""), s.value("credential", "")});
  }
  c.validate(); return c;
}
void Config::validate() const {
  if (keys.empty()) throw std::runtime_error("Configure API keys or use --dev");
  for (const auto& [key, secret] : keys) if (key.empty() || secret.empty()) throw std::runtime_error("API key and secret must not be empty");
}
}
