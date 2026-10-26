#include "lk/auth.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <array>
#include <algorithm>
namespace lk {
namespace {
std::string encode(const unsigned char* data, size_t size) {
  std::string out(4 * ((size + 2) / 3), '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(size));
  while (!out.empty() && out.back() == '=') out.pop_back();
  std::replace(out.begin(), out.end(), '+', '-'); std::replace(out.begin(), out.end(), '/', '_');
  return out;
}
std::string encode(const std::string& s) { return encode(reinterpret_cast<const unsigned char*>(s.data()), s.size()); }
std::string decode(std::string s) {
  if (s.empty() || s.size() % 4 == 1 || s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos)
    throw std::runtime_error("Invalid base64url");
  std::replace(s.begin(), s.end(), '-', '+'); std::replace(s.begin(), s.end(), '_', '/');
  size_t pad = (4 - s.size() % 4) % 4;
  s.append(pad, '=');
  std::string result(s.size() / 4 * 3, '\0');
  int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(result.data()), reinterpret_cast<const unsigned char*>(s.data()), static_cast<int>(s.size()));
  if (n < 0) throw std::runtime_error("Invalid base64url");
  result.resize(static_cast<size_t>(n) - pad); return result;
}
std::string mac(const std::string& data, const std::string& secret) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> out{}; unsigned int size = 0;
  if (!HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()), reinterpret_cast<const unsigned char*>(data.data()), data.size(), out.data(), &size))
    throw std::runtime_error("HMAC failed");
  return {reinterpret_cast<const char*>(out.data()), size};
}
}
std::string random_id(const std::string& prefix) {
  std::array<unsigned char, 12> data{};
  if (RAND_bytes(data.data(), static_cast<int>(data.size())) != 1) throw std::runtime_error("Random generator failed");
  return prefix + encode(data.data(), data.size());
}
std::string Auth::sign(const Json& claims, const std::string& secret) {
  auto content = encode(std::string("{\"alg\":\"HS256\",\"typ\":\"JWT\"}")) + "." + encode(claims.dump());
  return content + "." + encode(mac(content, secret));
}
Claims Auth::verify(const std::string& token, int64_t now) const {
  try {
    if (token.size() > 32768) throw std::runtime_error("Token too large");
    auto a = token.find('.'), b = token.find('.', a == std::string::npos ? 0 : a + 1);
    if (a == std::string::npos || b == std::string::npos || token.find('.', b + 1) != std::string::npos) throw std::runtime_error("Invalid token");
    auto header = Json::parse(decode(token.substr(0, a)));
    if (header.value("alg", "") != "HS256" || header.contains("crit")) throw std::runtime_error("Unsupported JWT header");
    auto claims = Json::parse(decode(token.substr(a + 1, b - a - 1)));
    auto key = keys_.find(claims.value("iss", ""));
    if (key == keys_.end()) throw std::runtime_error("Unknown issuer");
    auto expected = mac(token.substr(0, b), key->second), actual = decode(token.substr(b + 1));
    if (expected.size() != actual.size() || CRYPTO_memcmp(expected.data(), actual.data(), expected.size()) != 0) throw std::runtime_error("Invalid signature");
    if (claims.contains("exp") && (!claims["exp"].is_number_integer() || now >= claims["exp"].get<int64_t>())) throw std::runtime_error("Token expired");
    if (claims.contains("nbf") && (!claims["nbf"].is_number_integer() || now < claims["nbf"].get<int64_t>())) throw std::runtime_error("Token not active");
    Claims c;
    c.identity = claims.value("sub", ""); c.name = claims.value("name", ""); c.metadata = claims.value("metadata", "");
    c.attributes = claims.value("attributes", Json::object());
    if (!c.attributes.is_object()) throw std::runtime_error("Invalid attributes");
    for (auto& v : c.attributes) if (!v.is_string()) throw std::runtime_error("Invalid attribute value");
    auto v = claims.value("video", Json::object());
    c.room = v.value("room", ""); c.join = v.value("roomJoin", false); c.create = v.value("roomCreate", false);
    c.list = v.value("roomList", false); c.admin = v.value("roomAdmin", false);
    c.publish = v.value("canPublish", true); c.subscribe = v.value("canSubscribe", true);
    c.publish_data = v.value("canPublishData", true); c.update_metadata = v.value("canUpdateOwnMetadata", false);
    for (const auto& source : v.value("canPublishSources", Json::array())) {
      if (source.is_number_integer()) c.publish_sources.push_back(source.get<int>());
      else if (source == "camera") c.publish_sources.push_back(1);
      else if (source == "microphone") c.publish_sources.push_back(2);
      else if (source == "screen_share") c.publish_sources.push_back(3);
      else if (source == "screen_share_audio") c.publish_sources.push_back(4);
      else throw std::runtime_error("Invalid publish source");
    }
    if (v.value("hidden", false)) throw std::runtime_error("Hidden participants are not supported");
    return c;
  } catch (const std::exception&) { throw Error(401, "unauthenticated", "Invalid or expired access token"); }
}
}
