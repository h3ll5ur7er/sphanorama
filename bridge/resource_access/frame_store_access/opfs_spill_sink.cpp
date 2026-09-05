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

EM_JS(int32_t, host_spill_clear, (), {
  // The method, not just the host. `clear` is the only call here newer than the host itself, so
  // it is the only one a half-updated page can be missing — a service worker serving a cached
  // worker script beside a fresh .wasm, which is a combination this repo already has a test
  // about. Calling straight through would throw out of an EM_JS with no C++ frame to catch it and
  // abort the module, where returning 0 refuses the clear and lets Begin decline the session,
  // which is the outcome the refusal path was written for.
  if (!Module.sphSpill || typeof Module.sphSpill.clear !== 'function') return 0;
  return Module.sphSpill.clear() ? 1 : 0;
});

EM_JS(double, host_spill_generation, (), {
  // A negative answer means "cannot say", which is not the same as any token — the same
  // half-updated page the clear above guards against, a cached worker script beside a fresh
  // .wasm. Zero would be the wrong sentinel: that is what a host with no spill tier at all
  // reports, and a session document written against this one could then be matched to it later.
  if (!Module.sphSpill || typeof Module.sphSpill.generation !== 'function') return -1;
  var said = Module.sphSpill.generation();
  return typeof said === 'number' && Number.isSafeInteger(said) && said > 0 ? said : -1;
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

Status OpfsSpillSink::Clear() {
  if (host_spill_clear() == 0) {
    // A refusal here stops a session from beginning, which is the point: the frames still down
    // there belong to a sphere whose document may still name them, and a capture that started
    // anyway would issue their identities to its own frames.
    return Fail(StatusCode::Internal, kComponent, "the spill file could not be emptied");
  }
  return Status::Ok();
}

Result<uint64_t> OpfsSpillSink::Generation() {
  // The token crosses as a double, like the frame ids above and for the same reason: it is a
  // JavaScript number. It is minted with 52 bits so that it is exact on the way through, and the
  // host refuses to hand over anything that is not.
  const double said = host_spill_generation();
  if (said < 0) {
    // Refused rather than substituted. Any number invented here would be one a session document
    // could be written against and later matched to — the failure the token exists to remove,
    // reached by the component that was supposed to prevent it.
    return Err<uint64_t>(StatusCode::Unsupported, kComponent,
                         "this spill host cannot say which capture it is holding");
  }
  return Ok(static_cast<uint64_t>(said));
}

Status OpfsSpillSink::Drop(uint64_t frame) {
  if (host_spill_drop(static_cast<double>(frame)) == 0) {
    return Fail(StatusCode::Internal, kComponent, "the spilled frame could not be released");
  }
  return Status::Ok();
}

}  // namespace sphanorama::bridge
