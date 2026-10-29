#pragma once
#include "lk/common.hpp"
#include <map>
#pragma push_macro("PASSTHROUGH")
#undef PASSTHROUGH
#include "livekit_rtc.pb.h"
#pragma pop_macro("PASSTHROUGH")

namespace lk {
// The pinned Go endpoint bounds the decompressed protobuf at 1 MiB.
livekit::JoinRequest decode_join_request(const std::string& encoded);
// Legacy /rtc identifies its signaling capabilities in query parameters.
livekit::ClientInfo parse_client_info(const std::map<std::string, std::string>& query);
}  // namespace lk
