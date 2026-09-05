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
// Three of the five calls are a spill: the store knows exactly which frames it put here and asks
// for them by the identity it gave them. The other two are about the tier as a whole — emptying
// it, and asking which capture is in it — and neither is a listing. There is still no iteration
// and no notion of a directory here, which is the invariant: the sink never tells the store what
// it holds.
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

  // Which capture is down here, as an opaque token. The fifth call, and the one that is about the
  // tier rather than about a frame.
  //
  // A sink outlives the process that filled it and the store above restarts its identities at 1,
  // so "frame 7" names a different frame in each capture the tier has held. The token is what
  // tells them apart: it is minted fresh whenever the tier is emptied, it is written down beside
  // the frames, and it comes back with them. Whoever writes a session document records it, and a
  // resume that reads a different one knows the pixels it is about are gone (ADR 0035).
  //
  // Not a listing, for the same reason Clear is not: the sink says which capture it holds, never
  // what it holds.
  //
  // Fallible because a sink can be present and unable to say — the browser's host is a page-side
  // object and a page can be half-updated. The store passes that refusal up rather than inventing
  // a token, since a wrong answer here is a document that matches a tier it does not belong to.
  virtual Result<uint64_t> Generation() = 0;
};

}  // namespace sphanorama
