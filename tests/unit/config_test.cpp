#include "lk/config.hpp"
#include <iostream>
#include <functional>

namespace {
void check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void rejects(const std::function<void()>& fn,const char* message) {
  try { fn(); } catch (const std::exception&) { return; }
  throw std::runtime_error(message);
}
}
int main() {
  try {
    const auto yaml=lk::Config::parse("port: 7990\nbind_addresses: [127.0.0.1]\nkeys: {test: '0123'}\nroom: {enable_remote_unmute: true}\nrtc: {port_range_start: 61000, port_range_end: 61010}\n");
    check(yaml.port==7990 && yaml.bind=="127.0.0.1" && yaml.keys.at("test")=="0123" && yaml.enable_remote_unmute && yaml.rtc_port_min==61000,"YAML fields were not applied");
    const auto json=lk::Config::parse(R"({"port":7990,"bind":"127.0.0.1","keys":{"test":"0123"},"room":{"enable_remote_unmute":true},"rtc":{"port_range_start":61000,"port_range_end":61010}})");
    check(json.port==yaml.port && json.keys==yaml.keys && json.enable_remote_unmute==yaml.enable_remote_unmute && json.rtc_port_max==yaml.rtc_port_max,"JSON compatibility changed");
    const auto anchors=lk::Config::parse("keys: {a: &secret abcdefg, b: *secret}");
    check(anchors.keys.at("a")==anchors.keys.at("b"),"YAML scalar aliases not resolved");
    check(lk::Config::parse("keys: {numeric: 1234}").keys.at("numeric")=="1234","Typed YAML key map must preserve scalar strings");
    const auto keys=lk::Config::parse_keys("quoted: '1234'\nnumber: 1234\nboolean: false\ntext: abcdefg");
    check(keys.size()==2 && keys.at("quoted")=="1234" && keys.at("text")=="abcdefg","CLI keys must retain only string secrets like Go");
    check(lk::Config::parse_keys("word: on\nkeys: 1234")==std::map<std::string,std::string>{{"word","on"}},"CLI key names and YAML 1.1 words must not alter secret types");
    check(lk::Config::parse("keys: {x: value}\nroom: {enable_remote_unmute: on}").enable_remote_unmute,"Typed Go YAML bool compatibility missing");
    check(lk::Config::parse("port: 7990",false).keys.empty(),"Partial configuration must allow later key overrides");
    for (const auto& text:{"keys: {}", "[]", "keys: {x: value}\nroom: {enable_remote_unmute: 'true'}",
         "keys: {x: value}\nport: 7990\nport: 7991", "keys: {x: value, x: other}",
         "keys: {x: value}\nrtc: {unsupported: 1}", "keys: {x: value}\nbind_addresses: [127.0.0.1, '::1']",
         "keys: {x: value}\nport: .nan", "keys: &cycle {x: *cycle}", "keys: {x: value}\n---\nkeys: {x: other}"})
      rejects([&]{lk::Config::parse(text);},"Invalid/unsupported configuration was accepted");
    bool malformed_rejected=false;
    try { lk::Config::parse("keys: {x: secret-that-must-not-appear\n"); }
    catch (const std::exception& error) { malformed_rejected=true;check(std::string(error.what()).find("secret-that-must-not-appear")==std::string::npos,"Parser error disclosed input secret"); }
    check(malformed_rejected,"Malformed YAML accepted");
    std::cout<<"PASS YAML/JSON fields, aliases, key typing, strict parsing and bounded expansion\n";
    return 0;
  } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
