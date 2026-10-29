#pragma once
#include "lk/auth.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <functional>

namespace lk {
// Asynchronous loopback-only check. A missing/unavailable control plane fails closed.
void studio_admission(boost::asio::any_io_executor executor, const Claims& claims,
                      std::function<void(bool)> completion);
}  // namespace lk
