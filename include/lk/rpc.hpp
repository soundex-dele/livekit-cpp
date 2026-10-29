#pragma once
#include "lk/common.hpp"
#include "livekit_models.pb.h"
#include <boost/asio/io_context.hpp>
#include <functional>
#include <memory>
#include <variant>

namespace lk {
// All operations and callbacks run on the room io_context. The cancellation
// handle is safe after completion or destruction and does not deliver a result.
class RpcRequests {
 public:
  using Result = std::variant<std::string, Error>;
  using Complete = std::function<void(Result)>;
  using Cancel = std::function<void()>;
  using Send = std::function<bool(const livekit::DataPacket&)>;

  struct Timing {
    uint32_t ack_ms = 7000;
    uint32_t default_response_ms = 15000;
  };

  explicit RpcRequests(boost::asio::io_context&);
  RpcRequests(boost::asio::io_context&, Timing);
  ~RpcRequests();
  RpcRequests(const RpcRequests&) = delete;
  RpcRequests& operator=(const RpcRequests&) = delete;
  Cancel start(const std::string& participant_sid, const std::string& method,
               const std::string& payload, uint32_t timeout_ms, uint32_t rtt_ms, Send, Complete);
  bool receive(const std::string& participant_sid, const livekit::DataPacket&);
  void disconnected(const std::string& participant_sid);
  void shutdown();
  size_t size() const;

 private:
  struct State;
  std::shared_ptr<State> state_;
};
}  // namespace lk
