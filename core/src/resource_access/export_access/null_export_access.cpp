#include "resource_access/export_access/null_export_access.h"

namespace sphanorama {
namespace {
constexpr const char* kComponent = "NullExportAccess";
constexpr const char* kReason = "no export port: the browser adapter is not wired to the core yet";
}  // namespace

Status NullExportAccess::Save(std::string_view, std::string_view, std::span<const uint8_t>) {
  return Fail(StatusCode::Unsupported, kComponent, kReason);
}
Result<bool> NullExportAccess::CanShare() { return Ok(false); }
Status NullExportAccess::Share(std::string_view, std::string_view, std::span<const uint8_t>) {
  return Fail(StatusCode::Unsupported, kComponent, kReason);
}

}  // namespace sphanorama
