#include "utilities/config_store.h"

namespace sphanorama {

bool MapConfigStore::Flag(std::string_view key, bool fallback) {
  const auto it = flags_.find(key);
  return it == flags_.end() ? fallback : it->second;
}

double MapConfigStore::Number(std::string_view key, double fallback) {
  const auto it = numbers_.find(key);
  return it == numbers_.end() ? fallback : it->second;
}

std::string_view MapConfigStore::Text(std::string_view key, std::string_view fallback) {
  const auto it = texts_.find(key);
  return it == texts_.end() ? fallback : std::string_view(it->second);
}

void MapConfigStore::SetFlag(std::string key, bool value) { flags_[std::move(key)] = value; }

void MapConfigStore::SetNumber(std::string key, double value) { numbers_[std::move(key)] = value; }

void MapConfigStore::SetText(std::string key, std::string value) {
  texts_[std::move(key)] = std::move(value);
}

}  // namespace sphanorama
