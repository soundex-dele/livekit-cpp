#pragma once
#include "lk/common.hpp"
#pragma push_macro("PASSTHROUGH")
#undef PASSTHROUGH
#include "livekit_rtc.pb.h"
#pragma pop_macro("PASSTHROUGH")

namespace lk {
// The pinned Go endpoint bounds the decompressed protobuf at 1 MiB.
livekit::JoinRequest decode_join_request(const std::string& encoded);
}
