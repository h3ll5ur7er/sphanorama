// Does a refusal really give back every frame it took? Asked of every allocation the loader makes.
//
// This line read "does a refusal really allocate nothing" until round 6. That was never the
// header's promise and it is not this file's property — a refusal *does* allocate, and the
// string `refuse()` builds is the one allocation the second pass below exists to throw inside
// of. Round 5 corrected the invented quotation in four places and missed the fifth, which is
// the file the other four are about; it escaped the grep by spelling the verb `allocate`.
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
// they started. Not "the function returned" — this translation unit is one of **four** compiled
// `-fexceptions` (the OpenCV-backed registration engine in `core/src`, plus the loader, its gtest
// and this file in `core/test`), and every other consumer of the loader is `-fno-exceptions`, where
// an allocation failure terminates at the throw site whatever any `catch` here would have done.
// This line said "one of only three", written by the commit that made it the fourth.
//
// **The exception safety of `OwnedFrames::Rollback`'s compaction is argued here and tested nowhere**,
// and that is measured rather than assumed. A reviewer instrumented both rollback loops and counted
// the allocations inside them across all 600 loads of both passes, 120 and 480: **zero**. A
// *successful* `MemoryFrameStoreAccess::Forget` allocates nothing, and the rest of that loop is a
// POD assignment and a shrinking `erase`, so no arming of the second throw can land there. What
// the second pass does reach is `refuse()`'s string building — which is why emptying
// `~OwnedFrames` strands frames under it and not under the first pass.
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
// same failure terminates. So those frames are suppressed and everything else stays checked —
// *frames*, plural: one per sweep point that fails an allocation inside `Allocate`, in both
// passes. This line said "the one frame" while the suppressions file it points at had already
// been corrected, which is the third place that correction had to be made.
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
// narrow suppression. It is better than `detect_leaks=0` for every leak that is *not* a frame,
// which is most of this binary — but **not for the reason first given here.** This said "LSan
// is what caught this file's own first defect", and a reviewer showed the narrow form would
// now hide that defect: what an armed cleanup leaks is frames, and every frame's allocation
// stack runs through the suppressed function. Round 7 withdrew that argument in the
// suppressions file and beside `~Armed`, and left it standing here and in `CMakeLists.txt`.
#include "support/synthetic_dataset.h"

#include <cstdio>
#include <cstdlib>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "resource_access/frame_store_access/memory_frame_store_access.h"

namespace {

long gAllocations = 0;
long gThrowAt = -1;
bool gArmed = false;
// How many times the *second* failure actually fired. The second pass asserts this is non-zero, so
// the arrangement is checked rather than assumed.
long gSecondThrows = 0;
// And at how many *distinct* sweep points, which is the number that discriminates. See the verdict
// at the foot of this file: a total alone is satisfied by one point firing repeatedly.
long gSecondThrowPoints = 0;
bool gSecondThrewHere = false;

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
    if (gAllocations == gThrowAt) throw std::bad_alloc();
    if (gAllocations == gThrowAgainAt) {
      // Counted, because an arrangement nothing counts is an arrangement nothing tests. A reviewer
      // removed the second failure entirely — by dropping this branch and by arming `Armed(at)` —
      // and the sweep printed byte-identical output and exited 0, while still reporting "with a
      // second failure during the first's handling". The fix for a test that could not fail could
      // not fail either.
      ++gSecondThrows;
      gSecondThrewHere = true;
      throw std::bad_alloc();
    }
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
  // **Untested, and said so rather than implied.** A reviewer reinstated the exact defect this
  // guard was written for — widening the arming over the cleanup loop below — and the sweep's
  // output was byte-identical, exit 0, same leak set. Nothing today can throw in that loop:
  // `MemoryFrameStoreAccess::Forget` allocates nothing when it succeeds. And LeakSanitizer would
  // not catch it either any more, because what such a defect leaks is *frames*, and this binary
  // suppresses `MemoryFrameStoreAccess::Allocate` — see the suppressions file, which now says so.
  //
  // Kept anyway, and this is the `HeldFrame::gone_` case again: the state is reachable in life the
  // moment a cleanup path allocates, and a guard whose cost is two assignments is not worth trading
  // for a defect that took LSan to find the first time.
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
  std::vector<long> reach(static_cast<size_t>(total) + 1, 0);
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
    // How far this run's allocations actually got, counted while armed. The second pass needs it:
    // a run cut short at `at` makes fewer allocations than the clean load, so `at + gap` can be off
    // the end and no second failure is *reachable* there. Recorded rather than assumed, so the
    // second pass can check itself against what is possible instead of against a chosen number.
    reach[static_cast<size_t>(at)] = gAllocations;

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

  // **Second pass: a failure while the first one is being handled.** Each point of the first sweep
  // is re-run with another allocation failing shortly after, which is how a throw reaches
  // `refuse()`'s string building — the one place inside a handler that allocates, as measured at
  // the top of this file.
  //
  // Its checks are the first pass's, **both** of them. An earlier version claimed "the invariant is
  // the same and so is the arithmetic" while quietly dropping the whole-dataset check, and three
  // separate reviewers caught it the same way: under a partial-`Ok` sabotage the first pass reported
  // 51 and this pass reported 0 — blind to exactly the defect that check exists to see.
  int strandedTwice = 0;
  int wrongTwice = 0;
  long reachablePoints = 0;
  for (long at = 1; at <= total; ++at) {
    gSecondThrewHere = false;
    // The nearest gap this run can actually reach. If even `at + 1` is past where this run's
    // allocations stop, no second failure is possible here and this point is not held against the
    // sweep below.
    if (at + 1 <= reach[static_cast<size_t>(at)]) ++reachablePoints;
    // **The gap bound is derived, not chosen.** It was `gap <= 4`, and a reviewer measured that
    // `reach[at] - at` is exactly 3 at every reachable point, so the fourth gap was the first pass
    // run again — 120 of 480 arrangements doing nothing the first pass had not already done, while
    // the loop advertised four. Reading the bound off each run's own reach covers exactly the gaps
    // that can carry a second failure and follows the loader if that distance ever changes. The
    // same fact the gate below uses, applied one level up.
    for (long gap = 1; at + gap <= reach[static_cast<size_t>(at)]; ++gap) {
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
      if (taken && taken->frames.size() != gFramesInACleanLoad) {
        std::fprintf(stderr,
                     "allocations %ld and %ld: the load succeeded with %zu frames, and a clean "
                     "load gives %zu\n",
                     at, at + gap, taken->frames.size(), gFramesInACleanLoad);
        ++wrongTwice;
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
    if (gSecondThrewHere) ++gSecondThrowPoints;
  }

  // On stderr, not stdout, and not by taste: LeakSanitizer ends the process without flushing
  // stdio, so a sweep whose verdict went to a buffer reported nothing at all under ASan — which is
  // where this file's own first defect was found. (This block was in the file twice, verbatim: a
  // round pasted a copy, the next round's fix for that pasted a third, and a reviewer counted.)
  std::fprintf(stderr,
               "swept %ld allocation points of a full load; %d stranded, %d partial datasets "
               "returned as successes. With a second failure during the first's handling "
               "(%ld fired, at %ld of the %ld points where one was reachable): %d stranded, "
               "%d partial\n",
               total, stranded, wrong, gSecondThrows, gSecondThrowPoints, reachablePoints,
               strandedTwice, wrongTwice);
  // **The gate is the number of *points*, and it is measured against what was reachable rather
  // than against a number anyone chose.** `gSecondThrows > 0` was the first version of this check
  // and a reviewer defeated it by arming the second failure at a single sweep point: three firings
  // instead of hundreds, 477 of the 480 arrangements silently disarmed, and the sweep still exited
  // 0 while printing "with a second failure during the first's handling". A threshold of one does
  // not tell a sweep from a spot check.
  //
  // Not every point *can* fire one. A run cut short at `at` makes fewer allocations than the clean
  // load `total` was sized from — the same fact `Armed`'s docstring is about — so for a tail of
  // values `at + 1` is already past the end and there is nothing to arm. That tail is counted from
  // the first pass's own measurements rather than guessed at, which is why this is an equality and
  // not a floor with a fraction in it. This file bans written-down denominators for the reason
  // that they go stale; a count the instrument takes each run cannot.
  const bool sweptEveryReachablePoint = gSecondThrowPoints == reachablePoints;
  if (!sweptEveryReachablePoint) {
    std::fprintf(stderr,
                 "the second failure fired at %ld sweep points and %ld could have carried one, so "
                 "the difference was the first pass run again\n",
                 gSecondThrowPoints, reachablePoints);
  }
  return stranded == 0 && wrong == 0 && strandedTwice == 0 && wrongTwice == 0 &&
                 sweptEveryReachablePoint
             ? 0
             : 1;
}
