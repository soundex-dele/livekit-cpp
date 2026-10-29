#include "lk/join_request.hpp"
#include <openssl/evp.h>
#include <zlib.h>
#include <array>
#include <charconv>

namespace lk {
livekit::ClientInfo parse_client_info(const std::map<std::string, std::string>& query) {
  auto get = [&](const std::string& name) -> std::string {
    const auto it = query.find(name);
    return it == query.end() ? "" : it->second;
  };
  auto integer = [&](const std::string& name) {
    const auto text = get(name);
    const char* begin = text.data();
    if (!text.empty() && text.front() == '+') {
      ++begin;
      if (begin == text.data() + text.size() || *begin < '0' || *begin > '9')
        return int32_t(0);
    }
    int32_t value = 0;
    auto result = std::from_chars(begin, text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? value : int32_t(0);
  };
  livekit::ClientInfo info;
  info.set_protocol(integer("protocol"));
  info.set_client_protocol(integer("client_protocol"));
  static const std::map<std::string, livekit::ClientInfo::SDK> sdks{
      {"js", livekit::ClientInfo::JS},           {"ios", livekit::ClientInfo::SWIFT},
      {"swift", livekit::ClientInfo::SWIFT},     {"android", livekit::ClientInfo::ANDROID},
      {"flutter", livekit::ClientInfo::FLUTTER}, {"go", livekit::ClientInfo::GO},
      {"unity", livekit::ClientInfo::UNITY},     {"reactnative", livekit::ClientInfo::REACT_NATIVE},
      {"rust", livekit::ClientInfo::RUST},       {"python", livekit::ClientInfo::PYTHON},
      {"cpp", livekit::ClientInfo::CPP},         {"unityweb", livekit::ClientInfo::UNITY_WEB},
      {"node", livekit::ClientInfo::NODE},       {"esp32", livekit::ClientInfo::ESP32}};
  if (const auto it = sdks.find(get("sdk")); it != sdks.end())
    info.set_sdk(it->second);
  info.set_version(get("version"));
  info.set_os(get("os"));
  info.set_os_version(get("os_version"));
  info.set_browser(get("browser"));
  info.set_browser_version(get("browser_version"));
  info.set_device_model(get("device_model"));
  info.set_network(get("network"));
  return info;
}

livekit::JoinRequest decode_join_request(const std::string& encoded) {
  constexpr size_t limit = 1024 * 1024;
  auto reject = [](const char* message) { throw Error(400, "invalid_argument", message); };
  if (encoded.size() > 2 * limit)
    reject("join request too large");
  std::string base64;
  for (char c : encoded) {
    if (c == '\r' || c == '\n')
      continue;
    if (c == '-')
      c = '+';
    else if (c == '_')
      c = '/';
    else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '='))
      reject("cannot base64 decode wrapped join request");
    base64 += c;
  }
  if (base64.empty() || base64.size() % 4)
    reject("cannot base64 decode wrapped join request");
  const auto padding = base64.ends_with("==") ? 2 : base64.ends_with("=") ? 1 : 0;
  if (base64.substr(0, base64.size() - padding).find('=') != std::string::npos)
    reject("cannot base64 decode wrapped join request");
  std::string bytes(base64.size() / 4 * 3, '\0');
  int length = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(bytes.data()),
                               reinterpret_cast<const unsigned char*>(base64.data()),
                               static_cast<int>(base64.size()));
  if (length < padding)
    reject("cannot base64 decode wrapped join request");
  bytes.resize(length - padding);
  livekit::WrappedJoinRequest wrapped;
  if (!wrapped.ParseFromString(bytes))
    reject("cannot unmarshal wrapped join request");
  std::string payload;
  if (wrapped.compression() == livekit::WrappedJoinRequest::NONE)
    payload = wrapped.join_request();
  else if (wrapped.compression() == livekit::WrappedJoinRequest::GZIP) {
    z_stream stream{};
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK)
      throw std::runtime_error("Cannot initialize gzip decoder");

    struct Cleanup {
      z_stream* stream;

      ~Cleanup() {
        inflateEnd(stream);
      }
    } cleanup{&stream};

    stream.next_in = reinterpret_cast<Bytef*>(wrapped.mutable_join_request()->data());
    stream.avail_in = static_cast<uInt>(wrapped.join_request().size());
    std::array<char, 16384> buffer;
    for (;;) {
      stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
      stream.avail_out = static_cast<uInt>(buffer.size());
      int status = inflate(&stream, Z_NO_FLUSH);
      payload.append(buffer.data(), buffer.size() - stream.avail_out);
      if (payload.size() > limit)
        reject("join request too large");
      if (status == Z_STREAM_END) {
        if (!stream.avail_in)
          break;
        // Go gzip.Reader accepts concatenated members and validates each CRC.
        if (inflateReset(&stream) != Z_OK)
          reject("cannot read decompressed join request");
      } else if (status != Z_OK)
        reject("cannot read decompressed join request");
    }
  }
  // Pinned Go ignores unknown compression enums and proceeds with an empty join.
  if (payload.size() > limit)
    reject("join request too large");
  livekit::JoinRequest join;
  if (!join.ParseFromString(payload))
    reject("cannot unmarshal join request");
  return join;
}
}  // namespace lk
