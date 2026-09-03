#include "resource_access/project_store_access/browser_project_store_access.h"

#include <emscripten/emscripten.h>

#include <vector>

namespace sphanorama::bridge {
namespace {

constexpr const char* kComponent = "BrowserProjectStoreAccess";

// The host side of the port. Each of these calls a synchronous method on the document host the
// page installed before the core was used; none of them touches IndexedDB directly, because the
// host has already made the documents resident (ADR 0014).
//
// Strings come back as malloc'd UTF-8 that C++ owns and frees — the standard Emscripten
// convention for returning a string across the boundary.

EM_JS(int32_t, host_project_count, (), {
  return Module.sphHost ? Module.sphHost.projectIds().length : 0;
});

EM_JS(double, host_project_id_at, (int32_t index), {
  return Module.sphHost.projectIds()[index];
});

// Read in two steps, so nothing here allocates on the JavaScript side.
//
// The single-call version malloc'd a buffer and wrote the document through the pointer without
// checking it. With memory growth enabled an exhausted heap returns 0, which is a valid heap
// offset: the document landed on the start of linear memory, and returning that 0 reported the
// failure as NotFound — an out-of-memory read indistinguishable from a document that was never
// written, which is exactly the confusion that makes a resume start from a blank plan.
//
// Asking for the size first lets C++ own the allocation. It is two Map lookups instead of one,
// both synchronous, with no chance for the value to change in between.

// -1 when the document does not exist, so absence stays distinct from an empty document.
EM_JS(int32_t, host_document_size, (double project, const char* key), {
  if (!Module.sphHost) return -1;
  const value = Module.sphHost.read(project, UTF8ToString(key));
  if (value === undefined || value === null) return -1;
  return lengthBytesUTF8(value);
});

EM_JS(void, host_document_read, (double project, const char* key, char* out, int32_t size), {
  const value = Module.sphHost.read(project, UTF8ToString(key));
  if (value === undefined || value === null) return;
  stringToUTF8(value, out, size + 1);   // stringToUTF8 wants room for its NUL
});

EM_JS(int32_t, host_write_document, (double project, const char* key, const char* value), {
  if (!Module.sphHost) return 0;
  Module.sphHost.write(project, UTF8ToString(key), UTF8ToString(value));
  return 1;
});

EM_JS(int32_t, host_delete_project, (double project), {
  return Module.sphHost && Module.sphHost.remove(project) ? 1 : 0;
});

}  // namespace

Result<std::vector<ProjectId>> BrowserProjectStoreAccess::ListProjects() {
  const int32_t count = host_project_count();
  std::vector<ProjectId> ids;
  ids.reserve(static_cast<size_t>(count < 0 ? 0 : count));
  for (int32_t i = 0; i < count; ++i) {
    ids.push_back(ProjectId{static_cast<uint64_t>(host_project_id_at(i))});
  }
  return Ok(std::move(ids));
}

Result<std::string> BrowserProjectStoreAccess::ReadDocument(ProjectId project,
                                                            std::string_view key) {
  const std::string owned_key(key);
  const int32_t size = host_document_size(static_cast<double>(project.value), owned_key.c_str());
  if (size < 0) {
    return Err<std::string>(StatusCode::NotFound, kComponent, "no such document");
  }
  if (size == 0) return Ok(std::string{});

  // One byte of slack for the NUL stringToUTF8 always writes; the string itself keeps its size.
  std::string out(static_cast<size_t>(size) + 1, '\0');
  host_document_read(static_cast<double>(project.value), owned_key.c_str(), out.data(), size);
  out.resize(static_cast<size_t>(size));
  return Ok(std::move(out));
}

Status BrowserProjectStoreAccess::WriteDocument(ProjectId project, std::string_view key,
                                                std::string_view value) {
  if (!project.valid()) return Fail(StatusCode::InvalidArgument, kComponent, "project id is unset");
  if (key.empty()) return Fail(StatusCode::InvalidArgument, kComponent, "document key is empty");

  const std::string owned_key(key);
  const std::string owned_value(value);
  if (host_write_document(static_cast<double>(project.value), owned_key.c_str(),
                          owned_value.c_str()) == 0) {
    return Fail(StatusCode::FailedPrecondition, kComponent, "no document host installed");
  }
  return Status::Ok();
}

Status BrowserProjectStoreAccess::DeleteProject(ProjectId project) {
  if (host_delete_project(static_cast<double>(project.value)) == 0) {
    return Fail(StatusCode::NotFound, kComponent, "no such project");
  }
  return Status::Ok();
}

}  // namespace sphanorama::bridge
