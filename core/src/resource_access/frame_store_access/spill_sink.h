#pragma once

#include <cstdint>
#include <span>

#include "sphanorama/types.h"

namespace sphanorama {

// Where a spilled frame's bytes actually go.
//
// A seam inside the frame-store component rather than a contract of its own, and deliberately so.
// V11 is *where pixel bytes live*, and IFrameStoreAccess owns it: residency tiers, the ceiling,
// the fault-in on Pin and every decision about when a frame should leave the heap are the store's
// and stay there. What varies underneath is only the destination — RAM on a desktop, an OPFS sync
// access handle on a phone — and that is one component's implementation detail, not a second axis.
// Promoting it to contracts/ would make it a resource access that another resource access calls,
// which is the coupling the layer rules exist to prevent.
//
// It is three calls because that is all a spill needs. There is no listing, no iteration and no
// notion of a directory: the store knows exactly which frames it put here and asks for them by
// the identity it gave them.
class ISpillSink {
 public:
  virtual ~ISpillSink() = default;

  // Takes a copy. The store frees its own buffer immediately afterwards, so a sink that merely
  // borrowed the span would have spilled nothing.
  virtual Status Write(uint64_t frame, std::span<const uint8_t> bytes) = 0;

  // Fills `bytes` completely or fails. A short read is a failure rather than a partial frame:
  // half a frame that reports success is a stitch artefact nobody can trace back to here.
  virtual Status Read(uint64_t frame, std::span<uint8_t> bytes) = 0;

  // Idempotent. The store calls it when a spilled frame is forgotten, and a sink that has already
  // lost the frame has nothing to report — the outcome the caller wanted is the outcome it has.
  virtual Status Drop(uint64_t frame) = 0;

  // Drops everything, without being told what everything is.
  //
  // The fourth call, and it does not break the no-listing rule above: the store still never asks
  // what is down here, it says that none of it is wanted any more. A listing would be the other
  // thing — the sink telling the store what it holds — and that is what would make the store's
  // map the second copy of a truth the sink already had.
  //
  // It exists because a sink outlives the process that filled it (ADR 0030) while frame
  // identities restart at 1 in the next one, so the identities the store is about to issue are
  // already taken down here. Dropping them one at a time is not open to a store that has just
  // started and knows of no frames at all.
  virtual Status Clear() = 0;
};

}  // namespace sphanorama
