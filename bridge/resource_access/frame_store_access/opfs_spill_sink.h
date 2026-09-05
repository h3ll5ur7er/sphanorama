#pragma once

#include <cstdint>
#include <span>

#include "resource_access/frame_store_access/spill_sink.h"

namespace sphanorama::bridge {

// The browser's spill destination: an OPFS sync access handle the worker opened at startup and
// holds for the session (ADR 0019, ADR 0020).
//
// It is five calls through to the page-side host and no state of its own. The file, the offsets
// inside it and the free list all live on the JavaScript side, because the allocator is where the
// handle is — and because the store above has no business knowing a file is involved at all.
//
// A host that is not installed — a browser with no origin private file system, or one whose
// handle failed to open — leaves every call failing with `Unsupported`. That is not the way a
// frame store copes with having nowhere to spill: the composition root simply does not hand it a
// sink in that case, and a store with no sink refuses to spill at all. These failures exist for
// the narrower case where the host was there and went away.
class OpfsSpillSink final : public ISpillSink {
 public:
  Status Write(uint64_t frame, std::span<const uint8_t> bytes) override;
  Status Read(uint64_t frame, std::span<uint8_t> bytes) override;
  Status Drop(uint64_t frame) override;
  Status Clear() override;
  Result<uint64_t> Generation() override;

  // Whether the worker installed a spill host at all. Read once by the composition root, which
  // is the only thing entitled to decide whether the store gets a sink.
  static bool Available();
};

}  // namespace sphanorama::bridge
