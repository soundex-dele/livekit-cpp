#include "lk/config.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#if defined(_WIN32)
#include <rtc_base/win32_socket_init.h>
#endif

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void invalid(F operation) {
  try { operation(); } catch (const std::exception&) { return; }
  throw std::runtime_error("Invalid TURN configuration/issue request accepted");
}
struct ConfigFile {
  std::filesystem::path path = std::filesystem::temp_directory_path() / (lk::random_id("lk-turn-") + ".json");
  ~ConfigFile() { std::error_code error; std::filesystem::remove(path, error); }
  lk::Config load(const lk::Json& value) {
    { std::ofstream file(path); file << value.dump(); }
    return lk::Config::load(path.string());
  }
};
struct SecretEnv {
  const char* name = "LK_TEST_TURN_SECRET";
  std::optional<std::string> previous;
  SecretEnv() { if (const auto* value = std::getenv(name)) previous = value; }
  void set(const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
  }
  ~SecretEnv() { set(previous ? previous->c_str() : nullptr); }
};
}

int main() {
  try {
#if defined(_WIN32)
    webrtc::WinsockInitializer winsock;
#endif
    const std::string secret = "0123456789abcdef0123456789abcdef";
    lk::TurnRestAuth auth{{"turn:127.0.0.1:3478?transport=udp", "turns:turn.example.org:5349?transport=tcp"}, secret, 3600};
    const auto issued = auth.issue("PA_test", 1700000000);
    check(issued.username == "1700003600:PA_test", "TURN expiry/session username is incorrect");
    // Independent Python hmac/hashlib/base64 vector; includes '+' and '=' to
    // catch accidental reuse of JWT base64url encoding.
    check(issued.credential == "TrVzWtji+bcpBhYygeUBdwaQvCs=", "coturn REST HMAC vector differs");
    check(issued.urls == auth.urls, "TURN URL list changed while signing");
    check(auth.issue("PA_other",1700000000).credential != issued.credential, "Sessions shared credentials");
    check(auth.issue("PA_test",1700000001).credential != issued.credential, "Issuance expiry did not affect credentials");
    auto rotated = auth; rotated.shared_secret = std::string(32,'z');
    check(rotated.issue("PA_test",1700000000).credential != issued.credential, "Secret rotation did not affect credentials");
    invalid([&]{auth.issue("name:injection",1700000000);});
    invalid([&]{auth.issue("",1700000000);});
    invalid([&]{auth.issue("PA_test",-1);});
    invalid([&]{auth.issue("PA_test",std::numeric_limits<int64_t>::max());});
    for (const auto* url : {"stun:example.org", "turn:user@example.org", "turn:example.org:70000", "turn:example.org?transport=sctp"}) {
      auto bad = auth; bad.urls = {url}; invalid([&]{bad.validate();});
    }
    for (int ttl : {0,59,604801}) { auto bad = auth; bad.credential_ttl_seconds=ttl; invalid([&]{bad.validate();}); }
    auto bad = auth; bad.shared_secret = "short"; invalid([&]{bad.validate();});
    bad = auth; bad.urls.clear(); invalid([&]{bad.validate();});

    ConfigFile file;
    lk::Json json = {{"keys",{{"key","api-secret"}}}, {"rtc",{{"ice_servers",lk::Json::array({{{"urls",lk::Json::array({"stun:example.org"})}}})},
      {"turn_rest",{{"urls",auth.urls},{"shared_secret",secret},{"credential_ttl_seconds",3600}}}}}};
    auto config = file.load(json);
    const auto servers = config.session_ice_servers("PA_test",1700000000);
    check(servers.size()==2 && servers[0].urls[0]=="stun:example.org" && servers[1].credential==issued.credential,
          "Static ICE configuration was not preserved alongside dynamic TURN");
    for (const auto& ttl : {lk::Json(59),lk::Json(604801),lk::Json(4294967356LL),lk::Json(3600.5),lk::Json(true)}) {
      auto value=json; value["rtc"]["turn_rest"]["credential_ttl_seconds"]=ttl; invalid([&]{file.load(value);});
    }
    auto value=json; value["rtc"]["turn_rest"]["shared_secert"]=secret; invalid([&]{file.load(value);});
    value=json; value["rtc"]["ice_server"]=lk::Json::array(); invalid([&]{file.load(value);});
    SecretEnv environment;
    value=json; value["rtc"]["turn_rest"].erase("shared_secret");
    value["rtc"]["turn_rest"]["shared_secret_env"]=environment.name;
    environment.set(secret.c_str());
    check(file.load(value).session_ice_servers("PA_test",1700000000)[1].credential==issued.credential, "Environment secret not used");
    environment.set(nullptr); invalid([&]{file.load(value);});
    value["rtc"]["turn_rest"]["shared_secret"]=secret; invalid([&]{file.load(value);});
    json["rtc"].erase("turn_rest");
    check(file.load(json).session_ice_servers("PA_test").size()==1, "Disabled TURN REST changed static ICE configuration");
    std::cout << "PASS: coturn HMAC vector, expiry, session isolation, rotation, native URI validation and config errors\n";
  } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
