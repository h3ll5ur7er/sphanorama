#include "support/fake_export_access.h"

namespace sphanorama {
namespace {
constexpr const char* kComponent = "FakeExportAccess";
}

Status FakeExportAccess::Save(std::string_view filename, std::string_view mimeType,
                              std::span<const uint8_t> bytes) {
  if (filename.empty()) return Fail(StatusCode::InvalidArgument, kComponent, "filename is empty");
  artifacts_.push_back(Artifact{std::string(filename), std::string(mimeType),
                                std::vector<uint8_t>(bytes.begin(), bytes.end()), false});
  return Status::Ok();
}

Result<bool> FakeExportAccess::CanShare() { return Ok(can_share_); }

Status FakeExportAccess::Share(std::string_view filename, std::string_view mimeType,
                               std::span<const uint8_t> bytes) {
  if (!can_share_) {
    return Fail(StatusCode::Unsupported, kComponent, "no share target on this platform");
  }
  artifacts_.push_back(Artifact{std::string(filename), std::string(mimeType),
                                std::vector<uint8_t>(bytes.begin(), bytes.end()), true});
  return Status::Ok();
}

}  // namespace sphanorama
