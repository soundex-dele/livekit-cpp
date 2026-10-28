#include "lk/config.hpp"
#include <fstream>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <yaml-cpp/yaml.h>
namespace lk {
namespace {
Json yaml_value(const YAML::Node& node, size_t& remaining, unsigned depth=0, bool string_values=false,
                bool config_document=true, bool boolean_field=false) {
  if (!remaining-- || depth>64) throw std::runtime_error("Configuration nesting or alias expansion exceeds limit");
  if (node.IsNull()) return nullptr;
  if (node.IsMap()) {
    Json object=Json::object();
    for (const auto& entry:node) {
      if (!entry.first.IsScalar()) throw std::runtime_error("Configuration mapping keys must be scalars");
      auto key=entry.first.as<std::string>();
      if (object.contains(key)) throw std::runtime_error("Duplicate configuration mapping key");
      object[key]=yaml_value(entry.second,remaining,depth+1,string_values || (config_document && depth==0 && key=="keys"),
                            config_document,config_document && depth==1 && key=="enable_remote_unmute");
    }
    return object;
  }
  if (node.IsSequence()) {
    Json array=Json::array();
    for (const auto& entry:node) array.push_back(yaml_value(entry,remaining,depth+1,string_values,config_document));
    return array;
  }
  if (!node.IsScalar()) throw std::runtime_error("Unsupported configuration value");
  if (string_values || node.Tag()=="!" || node.Tag()=="tag:yaml.org,2002:str") return node.Scalar();
  bool boolean;
  const auto scalar=node.Scalar();
  // Go yaml.v3 resolves yes/no/on/off as strings in map[string]any, but
  // accepts them for typed bool fields. In particular --keys "key: on"
  // must keep that string secret, not discard it as a boolean.
  const bool bool_literal=scalar=="true" || scalar=="True" || scalar=="TRUE" ||
                          scalar=="false" || scalar=="False" || scalar=="FALSE";
  if ((boolean_field || bool_literal || node.Tag()=="tag:yaml.org,2002:bool") && YAML::convert<bool>::decode(node,boolean)) return boolean;
  int64_t integer;
  if (YAML::convert<int64_t>::decode(node,integer)) return integer;
  double number;
  if (YAML::convert<double>::decode(node,number)) {
    if (!std::isfinite(number)) throw std::runtime_error("Non-finite configuration number");
    return number;
  }
  return node.Scalar();
}
Json parse_yaml(const std::string& text,bool config_document=true) {
  try {
    const auto documents=YAML::LoadAll(text);
    if (documents.empty()) return Json::object();
    if (documents.size()!=1) throw std::runtime_error("Configuration must contain one document");
    size_t remaining=100000;
    auto value=yaml_value(documents.front(),remaining,0,false,config_document);
    if (!value.is_object()) throw std::runtime_error("Configuration must be an object");
    return value;
  } catch (const YAML::Exception&) {
    // Parser messages may quote credentials from the input.
    throw std::runtime_error("Invalid YAML/JSON configuration");
  }
}
int64_t integer_field(const Json& object, const std::string& key, int64_t fallback,
                      int64_t minimum = INT64_MIN, int64_t maximum = INT64_MAX) {
  if (!object.contains(key) || object.at(key).is_null()) return fallback;
  const auto& value = object.at(key);
  if (!value.is_number_integer() ||
      (value.is_number_unsigned() && value.get<uint64_t>() > static_cast<uint64_t>(maximum)))
    throw std::runtime_error(key + " must be an integer in range");
  const auto number = value.get<int64_t>();
  if (number < minimum || number > maximum) throw std::runtime_error(key + " must be an integer in range");
  return number;
}
}
Config Config::load(const std::string& path, bool complete) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("Cannot read configuration: " + path);
  return parse(std::string(std::istreambuf_iterator<char>(f),{}),complete);
}
std::map<std::string,std::string> Config::parse_keys(const std::string& text) {
  const auto values=parse_yaml(text,false);
  std::map<std::string,std::string> keys;
  // Go's --keys unmarshals map[string]any and retains only string secrets.
  for (const auto& item:values.items()) if (item.value().is_string()) keys[item.key()]=item.value().get<std::string>();
  return keys;
}
Config Config::parse(const std::string& text, bool complete) {
  const auto j=parse_yaml(text);
  Config c;
  for (const auto& item : j.items()) {
    if (item.key() != "bind" && item.key() != "bind_addresses" && item.key() != "port" && item.key() != "rtc" && item.key() != "room" && item.key() != "keys" && item.key() != "max_connections" && item.key() != "media_mode" && item.key() != "limit")
      throw std::runtime_error("Unknown configuration key: " + item.key());
  }
  c.bind = j.value("bind", c.bind);
  if (j.contains("bind_addresses")) {
    if (j.contains("bind")) throw std::runtime_error("Use either bind or bind_addresses");
    const auto& addresses=j.at("bind_addresses");
    if (!addresses.is_array() || addresses.size()!=1 || !addresses.front().is_string())
      throw std::runtime_error("bind_addresses currently requires exactly one address");
    c.bind=addresses.front().get<std::string>();
  }
  c.media_mode = j.value("media_mode", c.media_mode);
  auto port = j.value("port", int(c.port));
  if (port < 1 || port > 65535) throw std::runtime_error("port must be 1..65535");
  c.port = static_cast<uint16_t>(port);
  if (j.contains("keys")) c.keys = j.at("keys").get<decltype(c.keys)>();
  auto max_connections = j.value("max_connections", 256);
  if (max_connections < 1 || max_connections > 10000) throw std::runtime_error("max_connections must be 1..10000");
  c.max_connections = static_cast<size_t>(max_connections);
  if (j.contains("limit") && !j.at("limit").is_null()) {
    const auto& limits = j.at("limit");
    if (!limits.is_object()) throw std::runtime_error("limit must be an object");
    for (const auto& item : limits.items())
      if (item.key() != "max_room_name_length" && item.key() != "max_participant_identity_length" &&
          item.key() != "max_participant_name_length" && item.key() != "max_metadata_size" && item.key() != "max_attributes_size")
        throw std::runtime_error("Unsupported limit configuration key: " + item.key());
    auto& l = c.limits;
    l.max_room_name_length = integer_field(limits, "max_room_name_length", l.max_room_name_length);
    l.max_participant_identity_length = integer_field(limits, "max_participant_identity_length", l.max_participant_identity_length);
    l.max_participant_name_length = integer_field(limits, "max_participant_name_length", l.max_participant_name_length);
    l.max_metadata_size = static_cast<uint32_t>(integer_field(limits, "max_metadata_size", l.max_metadata_size, 0, UINT32_MAX));
    l.max_attributes_size = static_cast<uint32_t>(integer_field(limits, "max_attributes_size", l.max_attributes_size, 0, UINT32_MAX));
  }
  if (j.contains("room")) {
    const auto& room = j.at("room");
    if (!room.is_object()) throw std::runtime_error("room must be an object");
    for (const auto& item : room.items())
      if (item.key() != "enable_remote_unmute" && item.key() != "max_metadata_size" &&
          item.key() != "max_room_name_length" && item.key() != "max_participant_identity_length")
        throw std::runtime_error("Unknown room configuration key: " + item.key());
    c.enable_remote_unmute = room.value("enable_remote_unmute", false);
    // The pinned Go config copies nonzero legacy room limits over limit.*.
    const auto metadata = integer_field(room, "max_metadata_size", 0, 0, UINT32_MAX);
    const auto name = integer_field(room, "max_room_name_length", 0);
    const auto identity = integer_field(room, "max_participant_identity_length", 0);
    if (metadata) c.limits.max_metadata_size = static_cast<uint32_t>(metadata);
    if (name) c.limits.max_room_name_length = name;
    if (identity) c.limits.max_participant_identity_length = identity;
  }
  if (j.contains("rtc")) {
    auto rtc = j["rtc"];
    if (!rtc.is_object()) throw std::runtime_error("rtc must be an object");
    for (const auto& item : rtc.items())
      if (item.key() != "port_range_start" && item.key() != "port_range_end" && item.key() != "ice_servers" && item.key() != "turn_rest" && item.key() != "congestion_control")
        throw std::runtime_error("Unknown rtc configuration key: " + item.key());
    if(rtc.contains("congestion_control")){
      const auto& cc=rtc.at("congestion_control");
      if(!cc.is_object())throw std::runtime_error("rtc.congestion_control must be an object");
      for(const auto& item:cc.items()){
        if(item.key()!="enabled" && item.key()!="allow_pause")throw std::runtime_error("Unsupported rtc.congestion_control key: "+item.key());
        if(!item.value().is_boolean())throw std::runtime_error("rtc.congestion_control values must be boolean");
      }
      c.congestion_control=cc.value("enabled",c.congestion_control);
      c.allow_video_pause=cc.value("allow_pause",c.allow_video_pause);
    }
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
  if (complete) c.validate(); return c;
}
void Config::validate() const {
  if (media_mode != "sfu" && media_mode != "relay") throw std::runtime_error("media_mode must be sfu or relay");
  if (keys.empty()) throw std::runtime_error("Configure API keys or use --dev");
  for (const auto& [key, secret] : keys) if (key.empty() || secret.empty()) throw std::runtime_error("API key and secret must not be empty");
  if (turn_rest) turn_rest->validate();
}
}
