#pragma once

#include "lk/auth.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <functional>

namespace lk {

// Reads AV_* deployment settings and generates a fresh process incarnation.
// An empty return means valid configuration; errors never contain secrets.
std::string configure_av(Config& config);
bool av_rtc_claims_valid(const Config& config, const Claims& claims, int64_t now);
bool av_management_claims_valid(const Config& config, const Claims& claims);
bool av_same_authorization(const Claims& first, const Claims& second);

struct AdmissionResult {
  bool allowed = false;
  int64_t expires_at = 0;
};

// Completion runs once on executor. The caller must keep config alive until
// completion; transport, deadline and JSON failures all deny admission.
void av_admission(boost::asio::any_io_executor executor, const Config& config, const Claims& claims,
                  std::function<void(AdmissionResult)> complete);

}  // namespace lk
