#include "resource_access/frame_store_access/opfs_spill_sink.h"

#include <emscripten/emscripten.h>

namespace sphanorama::bridge {
namespace {

constexpr const char* kComponent = "OpfsSpillSink";

// The host side of the port. Each call is synchronous on the worker's sync access handle, which
// is the entire reason the core is in a worker (ADR 0019).
//
// Frame ids cross as doubles because that is what a JavaScript number is. They come from the
// store's own counter and are small, so the 2^53 exactness limit is not reachable here — unlike
// project ids, which arrive from storage and are range-checked before use.
//
// The byte spans cross as a pointer and a length, viewed through HEAPU8 rather than copied. The
// view is taken and used inside one synchronous call with no allocation in between, so memory
// growth cannot detach it underneath: growth happens on the C++ side of a call, never inside
// one of these.

EM_JS(int32_t, host_spill_available, (), {
  return Module.sphSpill ? 1 : 0;
});

EM_JS(int32_t, host_spill_write, (double frame, const uint8_t* bytes, int32_t size), {
  if (!Module.sphSpill) return 0;
  return Module.sphSpill.write(frame, HEAPU8.subarray(bytes, bytes + size)) ? 1 : 0;
});

EM_JS(int32_t, host_spill_read, (double frame, uint8_t* into, int32_t size), {
  if (!Module.sphSpill) return 0;
  return Module.sphSpill.read(frame, HEAPU8.subarray(into, into + size)) ? 1 : 0;
});

EM_JS(int32_t, host_spill_drop, (double frame), {
  if (!Module.sphSpill) return 0;
  return Module.sphSpill.drop(frame) ? 1 : 0;
});

}  // namespace

bool OpfsSpillSink::Available() { return host_spill_available() != 0; }

Status OpfsSpillSink::Write(uint64_t frame, std::span<const uint8_t> bytes) {
  if (bytes.empty()) return Fail(StatusCode::InvalidArgument, kComponent, "nothing to spill");
  if (host_spill_write(static_cast<double>(frame), bytes.data(),
                       static_cast<int32_t>(bytes.size())) == 0) {
    // The host does not say why, and the honest reason on a phone is almost always the same one:
    // there is no more room. A caller that could tell the two apart would do nothing different —
    // the store keeps the frame in the heap either way.
    return Fail(StatusCode::FrameStoreExhausted, kComponent,
                "the spill file would not take this frame");
  }
  return Status::Ok();
}

Status OpfsSpillSink::Read(uint64_t frame, std::span<uint8_t> bytes) {
  if (bytes.empty()) return Fail(StatusCode::InvalidArgument, kComponent, "nothing to read into");
  if (host_spill_read(static_cast<double>(frame), bytes.data(),
                      static_cast<int32_t>(bytes.size())) == 0) {
    return Fail(StatusCode::Internal, kComponent, "the spilled frame could not be read back");
  }
  return Status::Ok();
}

Status OpfsSpillSink::Drop(uint64_t frame) {
  if (host_spill_drop(static_cast<double>(frame)) == 0) {
    return Fail(StatusCode::Internal, kComponent, "the spilled frame could not be released");
  }
  return Status::Ok();
}

}  // namespace sphanorama::bridge
