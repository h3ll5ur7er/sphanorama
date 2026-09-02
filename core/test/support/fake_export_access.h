#pragma once

#include <string>
#include <vector>

#include "sphanorama/resource_access/export_access.h"

namespace sphanorama {

// Records what left the device. Export is the one place bytes escape, so a test asserting
// "nothing was exported" is as useful as one asserting the filename and payload.
class FakeExportAccess final : public IExportAccess {
 public:
  struct Artifact {
    std::string filename;
    std::string mimeType;
    std::vector<uint8_t> bytes;
    bool shared = false;
  };

  explicit FakeExportAccess(bool canShare = true) : can_share_(canShare) {}

  Status Save(std::string_view filename, std::string_view mimeType,
              std::span<const uint8_t> bytes) override;
  Result<bool> CanShare() override;
  Status Share(std::string_view filename, std::string_view mimeType,
               std::span<const uint8_t> bytes) override;

  const std::vector<Artifact>& artifacts() const { return artifacts_; }

 private:
  bool can_share_;
  std::vector<Artifact> artifacts_;
};

}  // namespace sphanorama
