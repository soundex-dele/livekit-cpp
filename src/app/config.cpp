#include "lk/config.hpp"
#include <fstream>
#include <cstdlib>
namespace lk {
Config Config::load(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("Cannot read configuration: " + path);
  Json j; f >> j;
  Config c;
  for (const auto& item : j.items()) {
    if (item.key() != "bind" && item.key() != "port" && item.key() != "rtc" && item.key() != "keys" && item.key() != "max_connections" && item.key() != "media_mode")
      throw std::runtime_error("Unknown configuration key: " + item.key());
  }
  c.bind = j.value("bind", c.bind);
  c.media_mode = j.value("media_mode", c.media_mode);
  auto port = j.value("port", int(c.port));
  if (port < 1 || port > 65535) throw std::runtime_error("port must be 1..65535");
  c.port = static_cast<uint16_t>(port);
  c.keys = j.at("keys").get<decltype(c.keys)>();
  auto max_connections = j.value("max_connections", 256);
  if (max_connections < 1 || max_connections > 10000) throw std::runtime_error("max_connections must be 1..10000");
  c.max_connections = static_cast<size_t>(max_connections);
  if (j.contains("rtc")) {
    auto rtc = j["rtc"];
    if (!rtc.is_object()) throw std::runtime_error("rtc must be an object");
    for (const auto& item : rtc.items())
      if (item.key() != "port_range_start" && item.key() != "port_range_end" && item.key() != "ice_servers" && item.key() != "turn_rest")
        throw std::runtime_error("Unknown rtc configuration key: " + item.key());
    auto min = rtc.value("port_range_start", int(c.rtc_port_min));
    auto max = rtc.value("port_range_end", int(c.rtc_port_max));
    if (min < 1 || max > 65535 || min > max) throw std::runtime_error("Invalid RTC port range");
    c.rtc_port_min = static_cast<uint16_t>(min); c.rtc_port_max = static_cast<uint16_t>(max);
    for (auto& s : rtc.value("ice_servers", Json::array()))
      c.ice_servers.push_back({s.at("urls").get<std::vector<std::string>>(), s.value("username", ""), s.value("credential", "")});
    if (rtc.contains("turn_rest")) {
      const auto& rest = rtc.at("turn_rest");
      if (!rest.is_object()) throw std::runtime_error("rtc.turn_rest must be an object");
      for (const auto& item : rest.items())
        if (item.key() != "urls" && item.key() != "shared_secret" && item.key() != "shared_secret_env" && item.key() != "credential_ttl_seconds")
          throw std::runtime_error("Unknown rtc.turn_rest configuration key: " + item.key());
      if (rest.contains("shared_secret") == rest.contains("shared_secret_env"))
        throw std::runtime_error("Configure exactly one of rtc.turn_rest shared_secret or shared_secret_env");
      TurnRestAuth auth;
      auth.urls = rest.at("urls").get<std::vector<std::string>>();
      if (rest.contains("credential_ttl_seconds") && !rest.at("credential_ttl_seconds").is_number_integer())
        throw std::runtime_error("rtc.turn_rest credential_ttl_seconds must be an integer");
      const auto ttl = rest.value("credential_ttl_seconds", int64_t(auth.credential_ttl_seconds));
      if (ttl < 60 || ttl > 604800) throw std::runtime_error("rtc.turn_rest credential_ttl_seconds must be 60..604800");
      auth.credential_ttl_seconds = static_cast<int>(ttl);
      if (rest.contains("shared_secret_env")) {
        const auto name = rest.at("shared_secret_env").get<std::string>();
        if (name.empty() || name.size() > 128 || name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
          throw std::runtime_error("Invalid rtc.turn_rest shared_secret_env name");
        const auto* secret = std::getenv(name.c_str());
        if (!secret || !*secret) throw std::runtime_error("rtc.turn_rest shared secret environment variable is unset or empty");
        auth.shared_secret = secret;
      } else auth.shared_secret = rest.at("shared_secret").get<std::string>();
      c.turn_rest = std::move(auth);
    }
  }
  c.validate(); return c;
}
void Config::validate() const {
  if (media_mode != "sfu" && media_mode != "relay") throw std::runtime_error("media_mode must be sfu or relay");
  if (keys.empty()) throw std::runtime_error("Configure API keys or use --dev");
  for (const auto& [key, secret] : keys) if (key.empty() || secret.empty()) throw std::runtime_error("API key and secret must not be empty");
  if (turn_rest) turn_rest->validate();
}
}
