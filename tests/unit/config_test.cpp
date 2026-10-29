#include "lk/config.hpp"
#include <iostream>
#include <functional>

namespace {
void check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

void rejects(const std::function<void()>& fn, const char* message) {
  try {
    fn();
  } catch (const std::exception&) {
    return;
  }
  throw std::runtime_error(message);
}
}  // namespace

int main() {
  try {
    const auto yaml = lk::Config::parse(
        "port: 7990\nbind_addresses: [127.0.0.1]\nkeys: {test: '0123'}\nroom: "
        "{enable_remote_unmute: true}\nrtc: {port_range_start: 61000, port_range_end: 61010}\n");
    check(yaml.port == 7990 && yaml.bind == "127.0.0.1" && yaml.keys.at("test") == "0123" &&
              yaml.enable_remote_unmute && yaml.rtc_port_min == 61000,
          "YAML fields were not applied");
    const auto json = lk::Config::parse(
        R"({"port":7990,"bind":"127.0.0.1","keys":{"test":"0123"},"room":{"enable_remote_unmute":true},"rtc":{"port_range_start":61000,"port_range_end":61010}})");
    check(json.port == yaml.port && json.keys == yaml.keys &&
              json.enable_remote_unmute == yaml.enable_remote_unmute &&
              json.rtc_port_max == yaml.rtc_port_max,
          "JSON compatibility changed");
    check(yaml.congestion_control && !yaml.allow_video_pause,
          "Go congestion-control defaults differ");
    const auto cc = lk::Config::parse(
        "keys: {x: value}\nrtc: {congestion_control: {enabled: false, allow_pause: true}}");
    check(!cc.congestion_control && cc.allow_video_pause, "Congestion-control flags not applied");
    rejects(
        [] { lk::Config::parse("keys: {x: value}\nrtc: {congestion_control: {allow_pause: 1}}"); },
        "Non-boolean congestion-control flag accepted");
    const auto anchors = lk::Config::parse("keys: {a: &secret abcdefg, b: *secret}");
    check(anchors.keys.at("a") == anchors.keys.at("b"), "YAML scalar aliases not resolved");
    check(lk::Config::parse("keys: {numeric: 1234}").keys.at("numeric") == "1234",
          "Typed YAML key map must preserve scalar strings");
    const auto keys =
        lk::Config::parse_keys("quoted: '1234'\nnumber: 1234\nboolean: false\ntext: abcdefg");
    check(keys.size() == 2 && keys.at("quoted") == "1234" && keys.at("text") == "abcdefg",
          "CLI keys must retain only string secrets like Go");
    check(lk::Config::parse_keys("word: on\nkeys: 1234") ==
              std::map<std::string, std::string>{{"word", "on"}},
          "CLI key names and YAML 1.1 words must not alter secret types");
    check(lk::Config::parse("keys: {x: value}\nroom: {enable_remote_unmute: on}")
              .enable_remote_unmute,
          "Typed Go YAML bool compatibility missing");
    check(lk::Config::parse("port: 7990", false).keys.empty(),
          "Partial configuration must allow later key overrides");
    const auto limited = lk::Config::parse(
        "keys: {x: value}\nlimit: {max_room_name_length: 12, max_participant_identity_length: 16, "
        "max_participant_name_length: 8, max_metadata_size: 32, max_attributes_size: 16}");
    check(limited.limits.max_room_name_length == 12 &&
              limited.limits.max_participant_identity_length == 16 &&
              limited.limits.max_participant_name_length == 8 &&
              limited.limits.max_metadata_size == 32 && limited.limits.max_attributes_size == 16,
          "Configured limits were not applied");
    const auto legacy = lk::Config::parse(
        "keys: {x: value}\nlimit: {max_room_name_length: 12, max_participant_identity_length: 16, "
        "max_metadata_size: 32}\nroom: {max_room_name_length: 6, max_participant_identity_length: "
        "9, max_metadata_size: 0}");
    check(legacy.limits.max_room_name_length == 6 &&
              legacy.limits.max_participant_identity_length == 9 &&
              legacy.limits.max_metadata_size == 32,
          "Nonzero legacy room limits did not override new limits");
    const auto zero = lk::Config::parse(
        "keys: {x: value}\nlimit: {max_room_name_length: 0, max_metadata_size: 0, "
        "max_attributes_size: null, max_participant_name_length: -1}");
    check(!lk::Limits::exceeded(1000000, zero.limits.max_room_name_length) &&
              !lk::Limits::exceeded(1000000, zero.limits.max_metadata_size) &&
              lk::Limits::exceeded(0, zero.limits.max_participant_name_length) &&
              zero.limits.max_attributes_size == 65536 && !lk::Limits::exceeded(16, 16) &&
              lk::Limits::exceeded(17, 16),
          "Zero, negative, null or inclusive limit semantics changed");
    for (const auto& fragment :
         {"max_metadata_size: -1", "max_metadata_size: 4294967296",
          "max_attributes_size: 4294967296", "max_room_name_length: 9223372036854775808",
          "max_metadata_size: true", "max_metadata_size: '32'", "max_metadata_size: [32]",
          "subscription_limit_audio: 3", "max_metdata_size: 3"})
      rejects(
          [&] { lk::Config::parse(std::string("keys: {x: value}\nlimit: {") + fragment + "}"); },
          "Invalid or unsupported limit was ignored");
    for (const auto& text :
         {"keys: {}", "[]", "keys: {x: value}\nroom: {enable_remote_unmute: 'true'}",
          "keys: {x: value}\nport: 7990\nport: 7991", "keys: {x: value, x: other}",
          "keys: {x: value}\nrtc: {unsupported: 1}",
          "keys: {x: value}\nbind_addresses: [127.0.0.1, '::1']", "keys: {x: value}\nport: .nan",
          "keys: &cycle {x: *cycle}", "keys: {x: value}\n---\nkeys: {x: other}"})
      rejects([&] { lk::Config::parse(text); }, "Invalid/unsupported configuration was accepted");
    bool malformed_rejected = false;
    try {
      lk::Config::parse("keys: {x: secret-that-must-not-appear\n");
    } catch (const std::exception& error) {
      malformed_rejected = true;
      check(std::string(error.what()).find("secret-that-must-not-appear") == std::string::npos,
            "Parser error disclosed input secret");
    }
    check(malformed_rejected, "Malformed YAML accepted");
    std::cout
        << "PASS YAML/JSON fields, aliases, key typing, strict parsing and bounded expansion\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
