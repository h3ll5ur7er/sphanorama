// Does a refusal really allocate nothing? Asked of every allocation the loader makes.
//
// `synthetic_dataset.h` promises that a failed load gives back every frame it took. Round 1
// believed that and was wrong twice, and neither window was reachable from the gtest suite: both
// needed an allocation to fail at a precisely chosen moment, which no fake store can arrange
// because the allocation is `std::vector`'s and not the store's. Three reviewers each built a
// throwing `operator new` by hand to find them. This is that instrument, kept.
//
// Its own binary, because replacing global `operator new` is not something to do to a process that
// 700 other tests share. Not gtest, for the same reason and one more: gtest allocates while it
// works, so the sweep's own counter would move under it.
//
// **The property asserted is the store's accounting**: for every allocation point, the load may
// return a `Result` or leave by throwing, and either way the store's totals must be exactly where
// they started. Not "the function returned" — this translation unit is one of only three compiled
// `-fexceptions`, and every other consumer of the loader is `-fno-exceptions`, where an allocation
// failure terminates at the throw site whatever any `catch` here would have done.
//
// **LeakSanitizer stays on, with one suppression**, in
// `support/dataset_alloc_test.lsan-suppressions`. With nothing suppressed, the sweep reports its own
// totals clean and LSan still finds frames unfreed: the throw lands on the red-black-tree node
// allocation inside `entries_.emplace` in `MemoryFrameStoreAccess::Allocate`, and the block left
// behind is the local `Entry`'s pixel buffer allocated a line earlier. That function is compiled
// `-fno-exceptions` (ADR 0012), so GCC emits no cleanup landing pads and the throw never runs that
// destructor.
//
// That is a fact about this instrument, not about the loader: making an allocation fail *inside*
// code that has opted out of exceptions is not something the real program can do, since there the
// same failure terminates. So the one frame is suppressed and everything else stays checked.
//
// **Two corrections, both from reviewers, both about how the first version of this comment was
// measured.** It said "one 6,912-byte frame … allocation 82 of 119", which was read off a bisect
// against an earlier state of this file; the committed binary strands one frame *per dataset frame*
// at several sweep points. And it named the tree node as the leaked block, conflating where the
// throw fires with what it orphans. The count is deliberately not restated here — see
// `OwnedFrames::Reserve` for why a number in a comment about this instrument can only go stale.
//
// It also said `detect_leaks=0`, which was process-wide on the only test that reaches these arms —
// so a real leak planted anywhere in this binary would have been green. A reviewer demonstrated the
// narrow suppression, and it is strictly better: LSan is what caught this file's own first defect.
#include "support/synthetic_dataset.h"

#include <cstdio>
#include <cstdlib>
#include <new>
#include <optional>
#include <string>
#include <utility>

#include "resource_access/frame_store_access/memory_frame_store_access.h"

namespace {

long gAllocations = 0;
long gThrowAt = -1;
bool gArmed = false;
// A second failure, so one can land *inside* a handler. Until this existed the sweep armed a single
// exact allocation, so nothing ever failed while `refuse()` was building its message or
// `Rollback()` was giving frames back — which is the one path where `~OwnedFrames` is not already
// preceded by an explicit rollback, and therefore the only thing that makes that destructor
// reachable at all. A reviewer filed both halves: that the sweep cannot reach handlers, and that
// the destructor's body can be emptied with the whole suite green.
long gThrowAgainAt = -1;

// How many frames a clean load of the fixture hands back. Measured by the counting pass rather than
// written down as 4, so the sweep's "a success must be whole" check cannot drift from the fixture.
size_t gFramesInACleanLoad = 0;

}  // namespace

void* operator new(size_t bytes) {
  if (gArmed) {
    ++gAllocations;
    if (gAllocations == gThrowAt || gAllocations == gThrowAgainAt) throw std::bad_alloc();
  }
  void* memory = std::malloc(bytes != 0 ? bytes : 1);
  if (memory == nullptr) throw std::bad_alloc();
  return memory;
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void* operator new[](size_t bytes) { return operator new(bytes); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory, size_t) noexcept { std::free(memory); }

namespace {

using namespace sphanorama;

/**
 * Arms the throw, and disarms on every way out — including the one that throws.
 *
 * Written as a guard after the first version disarmed with a plain assignment *after* its own
 * cleanup loop, which leaked the very frames the sweep exists to notice. A sweep iteration can make
 * fewer allocations than the clean load it was sized from, so for the last few values of `at` the
 * throw never fires and the load succeeds — and the cleanup that follows was then still armed, so
 * the throw landed inside `Forget` and four frames were never given back. LeakSanitizer caught it;
 * the sweep's own accounting did not, because the store's totals had already been corrected for the
 * one frame it did forget.
 *
 * Which is this repository's recurring lesson arriving in a new place: the defect was in the code
 * written to catch the defect.
 */
class Armed {
 public:
  explicit Armed(long at, long again = -1) {
    gAllocations = 0;
    gThrowAt = at;
    gThrowAgainAt = again;
    gArmed = true;
  }
  ~Armed() {
    gArmed = false;
    gThrowAgainAt = -1;
  }
  Armed(const Armed&) = delete;
  Armed& operator=(const Armed&) = delete;
};

const char* kDataset = SPHANORAMA_TEST_DATA_DIR "/synthetic-ring-4";

int64_t HeapUsed(IFrameStoreAccess& store) {
  const Result<FrameStoreBudget> budget = store.Budget();
  return budget.ok() ? budget.value.heapUsedBytes : -1;
}

/** How many allocations one clean load makes, so the sweep knows where to stop. */
long CountAllocationsOfACleanLoad() {
  MemoryFrameStoreAccess store(64 * 1024 * 1024);
  std::optional<SyntheticDataset> taken;
  std::string refusal;
  long counted = 0;
  {
    Armed armed(-1);
    Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, kDataset);
    counted = gAllocations;
    if (loaded.ok()) {
      taken = std::move(loaded.value);
    } else {
      refusal = loaded.status.detail;
    }
  }
  if (!taken) {
    std::fprintf(stderr, "the fixture does not load at all: %s\n", refusal.c_str());
    return -1;
  }
  gFramesInACleanLoad = taken->frames.size();
  for (const SyntheticFrame& frame : taken->frames) {
    const Status given = store.Forget(frame.frame);
    if (!given.ok()) {
      std::fprintf(stderr, "the clean load's own frames could not be given back: %s\n",
                   given.detail.c_str());
      return -1;
    }
  }
  const int64_t left = HeapUsed(store);
  if (left != 0) {
    std::fprintf(stderr, "the clean load left %lld bytes behind before the sweep even began\n",
                 static_cast<long long>(left));
    return -1;
  }
  return counted;
}

}  // namespace

int main() {
  const long total = CountAllocationsOfACleanLoad();
  if (total <= 0) return 1;

  int stranded = 0;
  int wrong = 0;
  for (long at = 1; at <= total; ++at) {
    MemoryFrameStoreAccess store(64 * 1024 * 1024);
    const int64_t before = HeapUsed(store);

    bool escaped = false;
    std::optional<SyntheticDataset> taken;
    {
      Armed armed(at);
      try {
        Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, kDataset);
        if (loaded.ok()) taken = std::move(loaded.value);
      } catch (const std::bad_alloc&) {
        escaped = true;
      }
    }
    // Disarmed before anything else touches the store. The frames of a load that succeeded are the
    // caller's, and giving them back is the caller's job — done here, unarmed, so this cleanup
    // cannot be the thing that fails.
    const bool succeeded = taken.has_value();
    if (succeeded) {
      // **A success must be a whole dataset.** The store's totals cannot tell a complete load from
      // a partial one — three frames of a four-frame ring balance just as neatly as four — so
      // without this the sweep is green when the loader hands back a truncated dataset as `Ok`,
      // which `OwnedFrames`'s own docstring calls worse than a refusal. A reviewer demonstrated it
      // by replacing the traversal's refusal with `Commit(); return Ok(...)`: 43 gtest tests and
      // every sweep point stayed green.
      if (taken->frames.size() != gFramesInACleanLoad) {
        std::fprintf(stderr,
                     "allocation %ld: the load succeeded with %zu frames, and a clean load gives "
                     "%zu\n",
                     at, taken->frames.size(), gFramesInACleanLoad);
        ++wrong;
      }
      for (const SyntheticFrame& frame : taken->frames) (void)store.Forget(frame.frame);
    }

    const int64_t after = HeapUsed(store);
    if (after != before) {
      ++stranded;
      std::fprintf(stderr, 
          "allocation %ld of %ld: the store kept %lld bytes after a load that %s\n", at, total,
          static_cast<long long>(after - before),
          escaped ? "left by throwing" : (succeeded ? "succeeded" : "refused"));
    }
  }

  // On stderr, not stdout, and not by taste: LeakSanitizer ends the process without flushing
  // stdio, so a sweep whose verdict went to a buffer reported nothing at all under ASan — which is
  // where this file's own first defect was found.
  // **Second pass: a failure while the first one is being handled.** Each point of the first sweep
  // is re-run with another allocation failing shortly after, which is how the throw reaches
  // `refuse()`'s string building and `Rollback()`'s loop. The invariant is the same and so is the
  // arithmetic; only the arming differs.
  int strandedTwice = 0;
  for (long at = 1; at <= total; ++at) {
    for (long gap = 1; gap <= 4; ++gap) {
      MemoryFrameStoreAccess store(64 * 1024 * 1024);
      const int64_t before = HeapUsed(store);
      std::optional<SyntheticDataset> taken;
      {
        Armed armed(at, at + gap);
        try {
          Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, kDataset);
          if (loaded.ok()) taken = std::move(loaded.value);
        } catch (const std::bad_alloc&) {
          // Expected here rather than exceptional: a throw inside a handler has nowhere to be
          // converted, and the point of this pass is that the store is still clean afterwards.
        }
      }
      if (taken) {
        for (const SyntheticFrame& frame : taken->frames) (void)store.Forget(frame.frame);
      }
      if (HeapUsed(store) != before) {
        ++strandedTwice;
        std::fprintf(stderr, "allocations %ld and %ld: the store kept %lld bytes\n", at, at + gap,
                     static_cast<long long>(HeapUsed(store) - before));
      }
    }
  }

  // On stderr, not stdout, and not by taste: LeakSanitizer ends the process without flushing
  // stdio, so a sweep whose verdict went to a buffer reported nothing at all under ASan — which is
  // where this file's own first defect was found.
  std::fprintf(stderr,
               "swept %ld allocation points of a full load; %d stranded, %d partial datasets "
               "returned as successes; %d stranded with a second failure during the first's "
               "handling\n",
               total, stranded, wrong, strandedTwice);
  return stranded == 0 && wrong == 0 && strandedTwice == 0 ? 0 : 1;
}
