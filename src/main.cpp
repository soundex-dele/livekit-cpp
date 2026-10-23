#include "lk/http.hpp"
#include <boost/asio/signal_set.hpp>
#include <iostream>
#if defined(_WIN32)
#include <rtc_base/win32_socket_init.h>
#endif
int main(int argc, char** argv) {
  try {
    lk::Config config; bool token = false, dev = false;
    std::string path, room, identity, bind; int port = 0;
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      auto value = [&]() -> std::string { if (++i >= argc) throw std::runtime_error("Missing value for " + arg); return argv[i]; };
      if (arg == "--dev") dev = true;
      else if (arg == "--config") path = value();
      else if (arg == "--bind") bind = value();
      else if (arg == "--port") { auto s = value(); size_t n = 0; port = std::stoi(s, &n); if (n != s.size() || port < 1 || port > 65535) throw std::runtime_error("Invalid port"); }
      else if (arg == "--token") token = true;
      else if (arg == "--room") room = value();
      else if (arg == "--identity") identity = value();
      else if (arg == "--version") { std::cout << "livekit-cpp 0.1.0\n"; return 0; }
      else if (arg == "--help") { std::cout << "livekit-server [--dev | --config file.json] [--bind IP] [--port 7880]\n"
        "livekit-server --dev --token --room demo --identity alice\n"; return 0; }
      else throw std::runtime_error("Unknown option: " + arg);
    }
    if (!path.empty()) config = lk::Config::load(path);
    if (dev) { if (!path.empty()) throw std::runtime_error("Use either --dev or --config"); config.keys = {{"devkey", "secret"}}; }
    if (!bind.empty()) config.bind = bind;
    if (port) config.port = static_cast<uint16_t>(port);
    config.validate();
    if (token) {
      if (room.empty() || identity.empty()) throw std::runtime_error("--token needs --room and --identity");
      auto [key, secret] = *config.keys.begin();
      std::cout << lk::Auth::sign({{"iss", key}, {"sub", identity}, {"nbf", lk::now_ms() / 1000 - 1}, {"exp", lk::now_ms() / 1000 + 3600}, {"video", {{"roomJoin", true}, {"room", room}, {"canUpdateOwnMetadata", true}}}}, secret) << '\n'; return 0;
    }
#if defined(_WIN32)
    webrtc::WinsockInitializer winsock; if (winsock.error()) throw std::runtime_error("Winsock initialization failed");
#endif
    boost::asio::io_context io;
    lk::RtcRuntime rtc;
    lk::Rooms rooms(io, rtc, config);
    lk::HttpServer server(io, rooms, config);
    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    boost::asio::steady_timer shutdown_timer(io);
    signals.async_wait([&](auto, auto) {
      server.stop(); rooms.shutdown();
      shutdown_timer.expires_after(std::chrono::milliseconds(500)); shutdown_timer.async_wait([&](auto) { io.stop(); });
    });
    std::cout << "livekit-cpp listening on http://" << config.bind << ':' << config.port << " (native WebRTC, UDP " << config.rtc_port_min << '-' << config.rtc_port_max << ")" << std::endl;
    io.run(); rooms.shutdown();
  } catch (const std::exception& e) { std::cerr << "Error: " << e.what() << '\n'; return 1; }
  return 0;
}
