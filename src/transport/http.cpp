#include "lk/http.hpp"
#include <boost/beast.hpp>
#include <google/protobuf/dynamic_message.h>
#include <deque>
#include <iostream>
namespace lk {
namespace {
namespace beast = boost::beast;
namespace http = beast::http;
namespace ws = beast::websocket;
using tcp = boost::asio::ip::tcp;
using Request = http::request<http::string_body>;
std::string url_decode(const std::string& value) {
  auto hex = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; };
  std::string result;
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%') {
      if (i + 2 >= value.size() || hex(value[i + 1]) < 0 || hex(value[i + 2]) < 0) throw Error(400, "invalid_argument", "Invalid URL encoding");
      result += static_cast<char>(hex(value[i + 1]) * 16 + hex(value[i + 2])); i += 2;
    } else result += value[i] == '+' ? ' ' : value[i];
  }
  return result;
}
struct Target {
  std::string path;
  std::map<std::string, std::string> query;
  explicit Target(const std::string& target) {
    auto q = target.find('?'); path = target.substr(0, q);
    if (q == std::string::npos) return;
    size_t start = q + 1;
    while (start < target.size()) {
      auto end = target.find('&', start); auto item = target.substr(start, end == std::string::npos ? end : end - start); auto eq = item.find('=');
      auto key = url_decode(item.substr(0, eq)); auto value = eq == std::string::npos ? "" : url_decode(item.substr(eq + 1));
      if (!query.emplace(key, value).second) throw Error(400, "invalid_argument", "Duplicate query parameter");
      if (end == std::string::npos) break; start = end + 1;
    }
  }
};
Claims authenticate(const Request& request, const Target& target, const Auth& auth) {
  auto header = std::string(request[http::field::authorization]);
  if (header.starts_with("Bearer ")) return auth.verify(header.substr(7));
  if (auto it = target.query.find("access_token"); it != target.query.end()) return auth.verify(it->second);
  throw Error(401, "unauthenticated", "Bearer token required");
}
struct ConnectionCount {
  std::shared_ptr<size_t> count;
  explicit ConnectionCount(std::shared_ptr<size_t> c) : count(std::move(c)) { ++*count; }
  ~ConnectionCount() { --*count; }
};
class WebSocketSession : public SignalSink, public std::enable_shared_from_this<WebSocketSession> {
 public:
  WebSocketSession(beast::tcp_stream stream, Rooms& rooms, std::shared_ptr<ConnectionCount> count)
      : socket_(std::move(stream)), rooms_(rooms), count_(std::move(count)) {}
  void run(Request request, Claims claims, bool auto_subscribe, bool reconnect) {
    request_ = std::move(request); claims_ = std::move(claims);
    beast::get_lowest_layer(socket_).expires_never();
    socket_.set_option(ws::stream_base::timeout::suggested(beast::role_type::server));
    socket_.read_message_max(1024 * 1024);
    socket_.async_accept(request_, [self = shared_from_this(), auto_subscribe, reconnect](beast::error_code error) {
      if (error) return self->finish();
      try {
        if (reconnect) {
          livekit::SignalResponse response; response.mutable_leave()->set_reason(livekit::STATE_MISMATCH); response.mutable_leave()->set_action(livekit::LeaveRequest::RECONNECT); response.mutable_leave()->set_can_reconnect(true);
          self->send(response); self->close(); return;
        }
        self->participant_ = self->rooms_.join(self->claims_, auto_subscribe, self); self->read();
      } catch (const std::exception& e) {
        std::cerr << "Join rejected: " << e.what() << '\n';
        livekit::SignalResponse res; res.mutable_leave()->set_reason(livekit::JOIN_FAILURE); self->send(res); self->close();
      }
    });
  }
  void send(const livekit::SignalResponse& response) override {
    if (finished_ || closing_) return;
    auto bytes = response.SerializeAsString();
    if (queued_bytes_ + bytes.size() > 4 * 1024 * 1024) {
      boost::asio::post(socket_.get_executor(), [self = shared_from_this()] { self->finish(); }); return;
    }
    queued_bytes_ += bytes.size(); queue_.push_back(std::move(bytes));
    if (queue_.size() == 1) write();
  }
  void close() override { closing_ = true; if (queue_.empty()) close_socket(); }
 private:
  void read() {
    socket_.async_read(buffer_, [self = shared_from_this()](beast::error_code error, size_t) {
      if (error) return self->finish();
      try {
        livekit::SignalRequest request; auto bytes = beast::buffers_to_string(self->buffer_.data());
        if (self->socket_.got_text()) request = from_json<livekit::SignalRequest>(Json::parse(bytes));
        else if (!request.ParseFromString(bytes)) throw Error(400, "invalid_argument", "Invalid protobuf signal");
        self->buffer_.consume(self->buffer_.size()); self->rooms_.signal(self->participant_, request);
      } catch (const std::exception& e) {
        std::cerr << "Signal rejected: " << e.what() << '\n';
        self->rooms_.leave(self->participant_, livekit::JOIN_FAILURE);
      }
      if (!self->finished_ && !self->closing_) self->read();
    });
  }
  void write() {
    socket_.binary(true);
    socket_.async_write(boost::asio::buffer(queue_.front()), [self = shared_from_this()](beast::error_code error, size_t) {
      if (error) return self->finish();
      self->queued_bytes_ -= self->queue_.front().size(); self->queue_.pop_front();
      if (!self->queue_.empty()) self->write(); else if (self->closing_) self->close_socket();
    });
  }
  void close_socket() {
    if (finished_ || close_started_) return;
    close_started_ = true;
    socket_.async_close(ws::close_code::normal, [self = shared_from_this()](beast::error_code) { self->finish(); });
  }
  void finish() {
    if (finished_) return;
    finished_ = true;
    beast::error_code ignored; beast::get_lowest_layer(socket_).socket().close(ignored);
    if (participant_ && !participant_->closed) rooms_.leave(participant_, livekit::SIGNAL_CLOSE);
  }
  ws::stream<beast::tcp_stream> socket_;
  Rooms& rooms_;
  std::shared_ptr<ConnectionCount> count_;
  Request request_;
  Claims claims_;
  std::shared_ptr<Participant> participant_;
  beast::flat_buffer buffer_;
  std::deque<std::string> queue_;
  size_t queued_bytes_ = 0;
  bool closing_ = false, close_started_ = false, finished_ = false;
};
class HttpSession : public std::enable_shared_from_this<HttpSession> {
 public:
  HttpSession(tcp::socket socket, Rooms& rooms, const Auth& auth, std::shared_ptr<size_t> connections)
      : stream_(std::move(socket)), rooms_(rooms), auth_(auth), count_(std::make_shared<ConnectionCount>(std::move(connections))) {}
  void run() {
    stream_.expires_after(std::chrono::seconds(10)); parser_.body_limit(1024 * 1024); parser_.header_limit(48 * 1024);
    http::async_read(stream_, buffer_, parser_, [self = shared_from_this()](beast::error_code error, size_t) {
      if (error) return;
      self->handle(parser_request(self));
    });
  }
 private:
  static Request parser_request(const std::shared_ptr<HttpSession>& self) { return self->parser_.release(); }
  void respond(int status, std::string body, const std::string& type = "application/json") {
    response_ = std::make_shared<http::response<http::string_body>>(static_cast<http::status>(status), 11);
    response_->set(http::field::content_type, type); response_->set(http::field::server, "livekit-cpp/0.1.0");
    response_->set(http::field::access_control_allow_origin, "*");
    response_->set(http::field::access_control_allow_headers, "Authorization, Content-Type");
    response_->set(http::field::access_control_allow_methods, "GET, POST, OPTIONS");
    response_->keep_alive(false); response_->body() = std::move(body); response_->prepare_payload();
    http::async_write(stream_, *response_, [self = shared_from_this()](beast::error_code, size_t) {
      beast::error_code ignored; self->stream_.socket().shutdown(tcp::socket::shutdown_send, ignored);
    });
  }
  void handle(Request request) {
    try {
      Target target(std::string(request.target()));
      if (request.method() == http::verb::options) { respond(204, ""); return; }
      if ((target.path == "/" || target.path == "/healthz") && request.method() == http::verb::get) { respond(200, "{\"status\":\"ok\",\"version\":\"0.1.0\"}"); return; }
      if (target.path == "/rtc" || target.path == "/rtc/validate") {
        if (request.method() != http::verb::get) throw Error(405, "bad_route", "GET required");
        auto claims = authenticate(request, target, auth_);
        if (!claims.join || claims.room.empty() || claims.identity.empty()) throw Error(403, "permission_denied", "Room join grant and identity required");
        if (target.query.contains("join_request")) throw Error(501, "unimplemented", "Single PeerConnection signaling is not supported; use /rtc dual PeerConnection mode");
        if (target.path == "/rtc/validate") { respond(200, "success", "text/plain"); return; }
        if (!ws::is_upgrade(request)) throw Error(400, "invalid_argument", "WebSocket upgrade required");
        auto session = std::make_shared<WebSocketSession>(std::move(stream_), rooms_, count_);
        bool auto_subscribe = !target.query.contains("auto_subscribe") || target.query["auto_subscribe"] != "0";
        bool reconnect = target.query.contains("reconnect") && target.query["reconnect"] == "1";
        session->run(std::move(request), std::move(claims), auto_subscribe, reconnect); return;
      }
      if (target.path == "/metrics") {
        auto c = authenticate(request, target, auth_); if (!c.list) throw Error(403, "permission_denied", "roomList required");
        auto metrics = rooms_.metrics();
        std::string text; for (const auto& item : metrics.items()) text += "livekit_cpp_" + item.key() + " " + item.value().dump() + "\n";
        respond(200, text, "text/plain; version=0.0.4"); return;
      }
      const std::string prefix = "/twirp/livekit.RoomService/";
      if (!target.path.starts_with(prefix)) throw Error(404, "bad_route", "Route not found");
      if (request.method() != http::verb::post) throw Error(405, "bad_route", "POST required");
      auto claims = authenticate(request, target, auth_); auto method = target.path.substr(prefix.size());
      auto* service = google::protobuf::DescriptorPool::generated_pool()->FindServiceByName("livekit.RoomService");
      auto* descriptor = service ? service->FindMethodByName(method) : nullptr;
      if (!descriptor) throw Error(404, "bad_route", "Unknown RoomService method");
      google::protobuf::DynamicMessageFactory factory;
      bool binary = std::string(request[http::field::content_type]).starts_with("application/protobuf");
      Json input;
      if (binary) {
        std::unique_ptr<google::protobuf::Message> proto(factory.GetPrototype(descriptor->input_type())->New());
        if (!proto->ParseFromString(request.body())) throw Error(400, "malformed", "Invalid protobuf request"); input = to_json(*proto);
      } else {
        if (!std::string(request[http::field::content_type]).starts_with("application/json")) throw Error(415, "bad_route", "Use application/json or application/protobuf");
        input = Json::parse(request.body());
      }
      auto output = rooms_.api(method, input, claims);
      if (binary) {
        std::unique_ptr<google::protobuf::Message> proto(factory.GetPrototype(descriptor->output_type())->New());
        auto status = google::protobuf::util::JsonStringToMessage(output.dump(), proto.get());
        if (!status.ok()) throw std::runtime_error(status.ToString());
        respond(200, proto->SerializeAsString(), "application/protobuf");
      } else respond(200, output.dump());
    } catch (const Error& error) { respond(error.status, Json{{"code", error.code}, {"msg", error.what()}}.dump()); }
    catch (const Json::exception&) { respond(400, "{\"code\":\"malformed\",\"msg\":\"Invalid JSON request\"}"); }
    catch (const std::exception& e) { std::cerr << "HTTP operation failed: " << e.what() << '\n'; respond(500, "{\"code\":\"internal\",\"msg\":\"Internal server error\"}"); }
  }
  beast::tcp_stream stream_;
  Rooms& rooms_;
  const Auth& auth_;
  std::shared_ptr<ConnectionCount> count_;
  beast::flat_buffer buffer_;
  http::request_parser<http::string_body> parser_;
  std::shared_ptr<http::response<http::string_body>> response_;
};
}
HttpServer::HttpServer(boost::asio::io_context& io, Rooms& rooms, const Config& config)
    : io_(io), rooms_(rooms), config_(config), auth_(config.keys), acceptor_(io) {
  auto endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(config.bind), config.port);
  acceptor_.open(endpoint.protocol()); acceptor_.set_option(boost::asio::socket_base::reuse_address(true)); acceptor_.bind(endpoint); acceptor_.listen(); accept();
}
void HttpServer::accept() {
  acceptor_.async_accept([this](boost::system::error_code error, boost::asio::ip::tcp::socket socket) {
    if (!error && *connections_ < config_.max_connections) std::make_shared<HttpSession>(std::move(socket), rooms_, auth_, connections_)->run();
    if (acceptor_.is_open()) accept();
  });
}
void HttpServer::stop() { boost::system::error_code error; acceptor_.close(error); }
}
