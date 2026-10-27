#include "lk/join_request.hpp"
#include <openssl/evp.h>
#include <zlib.h>
#include <array>

namespace lk {
livekit::JoinRequest decode_join_request(const std::string& encoded) {
  constexpr size_t limit = 1024 * 1024;
  auto reject = [](const char* message) { throw Error(400, "invalid_argument", message); };
  if (encoded.size() > 2 * limit) reject("join request too large");
  std::string base64;
  for (char c : encoded) {
    if (c == '\r' || c == '\n') continue;
    if (c == '-') c = '+';
    else if (c == '_') c = '/';
    else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '=')) reject("cannot base64 decode wrapped join request");
    base64 += c;
  }
  if (base64.empty() || base64.size() % 4) reject("cannot base64 decode wrapped join request");
  const auto padding = base64.ends_with("==") ? 2 : base64.ends_with("=") ? 1 : 0;
  if (base64.substr(0, base64.size() - padding).find('=') != std::string::npos)
    reject("cannot base64 decode wrapped join request");
  std::string bytes(base64.size() / 4 * 3, '\0');
  int length = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(bytes.data()),
      reinterpret_cast<const unsigned char*>(base64.data()), static_cast<int>(base64.size()));
  if (length < padding) reject("cannot base64 decode wrapped join request");
  bytes.resize(length - padding);
  livekit::WrappedJoinRequest wrapped;
  if (!wrapped.ParseFromString(bytes)) reject("cannot unmarshal wrapped join request");
  std::string payload;
  if (wrapped.compression() == livekit::WrappedJoinRequest::NONE) payload = wrapped.join_request();
  else if (wrapped.compression() == livekit::WrappedJoinRequest::GZIP) {
    z_stream stream{};
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) throw std::runtime_error("Cannot initialize gzip decoder");
    struct Cleanup { z_stream* stream; ~Cleanup() { inflateEnd(stream); } } cleanup{&stream};
    stream.next_in = reinterpret_cast<Bytef*>(wrapped.mutable_join_request()->data());
    stream.avail_in = static_cast<uInt>(wrapped.join_request().size());
    std::array<char, 16384> buffer;
    for (;;) {
      stream.next_out = reinterpret_cast<Bytef*>(buffer.data()); stream.avail_out = static_cast<uInt>(buffer.size());
      int status = inflate(&stream, Z_NO_FLUSH);
      payload.append(buffer.data(), buffer.size() - stream.avail_out);
      if (payload.size() > limit) reject("join request too large");
      if (status == Z_STREAM_END) {
        if (!stream.avail_in) break;
        // Go gzip.Reader accepts concatenated members and validates each CRC.
        if (inflateReset(&stream) != Z_OK) reject("cannot read decompressed join request");
      } else if (status != Z_OK) reject("cannot read decompressed join request");
    }
  }
  // Pinned Go ignores unknown compression enums and proceeds with an empty join.
  if (payload.size() > limit) reject("join request too large");
  livekit::JoinRequest join;
  if (!join.ParseFromString(payload)) reject("cannot unmarshal join request");
  return join;
}
}
