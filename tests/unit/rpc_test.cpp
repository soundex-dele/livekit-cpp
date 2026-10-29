#include "lk/rpc.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}

void check_error(const lk::RpcRequests::Result& result, int status, const std::string& code,
                 const std::string& text) {
  const auto* error = std::get_if<lk::Error>(&result);
  if (!error || error->status != status || error->code != code ||
      std::string(error->what()) != text)
    throw std::runtime_error("Expected " + text + "; received " +
                             (error ? error->what() : "success"));
}
}  // namespace

int main() {
  try {
    boost::asio::io_context io;
    lk::RpcRequests rpc(io, {30, 80});
    std::vector<lk::RpcRequests::Result> results;
    livekit::DataPacket sent;
    auto send = [&](const livekit::DataPacket& packet) {
      sent = packet;
      return true;
    };
    auto done = [&](lk::RpcRequests::Result result) { results.push_back(std::move(result)); };
    auto pump = [&] {
      // Every scenario completes or cancels all requests. Drain the actual
      // asynchronous timers (including IOCP cancellation completions) instead
      // of assuming a short run_for budget has dispatched an expired timer.
      // CTest supplies the outer hang timeout; ACK/response deadlines are unchanged.
      io.restart();
      io.run();
    };
    auto cancel = rpc.start("PA_target", "echo", "hello", 100, 10, send, done);
    check(sent.rpc_request().id().size() == 36 &&
              sent.participant_identity() == sent.rpc_request().id(),
          "Server RPC identity contract mismatch");
    check(sent.rpc_request().response_timeout_ms() == 90 && sent.rpc_request().version() == 1,
          "Timeout/RTT or version not sent");
    livekit::DataPacket response;
    response.mutable_rpc_response()->set_request_id(sent.rpc_request().id());
    response.mutable_rpc_response()->set_payload("");
    check(!rpc.receive("PA_attacker", response) && rpc.size() == 1,
          "Another participant forged an RPC response");
    check(rpc.receive("PA_target", response) && rpc.size() == 0,
          "Response before ACK did not complete");
    check(std::get<std::string>(results.back()).empty(),
          "Empty response payload lost oneof presence");
    check(!rpc.receive("PA_target", response), "Duplicate response completed twice");
    cancel();
    pump();
    check(results.size() == 1, "Completed RPC still fired a timer");

    rpc.start("PA_target", "slow", "", 100, 0, send, done);
    pump();
    check_error(results.back(), 408, "canceled", "RpcError 1501: Connection timeout");
    check(rpc.size() == 0, "ACK timeout retained request");
    rpc.start("PA_target", "slow", "", 55, 0, send, done);
    livekit::DataPacket ack;
    ack.mutable_rpc_ack()->set_request_id(sent.rpc_request().id());
    check(!rpc.receive("PA_attacker", ack), "Another participant forged an ACK");
    check(rpc.receive("PA_target", ack), "ACK not matched");
    pump();
    check_error(results.back(), 408, "canceled", "RpcError 1502: Response timeout");

    auto first = rpc.start("PA_old", "echo", "", 0, 0, send, done);
    auto second = rpc.start("PA_new", "echo", "", 0, 0, send, done);
    rpc.disconnected("PA_old");
    check(rpc.size() == 1, "Participant departure canceled another session");
    check_error(results.back(), 503, "unavailable", "RpcError 1503: Recipient disconnected");
    const auto count = results.size();
    first();
    second();
    pump();
    check(results.size() == count && rpc.size() == 0,
          "Cancellation delivered a result or leaked state");

    rpc.start("PA_target", "error", "", 0, 0, send, done);
    response.mutable_rpc_response()->set_request_id(sent.rpc_request().id());
    auto* error = response.mutable_rpc_response()->mutable_error();
    error->set_code(2001);
    error->set_message("application rejection");
    error->set_data("detail");
    rpc.receive("PA_target", response);
    check_error(results.back(), 500, "internal", "RpcError 2001: application rejection: detail");
    bool rejected = false;
    try {
      rpc.start("PA_target", "large", std::string(15361, 'x'), 0, 0, send, done);
    } catch (const lk::Error& e) {
      rejected = e.status == 400;
    }
    check(rejected && rpc.size() == 0, "Oversized payload was sent or retained");
    rejected = false;
    try {
      rpc.start("PA_target", "closed", "", 0, 0, [](const auto&) { return false; }, done);
    } catch (const lk::Error& e) {
      rejected = e.status == 500;
    }
    check(rejected && rpc.size() == 0, "Send failure retained timers or request");
    cancel = rpc.start("PA_target", "boundary", std::string(15360, 'x'), 20, 30, send, done);
    check(sent.rpc_request().response_timeout_ms() == uint32_t(20u - 30u),
          "RTT field differs from Go uint32 wire contract");
    pump();
    check_error(results.back(), 408, "canceled", "RpcError 1502: Response timeout");
    check(rpc.size() == 0, "Wrapped client timeout extended the server deadline");
    cancel = rpc.start("PA_target", "shutdown", "", 0, 0, send, done);
    rpc.shutdown();
    check_error(results.back(), 503, "unavailable", "RpcError 1503: Recipient disconnected");
    cancel();
    pump();
    check(rpc.size() == 0, "Shutdown retained requests");
    lk::RpcRequests::Cancel late;
    {
      lk::RpcRequests temporary(io);
      late = temporary.start("PA_target", "destroy", "", 0, 0, send, done);
    }
    late();
    pump();
    std::cout << "PASS: RPC success, sender binding, ACK/response deadlines, cancellation, "
                 "disconnect and destruction\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
