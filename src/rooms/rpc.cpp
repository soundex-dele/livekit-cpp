#include "lk/rpc.hpp"
#include <boost/asio/steady_timer.hpp>
#include <openssl/rand.h>
#include <array>
#include <map>
#include <optional>
#include <vector>

namespace lk {
namespace {
std::string uuid() {
  std::array<unsigned char, 16> bytes;
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    throw std::runtime_error("Cannot generate RPC request ID");
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  constexpr char hex[] = "0123456789abcdef";
  std::string id;
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) id += '-';
    id += hex[bytes[i] >> 4]; id += hex[bytes[i] & 0xf];
  }
  return id;
}
Error rpc_error(uint32_t code, const std::string& message, const std::string& data = {}) {
  // Mirrors protocol/utils.DataChannelRpcError -> PSRPC -> Twirp.
  int status = 500;
  std::string type = "internal";
  switch (code) {
    case 1400: case 1402: status = 400; type = "invalid_argument"; break;
    case 1401: status = 404; type = "not_found"; break;
    case 1403: case 1404: status = 501; type = "unimplemented"; break;
    case 1501: case 1502: status = 408; type = "canceled"; break;
    case 1503: status = 503; type = "unavailable"; break;
    default: break;
  }
  return Error(status, type, "RpcError " + std::to_string(code) + ": " + message +
               (data.empty() ? "" : ": " + data));
}
}

struct RpcRequests::State : std::enable_shared_from_this<State> {
  struct Pending {
    std::string participant;
    Complete complete;
    boost::asio::steady_timer ack, response;
    Pending(boost::asio::io_context& io, std::string target, Complete done)
        : participant(std::move(target)), complete(std::move(done)), ack(io), response(io) {}
  };
  boost::asio::io_context& io;
  Timing timing;
  std::map<std::string, std::shared_ptr<Pending>> pending;
  bool stopped = false;
  State(boost::asio::io_context& context, Timing times) : io(context), timing(times) {}

  void finish(const std::string& id, std::optional<Result> result) {
    auto it = pending.find(id);
    if (it == pending.end()) return;
    auto entry = std::move(it->second);
    pending.erase(it);
    entry->ack.cancel(); entry->response.cancel();
    if (result) entry->complete(std::move(*result));
  }
  void expire(boost::asio::steady_timer& timer, uint32_t ms, const std::string& id, Error error) {
    timer.expires_after(std::chrono::milliseconds(ms));
    timer.async_wait([weak = weak_from_this(), id, error](boost::system::error_code ec) {
      if (!ec) if (auto state = weak.lock()) state->finish(id, error);
    });
  }
};

RpcRequests::RpcRequests(boost::asio::io_context& io) : RpcRequests(io, Timing{}) {}
RpcRequests::RpcRequests(boost::asio::io_context& io, Timing timing)
    : state_(std::make_shared<State>(io, timing)) {}
RpcRequests::~RpcRequests() { shutdown(); }

RpcRequests::Cancel RpcRequests::start(const std::string& participant, const std::string& method,
    const std::string& payload, uint32_t timeout, uint32_t rtt, Send send, Complete complete) {
  if (state_->stopped) throw Error(503, "unavailable", "Server is stopping");
  if (payload.size() > 15360) throw rpc_error(1402, "Request payload too large");
  if (!timeout) timeout = state_->timing.default_response_ms;
  const auto id = uuid();
  auto entry = std::make_shared<State::Pending>(state_->io, participant, std::move(complete));
  state_->pending.emplace(id, entry);
  state_->expire(entry->ack, state_->timing.ack_ms, id, rpc_error(1501, "Connection timeout"));
  state_->expire(entry->response, timeout, id, rpc_error(1502, "Response timeout"));
  livekit::DataPacket packet;
  packet.set_kind(livekit::DataPacket::RELIABLE);
  packet.set_participant_identity(id);
  auto* request = packet.mutable_rpc_request();
  request->set_id(id); request->set_method(method); request->set_payload(payload);
  request->set_version(1);
  // Preserve the pinned Go wire contract (uint32 subtraction). The server's
  // actual response deadline remains `timeout`, including when RTT exceeds it.
  request->set_response_timeout_ms(timeout - rtt);
  try {
    if (!send(packet)) throw Error(500, "internal", "data channel unavailable");
  } catch (...) {
    state_->finish(id, std::nullopt);
    throw;
  }
  return [weak = std::weak_ptr<State>(state_), id] {
    if (auto state = weak.lock()) state->finish(id, std::nullopt);
  };
}

bool RpcRequests::receive(const std::string& participant, const livekit::DataPacket& packet) {
  if (!packet.has_rpc_ack() && !packet.has_rpc_response()) return false;
  const auto& id = packet.has_rpc_ack() ? packet.rpc_ack().request_id() : packet.rpc_response().request_id();
  const auto it = state_->pending.find(id);
  if (it == state_->pending.end() || it->second->participant != participant) return false;
  if (packet.has_rpc_ack()) { it->second->ack.cancel(); return true; }
  const auto& response = packet.rpc_response();
  if (response.has_payload()) state_->finish(id, response.payload());
  else if (response.has_error()) {
    const auto& error = response.error();
    state_->finish(id, rpc_error(error.code(), error.message(), error.data()));
  } else return false; // The pinned Go implementation does not resolve compressed/unknown values.
  return true;
}

void RpcRequests::disconnected(const std::string& participant) {
  std::vector<std::string> ids;
  for (const auto& [id, entry] : state_->pending) if (entry->participant == participant) ids.push_back(id);
  for (const auto& id : ids) state_->finish(id, rpc_error(1503, "Recipient disconnected"));
}
void RpcRequests::shutdown() {
  if (state_->stopped) return;
  state_->stopped = true;
  while (!state_->pending.empty()) {
    const auto id = state_->pending.begin()->first;
    state_->finish(id, rpc_error(1503, "Recipient disconnected"));
  }
}
size_t RpcRequests::size() const { return state_->pending.size(); }
}
