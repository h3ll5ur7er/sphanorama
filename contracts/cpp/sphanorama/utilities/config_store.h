#pragma once
#include <string_view>
#include "sphanorama/types.h"

namespace sphanorama {

// Algorithm parameters live here rather than in constants so the bench client can sweep them
// without a rebuild, and so a device-class profile can override them at startup.
class IConfigStore {
 public:
  virtual ~IConfigStore() = default;
  virtual bool Flag(std::string_view key, bool fallback) = 0;
  virtual double Number(std::string_view key, double fallback) = 0;
  virtual std::string_view Text(std::string_view key, std::string_view fallback) = 0;
};

}  // namespace sphanorama
