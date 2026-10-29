#pragma once
#include "lk/common.hpp"

namespace lk {
// Go Twirp decodes one JSON value, discards unknown fields, but rejects
// duplicate known fields (including their JSON/proto-name aliases).
void parse_twirp_json(const std::string&, google::protobuf::Message&);
std::string print_twirp_json(const google::protobuf::Message&);
}  // namespace lk
