#pragma once
#include "lk/config.hpp"
namespace lk {
struct Claims {
  std::string identity, name, metadata, room;
  Json attributes = Json::object();
  bool join = false, create = false, list = false, admin = false;
  bool publish = true, subscribe = true, publish_data = true, update_metadata = false;
  std::vector<int> publish_sources;
};
class Auth {
 public:
  explicit Auth(std::map<std::string, std::string> keys) : keys_(std::move(keys)) {}
  Claims verify(const std::string& token, int64_t now = now_ms() / 1000) const;
  static std::string sign(const Json& claims, const std::string& secret);
 private:
  std::map<std::string, std::string> keys_;
};
}
