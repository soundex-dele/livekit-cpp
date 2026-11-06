#include "lk/av_control.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/ssl.h>
#include <cstdlib>
#include <memory>

namespace lk {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

struct Endpoint {
  bool secure = false;
  std::string host, port, target;
};

bool parse_endpoint(const std::string& value, Endpoint& out) {
  size_t offset = 0;
  if (value.compare(0, 8, "https://") == 0) {
    out.secure = true;
    offset = 8;
  } else if (value.compare(0, 7, "http://") == 0) {
    offset = 7;
  } else {
    return false;
  }
  const auto slash = value.find('/', offset);
  const auto authority = value.substr(offset, slash == std::string::npos ? slash : slash - offset);
  if (authority.empty() || authority.find_first_of("@?#\r\n") != std::string::npos) {
    return false;
  }
  out.target = slash == std::string::npos ? "/" : value.substr(slash);
  if (out.target.find_first_of("?#\r\n") != std::string::npos) {
    return false;
  }
  out.port = out.secure ? "443" : "80";
  if (authority.front() == '[') {
    const auto close = authority.find(']');
    if (close == std::string::npos) {
      return false;
    }
    out.host = authority.substr(1, close - 1);
    if (close + 1 < authority.size()) {
      if (authority[close + 1] != ':') {
        return false;
      }
      out.port = authority.substr(close + 2);
    }
  } else {
    const auto colon = authority.find(':');
    out.host = authority.substr(0, colon);
    if (colon != std::string::npos) {
      out.port = authority.substr(colon + 1);
    }
  }
  return !out.host.empty() && !out.port.empty() &&
         out.port.find_first_not_of("0123456789") == std::string::npos;
}

std::string env(const char* key) {
  const auto* value = std::getenv(key);
  return value ? value : "";
}

class AdmissionCheck : public std::enable_shared_from_this<AdmissionCheck> {
 public:
  AdmissionCheck(asio::any_io_executor executor, const Config& config, const Claims& claims,
                 std::function<void(AdmissionResult)> complete)
      : resolver_(executor),
        deadline_(executor),
        tls_context_(ssl::context::tls_client),
        config_(config),
        claims_(claims),
        complete_(std::move(complete)) {}

  void run() {
    if (!parse_endpoint(config_.av_admission_url, endpoint_) ||
        (!endpoint_.secure && !config_.av_allow_insecure_admission) ||
        !av_rtc_claims_valid(config_, claims_, now_ms() / 1000)) {
      finish({});
      return;
    }
    beast::error_code error;
    if (endpoint_.secure) {
      tls_context_.set_default_verify_paths(error);
      if (!config_.av_admission_ca_file.empty()) {
        error.clear();
        tls_context_.load_verify_file(config_.av_admission_ca_file, error);
      }
      if (error) {
        finish({});
        return;
      }
      secure_ = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(resolver_.get_executor(),
                                                                       tls_context_);
      secure_->set_verify_mode(ssl::verify_peer, error);
      secure_->set_verify_callback(ssl::host_name_verification(endpoint_.host), error);
      if (error ||
          SSL_set_tlsext_host_name(secure_->native_handle(), endpoint_.host.c_str()) != 1) {
        finish({});
        return;
      }
    } else {
      plain_ = std::make_unique<beast::tcp_stream>(resolver_.get_executor());
    }
    request_ = {http::verb::post, endpoint_.target, 11};
    request_.set(http::field::host, endpoint_.host + ":" + endpoint_.port);
    request_.set(http::field::content_type, "application/json");
    request_.set(http::field::authorization, "Bearer " + config_.av_admission_secret);
    request_.body() = Json{{"node_id", config_.node_id},
                           {"node_incarnation", config_.node_incarnation},
                           {"room", claims_.room},
                           {"identity", claims_.identity},
                           {"grant_id", claims_.av_grant},
                           {"policy_version", claims_.av_policy_version},
                           {"binding_generation", claims_.av_binding_generation}}
                          .dump();
    request_.prepare_payload();
    response_.body_limit(8192);
    deadline_.expires_after(std::chrono::seconds(2));
    deadline_.async_wait([self = shared_from_this()](beast::error_code error) {
      if (!error) {
        self->finish({});
      }
    });
    resolver_.async_resolve(endpoint_.host, endpoint_.port,
                            [self = shared_from_this()](beast::error_code error,
                                                        tcp::resolver::results_type addresses) {
                              if (error || !self->complete_) {
                                self->finish({});
                                return;
                              }
                              if (self->secure_) {
                                self->connect(*self->secure_, addresses);
                              } else {
                                self->connect(*self->plain_, addresses);
                              }
                            });
  }

 private:
  template <typename Stream>
  void connect(Stream& stream, const tcp::resolver::results_type& addresses) {
    beast::get_lowest_layer(stream).async_connect(
        addresses, [self = shared_from_this()](beast::error_code error, const tcp::endpoint&) {
          if (error || !self->complete_) {
            self->finish({});
            return;
          }
          if (self->secure_) {
            self->secure_->async_handshake(ssl::stream_base::client,
                                           [self](beast::error_code error) {
                                             if (error || !self->complete_) {
                                               self->finish({});
                                               return;
                                             }
                                             self->write(*self->secure_);
                                           });
          } else {
            self->write(*self->plain_);
          }
        });
  }

  template <typename Stream>
  void write(Stream& stream) {
    http::async_write(stream, request_,
                      [self = shared_from_this()](beast::error_code error, size_t) {
                        if (error || !self->complete_) {
                          self->finish({});
                          return;
                        }
                        if (self->secure_) {
                          self->read(*self->secure_);
                        } else {
                          self->read(*self->plain_);
                        }
                      });
  }

  template <typename Stream>
  void read(Stream& stream) {
    http::async_read(
        stream, buffer_, response_, [self = shared_from_this()](beast::error_code error, size_t) {
          if (error || !self->complete_ || self->response_.get().result_int() != 200) {
            self->finish({});
            return;
          }
          auto body = Json::parse(self->response_.get().body(), nullptr, false);
          if (!body.is_object() || !body.contains("allowed") || body["allowed"] != true ||
              !body.value("policy_version", Json()).is_number_integer() ||
              !body.value("binding_generation", Json()).is_number_integer() ||
              !body.value("expires_at", Json()).is_number_integer()) {
            self->finish({});
            return;
          }
          const auto expiration = body["expires_at"].get<int64_t>();
          const bool allowed = body["policy_version"] == self->claims_.av_policy_version &&
                               body["binding_generation"] == self->claims_.av_binding_generation &&
                               expiration > now_ms() / 1000 &&
                               expiration <= self->claims_.av_grant_expires_at;
          self->finish({allowed, allowed ? expiration : 0});
        });
  }

  void finish(AdmissionResult result) {
    if (!complete_) {
      return;
    }
    auto complete = std::move(complete_);
    resolver_.cancel();
    deadline_.cancel();
    beast::error_code ignored;
    if (secure_) {
      beast::get_lowest_layer(*secure_).socket().close(ignored);
    }
    if (plain_) {
      plain_->socket().close(ignored);
    }
    complete(result);
  }

  tcp::resolver resolver_;
  asio::steady_timer deadline_;
  ssl::context tls_context_;
  const Config& config_;
  Claims claims_;
  Endpoint endpoint_;
  std::function<void(AdmissionResult)> complete_;
  std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> secure_;
  std::unique_ptr<beast::tcp_stream> plain_;
  beast::flat_buffer buffer_;
  http::request<http::string_body> request_;
  http::response_parser<http::string_body> response_;
};
}  // namespace

std::string configure_av(Config& config) {
  config.av_mode = env("AV_MEDIA_MODE") == "true";
  config.node_incarnation = random_id("NI_");
  if (!config.av_mode) {
    return {};
  }
  config.node_id = env("AV_NODE_ID");
  config.av_rtc_key = env("AV_RTC_KEY");
  config.av_management_key = env("AV_MANAGEMENT_KEY");
  config.av_admission_url = env("AV_ADMISSION_URL");
  config.av_admission_secret = env("AV_ADMISSION_SECRET");
  config.av_admission_ca_file = env("AV_ADMISSION_CA_FILE");
  config.av_allow_insecure_admission = env("AV_ALLOW_INSECURE_ADMISSION") == "true";
  Endpoint endpoint;
  if (config.node_id.empty() || config.node_id.size() > 128 || config.av_rtc_key.empty() ||
      config.av_rtc_key == config.av_management_key || !config.keys.count(config.av_rtc_key) ||
      !config.keys.count(config.av_management_key) || config.av_admission_secret.size() < 32 ||
      !parse_endpoint(config.av_admission_url, endpoint) ||
      (!endpoint.secure && !config.av_allow_insecure_admission)) {
    return "AV mode requires a node ID, distinct RTC/management key issuers, a 32-byte admission "
           "secret and an HTTPS admission URL (HTTP requires explicit development opt-in)";
  }
  const auto& rtc_secret = config.keys.find(config.av_rtc_key)->second;
  const auto& management_secret = config.keys.find(config.av_management_key)->second;
  if (rtc_secret.size() < 32 || management_secret.size() < 32 || rtc_secret == management_secret) {
    return "AV mode requires distinct RTC and management secrets of at least 32 bytes";
  }
  return {};
}

bool av_rtc_claims_valid(const Config& config, const Claims& claims, int64_t now) {
  return config.av_mode && claims.issuer == config.av_rtc_key && claims.join && !claims.create &&
         !claims.admin && !claims.list && !claims.av_grant.empty() &&
         claims.av_grant.size() <= 256 && !claims.identity.empty() && !claims.room.empty() &&
         claims.av_policy_version > 0 && claims.av_binding_generation > 0 &&
         claims.av_node_id == config.node_id &&
         claims.av_node_incarnation == config.node_incarnation && claims.expires_at > now &&
         claims.av_grant_expires_at >= claims.expires_at;
}

bool av_management_claims_valid(const Config& config, const Claims& claims) {
  return config.av_mode && claims.issuer == config.av_management_key && !claims.join &&
         claims.av_grant.empty();
}

bool av_same_authorization(const Claims& first, const Claims& second) {
  return first.av_grant == second.av_grant && first.av_policy_version == second.av_policy_version &&
         first.av_binding_generation == second.av_binding_generation &&
         first.av_node_id == second.av_node_id &&
         first.av_node_incarnation == second.av_node_incarnation;
}

void av_admission(asio::any_io_executor executor, const Config& config, const Claims& claims,
                  std::function<void(AdmissionResult)> complete) {
  auto callback = std::make_shared<std::function<void(AdmissionResult)>>(std::move(complete));
  auto once = [callback](AdmissionResult result) {
    auto notify = std::move(*callback);
    if (notify) {
      notify(result);
    }
  };
  // OpenSSL/Asio context construction is a third-party exception boundary.
  try {
    auto check = std::make_shared<AdmissionCheck>(executor, config, claims, once);
    check->run();
  } catch (const std::exception&) {
    once({});
  }
}

}  // namespace lk
