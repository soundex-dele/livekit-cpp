#include "lk/join_request.hpp"
#include <openssl/evp.h>
#include <zlib.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::string encode(const std::string& bytes) {
  std::string text((bytes.size() + 2) / 3 * 4, '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char*>(text.data()),
      reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
  std::replace(text.begin(), text.end(), '+', '-'); std::replace(text.begin(), text.end(), '/', '_');
  return text;
}
std::string gzip(const std::string& bytes) {
  z_stream stream{};
  check(deflateInit2(&stream, 6, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK, "deflate init");
  std::string result(compressBound(static_cast<uLong>(bytes.size())) + 32, '\0');
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data())); stream.avail_in = static_cast<uInt>(bytes.size());
  stream.next_out = reinterpret_cast<Bytef*>(result.data()); stream.avail_out = static_cast<uInt>(result.size());
  const auto status = deflate(&stream, Z_FINISH); result.resize(stream.total_out); deflateEnd(&stream);
  check(status == Z_STREAM_END, "deflate finish"); return result;
}
std::string wrapped(const std::string& bytes, bool compressed) {
  livekit::WrappedJoinRequest request;
  request.set_compression(compressed ? livekit::WrappedJoinRequest::GZIP : livekit::WrappedJoinRequest::NONE);
  request.set_join_request(bytes); return encode(request.SerializeAsString());
}
void rejected(const std::string& encoded) {
  try { lk::decode_join_request(encoded); }
  catch (const lk::Error& error) { check(error.status == 400, "wrong rejection status"); return; }
  throw std::runtime_error("malformed or oversized join accepted");
}
}
int main() {
  try {
    auto info = lk::parse_client_info({{"sdk", "rust"}, {"protocol", "+15"}, {"client_protocol", "2"}, {"version", "0.1"}});
    check(info.sdk() == livekit::ClientInfo::RUST && info.protocol() == 15 && info.client_protocol() == 2 && info.version() == "0.1", "legacy client info not parsed");
    for (const auto* text : {"", "15x", " 15", "2147483648", "+-15", "+"})
      check(lk::parse_client_info({{"protocol", text}}).protocol() == 0, "invalid protocol did not use zero default");
    check(lk::parse_client_info({{"sdk", "Rust"}, {"protocol", "-1"}}).sdk() == livekit::ClientInfo::UNKNOWN &&
          lk::parse_client_info({{"protocol", "-1"}}).protocol() == -1, "client info normalization differs from Go");
    check(lk::parse_client_info({{"sdk", "ios"}}).sdk() == livekit::ClientInfo::SWIFT, "legacy SDK alias missing");
    livekit::JoinRequest join; join.set_metadata("metadata"); join.mutable_client_info()->set_protocol(18);
    (*join.mutable_participant_attributes())["a"] = "b";
    auto bytes = join.SerializeAsString();
    for (bool compressed : {false, true}) {
      auto decoded = lk::decode_join_request(wrapped(compressed ? gzip(bytes) : bytes, compressed));
      check(decoded.SerializeAsString() == bytes, "join round trip");
    }
    check(lk::decode_join_request(wrapped(gzip(bytes.substr(0, 3)) + gzip(bytes.substr(3)), true)).metadata() == "metadata", "gzip multistream");
    rejected("a"); rejected("!==="); rejected("AA=A"); rejected(encode("\xff"));
    rejected(wrapped("\xff", false)); rejected(wrapped("not gzip", true));
    auto damaged = gzip(bytes); damaged[damaged.size()-8] ^= 1;
    rejected(wrapped(damaged, true)); rejected(wrapped(gzip(bytes).substr(0, 12), true));
    join.set_metadata(std::string(1024 * 1024, 'a')); bytes = join.SerializeAsString();
    rejected(wrapped(bytes, false)); rejected(wrapped(gzip(bytes), true));
    // Exact 1 MiB protobuf boundary, not a limit on compressed bytes.
    join.Clear(); join.set_metadata(std::string(1024 * 1024 - 4, 'a')); bytes = join.SerializeAsString();
    check(bytes.size() == 1024 * 1024, "boundary fixture");
    check(lk::decode_join_request(wrapped(gzip(bytes), true)).metadata() == join.metadata(), "exact limit rejected");
    std::cout << "join request bounds, gzip CRC and concatenated members passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
