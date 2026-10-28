#include "lk/twirp.hpp"
#include <set>
#include <vector>

namespace lk {
namespace {
using Descriptor = google::protobuf::Descriptor;
using Field = google::protobuf::FieldDescriptor;
const Field* find_field(const Descriptor* descriptor, const std::string& name) {
  if (!descriptor) return nullptr;
  if (auto field = descriptor->FindFieldByName(name)) return field;
  for (int i = 0; i < descriptor->field_count(); ++i)
    if (descriptor->field(i)->json_name() == name) return descriptor->field(i);
  return nullptr;
}
void null_messages(Json& json, const Descriptor* descriptor) {
  for (int i = 0; i < descriptor->field_count(); ++i) {
    const auto* field = descriptor->field(i);
    if (field->cpp_type() != Field::CPPTYPE_MESSAGE) continue;
    const auto& key = field->name();
    if (!json.contains(key)) {
      // EmitUnpopulated does not emit unset oneof or proto3 optional fields.
      if (!field->is_repeated() && !field->containing_oneof()) json[key] = nullptr;
      continue;
    }
    auto& value = json[key];
    if (field->is_map()) {
      auto* mapped = field->message_type()->FindFieldByName("value");
      if (mapped->cpp_type() == Field::CPPTYPE_MESSAGE)
        for (auto& item : value) null_messages(item, mapped->message_type());
    } else if (field->is_repeated()) {
      for (auto& item : value) null_messages(item, field->message_type());
    } else if (value.is_object()) null_messages(value, field->message_type());
  }
}
}
void parse_twirp_json(const std::string& body, google::protobuf::Message& message) {
  try {
    // encoding/json.Decoder.Decode reads the first value. protojson then
    // validates that object. Preserve this behavior even with trailing input.
    const auto start = body.find_first_not_of(" \t\r\n");
    if (start == std::string::npos || body[start] != '{') throw std::runtime_error("object required");
    size_t end = start;
    unsigned nesting = 0;
    bool quoted = false, escaped = false;
    for (; end < body.size(); ++end) {
      const char c = body[end];
      if (quoted) {
        if (escaped) escaped = false;
        else if (c == '\\') escaped = true;
        else if (c == '"') quoted = false;
      } else if (c == '"') quoted = true;
      else if (c == '{' || c == '[') { if (++nesting > 100) throw std::runtime_error("nesting limit"); }
      else if (c == '}' || c == ']') { if (!--nesting) { ++end; break; } }
    }
    struct Frame {
      const Descriptor* descriptor = nullptr;
      bool map = false, array = false;
      const Field* next = nullptr;
      std::set<std::string> seen;
    };
    std::vector<Frame> frames;
    auto callback = [&](int, Json::parse_event_t event, Json& parsed) {
      if (event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start) {
        Frame frame;
        frame.array = event == Json::parse_event_t::array_start;
        if (frames.empty()) frame.descriptor = message.GetDescriptor();
        else {
          const auto& parent = frames.back();
          if (parent.array) frame.descriptor = parent.descriptor;
          else if (parent.next && parent.next->cpp_type() == Field::CPPTYPE_MESSAGE) {
            frame.descriptor = parent.next->message_type(); frame.map = parent.next->is_map();
          }
        }
        frames.push_back(std::move(frame));
      } else if (event == Json::parse_event_t::key) {
        auto& frame = frames.back();
        const auto key = parsed.get<std::string>();
        frame.next = frame.map ? frame.descriptor->FindFieldByName("value") : find_field(frame.descriptor, key);
        if (frame.map || frame.next) {
          const auto canonical = frame.map ? key : frame.next->name();
          if (!frame.seen.insert(canonical).second) throw std::runtime_error("duplicate field");
        }
      } else if (event == Json::parse_event_t::object_end || event == Json::parse_event_t::array_end) frames.pop_back();
      return true;
    };
    auto json = Json::parse(body.begin() + start, body.begin() + end, callback);
    google::protobuf::util::JsonParseOptions options; options.ignore_unknown_fields = true;
    if (!google::protobuf::util::JsonStringToMessage(json.dump(), &message, options).ok())
      throw std::runtime_error("invalid protobuf JSON");
  } catch (const std::exception&) { throw Error(400, "malformed", "Invalid JSON request"); }
}
std::string print_twirp_json(const google::protobuf::Message& message) {
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true; options.always_print_primitive_fields = true;
  std::string text;
  const auto result = google::protobuf::util::MessageToJsonString(message, &text, options);
  if (!result.ok()) throw std::runtime_error(result.ToString());
  auto json = Json::parse(text); null_messages(json, message.GetDescriptor()); return json.dump();
}
}
