#pragma once
#include <chrono>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
#include <google/protobuf/util/json_util.h>

namespace lk {
using Json = nlohmann::json;

inline int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

struct Error : std::runtime_error {
  int status;
  std::string code;
  Json meta;

  Error(int status, std::string code, std::string message, Json meta = Json::object())
      : runtime_error(std::move(message)),
        status(status),
        code(std::move(code)),
        meta(std::move(meta)) {}

  Json body() const {
    Json result{{"code", code}, {"msg", what()}};
    if (!meta.empty())
      result["meta"] = meta;
    return result;
  }
};

inline Json to_json(const google::protobuf::Message& message) {
  std::string out;
  auto result = google::protobuf::util::MessageToJsonString(message, &out);
  if (!result.ok())
    throw std::runtime_error(result.ToString());
  return Json::parse(out);
}

template <typename T>
T from_json(const Json& json) {
  T result;
  auto status = google::protobuf::util::JsonStringToMessage(json.dump(), &result);
  if (!status.ok())
    throw Error(400, "invalid_argument", status.ToString());
  return result;
}

std::string random_id(const std::string& prefix);
}  // namespace lk
