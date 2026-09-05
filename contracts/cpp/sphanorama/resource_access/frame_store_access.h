#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// V11 — where pixel bytes live. The only component that knows how much memory exists, and the
// reason a 15 GB sphere of bursts fits on a phone.
//
// Not marked @boundary: this contract moves bytes through the shared heap rather than
// through marshalled values, so its TypeScript adapter is written against the shared-heap
// protocol rather than mirroring this signature. See ADR 0009.
class IFrameStoreAccess {
 public:
  virtual ~IFrameStoreAccess() = default;

  virtual Result<FrameStoreBudget> Budget() = 0;
  virtual Result<FrameRef> Allocate(int32_t width, int32_t height, PixelFormat) = 0;

  // Pin promotes to HeapPinned, faulting in from spill if needed, and guarantees the mapping
  // until Release. Engines read pixels only between Pin and Release.
  virtual Result<std::span<uint8_t>> Pin(const FrameRef&) = 0;
  virtual Status Release(const FrameRef&) = 0;

  // Residency is store state rather than frame identity, so it is queried, never carried on
  // the handle: a FrameRef copied before a spill would otherwise lie about where its bytes are.
  virtual Result<Residency> ResidencyOf(const FrameRef& frame) = 0;

  // Moves a frame to a cheaper tier. Only tiers a demotion can actually produce are accepted:
  // `HeapPinned` is what Pin establishes and not something a store may assert on its own, and a
  // store with no GPU tier refuses `GpuTexture` rather than pretending. Taking any value would
  // have ResidencyOf reporting a state that was never true.
  virtual Status Demote(const FrameRef&, Residency target) = 0;
  // Takes back a frame this store never allocated, whose bytes are already in the sink.
  //
  // The identity comes from the caller, which is the whole point of it: a store dies with its
  // tab, and after a reload the only thing left naming the frames a capture took is the session
  // document. The frame arrives `Spilled` and a later `Pin` faults it in exactly as it would one
  // this store had demoted itself.
  //
  // It cannot check that the bytes are down there. A sink answers for frames it was given and
  // offers no listing — deliberately, so that the store stays the only thing that knows what it
  // put where — so a frame adopted against a sink that lost it fails at the `Pin`, with the
  // sink's own reason, rather than here.
  virtual Status Adopt(const FrameRef& frame) = 0;

  virtual Status Forget(const FrameRef&) = 0;

  // Forgets every frame, and takes the spill tier's copies with it.
  //
  // The counterpart of Adopt, and it exists for the same reason: the tier outlives the process
  // that filled it, while frame identities restart at 1 in the next one. A capture beginning
  // against a tier somebody else's session document still names would write over those frames at
  // the identities it reissues — a read that succeeds with the wrong pixels, which is the one
  // failure the fault-in on Pin cannot catch. So a new session empties the tier and a resumed one
  // does not (ADR 0034).
  //
  // Refused while any frame is pinned: Pin guarantees its mapping until Release, and a span into
  // a freed buffer still looks like a span. Refused, too, when the tier will not let go — with
  // nothing forgotten, so that the caller's choice is between declining to start and starting on
  // a tier it knows is stale, rather than between two states it cannot tell apart.
  //
  // It does not wind the identity counter back. A handle is a plain value that outlives the frame
  // it names; reissuing one would turn a stale handle from dangling into a handle on somebody
  // else's pixels.
  virtual Status Clear() = 0;

  // Which capture the spill tier is holding, as an opaque token.
  //
  // The counterpart of Clear, and the reason it exists is what Clear cannot do: a session
  // document belonging to another project still names the identities a cleared tier is about to
  // reissue, and this store cannot see that document — the manager above it reads documents by
  // project id and the project store has no listing. So the tier says which capture it holds,
  // whoever writes a document records that answer, and a resume compares the two (ADR 0035).
  //
  // Compared for equality and never ordered. A token is minted fresh on every clear rather than
  // counted up, because the tier's own index is a file that can be lost — and a counter that
  // restarted from 1 after losing it would reissue an epoch some surviving document still names,
  // which is the failure at the level above the one this exists to fix.
  //
  // **Zero means there is no durable tier at all** — a store with no sink, which is a desktop or
  // a browser with no origin private file system. That is a real answer rather than an absent
  // one: such a store has no pixels that can outlive it, so no document can be matched to the
  // wrong ones, and a document written against it records zero and matches zero later. What it
  // does not do is make those frames reachable; `Adopt` still refuses, saying there is no tier.
  //
  // Fallible because a sink can be there and unable to answer — a page whose worker script is
  // older than its module. A store that cannot say which capture its tier holds must not have a
  // document written against it, and the manager's checkpoint declines to write one.
  virtual Result<uint64_t> TierGeneration() = 0;

  virtual Result<uint64_t> ContentHash(const FrameRef&) = 0;
};

}  // namespace sphanorama
