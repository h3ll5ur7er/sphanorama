#pragma once
#include <span>
#include <string_view>
#include "sphanorama/types.h"

namespace sphanorama {

// V15 — how a result leaves the device.
// @boundary
class IExportAccess {
 public:
  virtual ~IExportAccess() = default;

  virtual Status Save(std::string_view filename, std::string_view mimeType,
                      std::span<const uint8_t> bytes) = 0;
  virtual Result<bool> CanShare() = 0;
  virtual Status Share(std::string_view filename, std::string_view mimeType,
                       std::span<const uint8_t> bytes) = 0;
};

}  // namespace sphanorama
