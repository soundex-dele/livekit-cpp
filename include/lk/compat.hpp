#pragma once

#include <algorithm>
#include <string_view>
#include <vector>

namespace lk::compat {

template <class T, class Allocator, class Predicate>
auto erase_if(std::vector<T, Allocator>& container, Predicate predicate) {
  const auto size = container.size();
  container.erase(std::remove_if(container.begin(), container.end(), predicate), container.end());
  return size - container.size();
}

inline bool starts_with(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

inline bool ends_with(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace lk::compat
