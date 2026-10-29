#include "lk/config.hpp"
#include <pc/ice_server_parsing.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <array>
#include <limits>

namespace lk {
void TurnRestAuth::validate() const {
  if (shared_secret.size() < 32 || shared_secret.size() > 1024)
    throw std::runtime_error("rtc.turn_rest shared secret must contain 32..1024 bytes");
  if (credential_ttl_seconds < 60 || credential_ttl_seconds > 604800)
    throw std::runtime_error("rtc.turn_rest credential_ttl_seconds must be 60..604800");
  if (urls.empty() || urls.size() > 8)
    throw std::runtime_error("rtc.turn_rest urls must contain 1..8 TURN URLs");
  for (const auto& url : urls) {
    if (url.size() > 2048 || (!url.starts_with("turn:") && !url.starts_with("turns:")) ||
        url.find_first_of("\r\n\t @") != std::string::npos)
      throw std::runtime_error(
          "rtc.turn_rest requires turn: or turns: URLs without embedded credentials");
  }
  // Reuse native WebRTC URI/transport validation rather than a second parser.
  webrtc::PeerConnectionInterface::IceServer server;
  server.urls = urls;
  server.username = "validation";
  server.password = "validation";
  webrtc::ServerAddresses stun;
  std::vector<webrtc::RelayServerConfig> turn;
  if (!webrtc::ParseIceServersOrError({server}, &stun, &turn).ok() || !stun.empty() || turn.empty())
    throw std::runtime_error("Invalid rtc.turn_rest TURN URL configuration");
}

IceServer TurnRestAuth::issue(const std::string& session_id, int64_t now) const {
  validate();
  if (session_id.empty() || session_id.size() > 128 ||
      session_id.find_first_not_of(
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
    throw std::runtime_error("Invalid TURN session identifier");
  if (now < 0 || now > std::numeric_limits<int64_t>::max() - credential_ttl_seconds)
    throw std::runtime_error("Invalid TURN credential issue time");
  IceServer result{urls, std::to_string(now + credential_ttl_seconds) + ":" + session_id, {}};
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  // coturn REST authentication specifies Base64(HMAC-SHA1(secret, username)).
  // SHA-1 here is protocol-required HMAC, not a replacement for WebRTC crypto.
  if (!HMAC(EVP_sha1(), shared_secret.data(), static_cast<int>(shared_secret.size()),
            reinterpret_cast<const unsigned char*>(result.username.data()), result.username.size(),
            digest.data(), &size))
    throw std::runtime_error("TURN credential signing failed");
  std::array<unsigned char, 4 * ((EVP_MAX_MD_SIZE + 2) / 3) + 1> encoded{};
  int length = EVP_EncodeBlock(encoded.data(), digest.data(), static_cast<int>(size));
  if (length < 0)
    throw std::runtime_error("TURN credential encoding failed");
  result.credential.assign(reinterpret_cast<const char*>(encoded.data()),
                           static_cast<size_t>(length));
  return result;
}

std::vector<IceServer> Config::session_ice_servers(const std::string& session_id,
                                                   int64_t now) const {
  auto result = ice_servers;
  if (turn_rest)
    result.push_back(turn_rest->issue(session_id, now));
  return result;
}
}  // namespace lk
