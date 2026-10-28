#include "lk/studio_admission.hpp"
#include <boost/beast.hpp>
#include <cstdlib>
namespace lk {
namespace {
namespace http = boost::beast::http;
struct Check : std::enable_shared_from_this<Check> {
  boost::beast::tcp_stream stream;
  boost::beast::flat_buffer buffer;
  http::request<http::string_body> request;
  http::response_parser<http::string_body> response;
  std::function<void(bool)> done;
  Check(boost::asio::any_io_executor ex, std::function<void(bool)> cb) : stream(ex), done(std::move(cb)) {}
  void finish(bool ok) { if (done) { auto cb = std::move(done); boost::system::error_code e; stream.socket().close(e); cb(ok); } }
  void run(const Claims& c) {
    const bool conference = !c.conference_grant.empty();
    const char* secret = std::getenv(conference ? "CONFERENCE_ADMISSION_SECRET" : "LIVE_STUDIO_ADMISSION_SECRET");
    const char* configured = std::getenv(conference ? "CONFERENCE_ADMISSION_PORT" : "LIVE_STUDIO_ADMISSION_PORT");
    int port = 8080;
    try { if (configured) port = std::stoi(configured); } catch (...) { finish(false); return; }
    if (!secret || std::string(secret).size() < 24 || port < 1 || port > 65535) { finish(false); return; }
    request = {http::verb::post, conference ? "/api/v1/conference/admission" : "/api/v1/live-studio/admission", 11};
    request.set(http::field::host, "127.0.0.1"); request.set(http::field::content_type, "application/json");
    request.set(http::field::authorization, std::string("Bearer ") + secret);
    request.body() = Json{{"grant", conference ? c.conference_grant : c.studio_grant}, {"room", c.room}, {"identity", c.identity}}.dump(); request.prepare_payload();
    response.body_limit(8192); stream.expires_after(std::chrono::seconds(2));
    stream.async_connect({boost::asio::ip::make_address("127.0.0.1"), static_cast<uint16_t>(port)},
      [self=shared_from_this()](boost::system::error_code e) {
        if (e) { self->finish(false); return; }
        http::async_write(self->stream, self->request, [self](boost::system::error_code e, size_t) {
          if (e) { self->finish(false); return; }
          http::async_read(self->stream, self->buffer, self->response, [self](boost::system::error_code e, size_t) {
            self->finish(!e && self->response.get().result_int() == 200);
          });
        });
      });
  }
};
}
void studio_admission(boost::asio::any_io_executor executor, const Claims& c, std::function<void(bool)> cb) {
  auto check = std::make_shared<Check>(executor, std::move(cb)); check->run(c);
}
}
