#pragma once

#include <map>
#include <string>
#include <string_view>

#include "sphanorama/utilities/config_store.h"

namespace sphanorama {

// An in-memory config store. Backed by std::map rather than a vector or unordered_map because
// Text hands out a view into stored data, and node-based storage keeps that view valid as the
// store grows. (Overwriting a key does invalidate views of its old value.)
//
// The three keyspaces are separate, so a flag and a number may share a name without callers
// having to coordinate.
class MapConfigStore final : public IConfigStore {
 public:
  bool Flag(std::string_view key, bool fallback) override;
  double Number(std::string_view key, double fallback) override;
  std::string_view Text(std::string_view key, std::string_view fallback) override;

  void SetFlag(std::string key, bool value);
  void SetNumber(std::string key, double value);
  void SetText(std::string key, std::string value);

 private:
  // std::less<> gives heterogeneous lookup, so reads do not allocate a std::string per call.
  std::map<std::string, bool, std::less<>> flags_;
  std::map<std::string, double, std::less<>> numbers_;
  std::map<std::string, std::string, std::less<>> texts_;
};

}  // namespace sphanorama
