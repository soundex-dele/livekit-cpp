#pragma once
#include "lk/rooms.hpp"
#include <boost/asio/ip/tcp.hpp>

namespace lk {
class HttpServer {
 public:
  HttpServer(boost::asio::io_context&, Rooms&, const Config&);
  void stop();

 private:
  void accept();
  boost::asio::io_context& io_;
  Rooms& rooms_;
  const Config& config_;
  Auth auth_;
  boost::asio::ip::tcp::acceptor acceptor_;
  std::shared_ptr<size_t> connections_ = std::make_shared<size_t>(0);
};
}  // namespace lk
