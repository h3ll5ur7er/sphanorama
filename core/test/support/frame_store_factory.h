#pragma once

#include <memory>

#include "resource_access/frame_store_access/memory_frame_store_access.h"
#include "sphanorama/resource_access/frame_store_access.h"
#include "support/fake_spill_sink.h"

namespace sphanorama {

// How the contract suite builds a store, kept here rather than beside the implementation because
// what it builds is a *test* arrangement: a small ceiling and a sink that keeps what it is given.
//
// The sink is not optional. A store without one has no spill tier — `Demote` to `Spilled` refuses
// — so a contract suite running against a sinkless store could not exercise the half of the
// contract that is about a frame not being resident. The fixture owns the sink and hands the
// store a pointer to it, which is also the lifetime the real composition roots have.
struct MemoryFrameStoreAccessFactory {
  using Sink = FakeSpillSink;

  static std::unique_ptr<IFrameStoreAccess> Create(Sink* sink) {
    // Small on purpose: the suite's exhaustion cases have to be reachable in a test, and a
    // ceiling nothing can hit is a ceiling nothing tests.
    return std::make_unique<MemoryFrameStoreAccess>(1 << 20, sink);
  }
};

}  // namespace sphanorama
