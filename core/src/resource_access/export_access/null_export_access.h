#pragma once

#include "sphanorama/resource_access/export_access.h"

namespace sphanorama {

// Stands in until the browser port lands. Export is where bytes leave the device, so refusing
// is the only honest answer from a core that has no way to hand them anywhere.
class NullExportAccess final : public IExportAccess {
 public:
  Status Save(std::string_view filename, std::string_view mimeType,
              std::span<const uint8_t> bytes) override;
  Result<bool> CanShare() override;
  Status Share(std::string_view filename, std::string_view mimeType,
               std::span<const uint8_t> bytes) override;
};

}  // namespace sphanorama
