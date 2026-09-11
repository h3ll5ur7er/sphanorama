// V6 — what makes a frame the best of its burst.
//
// The numbers this engine produces are not knowable in advance and would be meaningless if they
// were: variance of a Laplacian has no natural unit and its scale depends on contrast and
// resolution. So every test here asserts an *invariant* — an ordering, a floor, a permutation —
// rather than a value. That is the shape the skill asks for when the expected output cannot be
// written down, and it is the shape that survives the algorithm being tuned.
#include <gtest/gtest.h>

#include <limits>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "engines/frame_quality_engine/sharpness_frame_quality_engine.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"

namespace sphanorama {
namespace {

constexpr int32_t kWidth = 64;
constexpr int32_t kHeight = 64;

class FrameQuality : public ::testing::Test {
 protected:
  MemoryFrameStoreAccess store{1 << 22};
  SharpnessFrameQualityEngine engine{store};

  /**
   * An I420 frame: flat luma, and chroma full of edges.
   *
   * The layout is what makes the planar guard matter — a full-resolution luma plane followed by two
   * quarter-resolution chroma planes — so a handle claiming more rows than the picture has reads
   * chroma and calls it detail. Flat luma and busy chroma is the arrangement that makes the
   * difference visible in the score rather than merely present.
   */
  FrameRef PlanarFrame(int32_t width, int32_t height) {
    const Result<FrameRef> allocated = store.Allocate(width, height, PixelFormat::I420);
    EXPECT_TRUE(allocated.ok()) << allocated.status.detail;
    const Result<std::span<uint8_t>> pinned = store.Pin(allocated.value);
    EXPECT_TRUE(pinned.ok()) << pinned.status.detail;
    const size_t luma = static_cast<size_t>(width) * height;
    for (size_t at = 0; at < pinned.value.size(); ++at) {
      pinned.value[at] = at < luma ? 128 : static_cast<uint8_t>(((at / 4) % 2) == 0 ? 16 : 240);
    }
    EXPECT_TRUE(store.Release(allocated.value).ok());
    return allocated.value;
  }

  /** A frame whose luma at (x, y) is whatever `paint` says. RGBA8, so grey means r == g == b. */
  template <typename Paint>
  FrameRef Frame(Paint paint, int32_t width = kWidth, int32_t height = kHeight) {
    auto allocated = store.Allocate(width, height, PixelFormat::RGBA8);
    EXPECT_TRUE(allocated.ok()) << allocated.status.detail;
    auto pinned = store.Pin(allocated.value);
    EXPECT_TRUE(pinned.ok()) << pinned.status.detail;
    for (int32_t y = 0; y < height; ++y) {
      for (int32_t x = 0; x < width; ++x) {
        const uint8_t value = paint(x, y);
        const size_t at = (static_cast<size_t>(y) * width + x) * 4;
        pinned.value[at] = value;
        pinned.value[at + 1] = value;
        pinned.value[at + 2] = value;
        pinned.value[at + 3] = 255;
      }
    }
    EXPECT_TRUE(store.Release(allocated.value).ok());
    return allocated.value;
  }

  FrameRef Flat(uint8_t value = 128) { return Frame([value](int32_t, int32_t) { return value; }); }

  /** A handle that names a real frame and lies about how big it is. */
  static FrameRef Claiming(FrameRef real, int32_t width, int32_t height, int32_t stride) {
    FrameRef lying = real;
    lying.width = width;
    lying.height = height;
    lying.stride = stride;
    return lying;
  }

  /** Hard black-and-white squares: the most sharpness a frame this size can carry. */
  FrameRef Checkerboard(int32_t square = 4) {
    return Frame([square](int32_t x, int32_t y) -> uint8_t {
      return ((x / square) + (y / square)) % 2 == 0 ? 0 : 255;
    });
  }

  /** The same pattern with every edge smeared over `radius` pixels — a blurred photograph. */
  FrameRef SoftCheckerboard(int32_t square = 4) {
    return Frame([square](int32_t x, int32_t y) -> uint8_t {
      // A smooth cosine at the checkerboard's period carries the same structure with no edges.
      const double u = std::cos(3.14159265358979 * x / square);
      const double v = std::cos(3.14159265358979 * y / square);
      return static_cast<uint8_t>(127.5 + 127.0 * u * v);
    });
  }

  double Sharpness(const FrameRef& frame, const NodeContext& context = {}) {
    auto scored = engine.Score(frame, PoseSample{}, context);
    EXPECT_TRUE(scored.ok()) << scored.status.detail;
    return scored.value.sharpness;
  }

  Candidate CandidateOf(uint64_t id, const FrameRef& frame, double sharpness) {
    Candidate candidate;
    candidate.id = CandidateId{id};
    candidate.frame = frame;
    candidate.quality.sharpness = sharpness;
    candidate.quality.exposureAgreement = 1.0;
    return candidate;
  }
};

// ------------------------------------------------------------------ sharpness

TEST_F(FrameQuality, AFrameWithNoDetailHasNoSharpness) {
  // The floor, and it has to be exactly zero rather than small: a flat frame is what a covered
  // lens produces, and it must not out-rank anything.
  EXPECT_DOUBLE_EQ(Sharpness(Flat()), 0.0);
}

TEST_F(FrameQuality, DetailScoresAboveNoDetail) {
  EXPECT_GT(Sharpness(Checkerboard()), Sharpness(Flat()));
}

TEST_F(FrameQuality, ASharpEdgeScoresAboveTheSameStructureSmearedOut) {
  // The whole point of the measure. Both frames carry the same pattern at the same period and
  // roughly the same contrast; one has edges and the other does not. A sharpness measure that
  // could not separate these would rank a burst by nothing.
  EXPECT_GT(Sharpness(Checkerboard()), Sharpness(SoftCheckerboard()));
}

TEST_F(FrameQuality, SharpnessDoesNotDependOnWhereTheDetailSits) {
  // A frame and the same frame shifted by one square. Selection compares frames of the same
  // scene taken moments apart, so a measure that moved with the content would rank hand-shake.
  const FrameRef left = Frame([](int32_t x, int32_t y) -> uint8_t {
    return ((x / 4) + (y / 4)) % 2 == 0 ? 0 : 255;
  });
  const FrameRef shifted = Frame([](int32_t x, int32_t y) -> uint8_t {
    return (((x + 4) / 4) + (y / 4)) % 2 == 0 ? 0 : 255;
  });
  EXPECT_NEAR(Sharpness(left), Sharpness(shifted), Sharpness(left) * 0.05);
}

TEST_F(FrameQuality, ScoringIsDeterministic) {
  // The build graph is keyed on the selection, so an unstable score would invalidate cached
  // stages for no reason.
  const FrameRef frame = Checkerboard();
  EXPECT_DOUBLE_EQ(Sharpness(frame), Sharpness(frame));
}

TEST_F(FrameQuality, AFrameTheStoreCannotProduceIsAFailureRatherThanAZero) {
  // A default QualityScore is what a genuinely terrible frame gets, so a scorer that returned one
  // on failure would make the two indistinguishable — and the manager would bank a cell whose
  // frames nothing ever read.
  FrameRef stranger;
  stranger.id = FrameId{9999};
  stranger.width = kWidth;
  stranger.height = kHeight;
  stranger.format = PixelFormat::RGBA8;
  EXPECT_FALSE(engine.Score(stranger, PoseSample{}, NodeContext{}).ok());
}

TEST_F(FrameQuality, AHandleWhoseOwnArithmeticWouldOverflowIsRefused) {
  // The geometry guard above computes `(height - 1) * stride + rowBytes` and never asks whether
  // that product fits. With `stride <= 0` the fallback step is `width * 4`, bounded by 2^33 rather
  // than by `int32_t`, so `INT32_MAX` rows of it reaches ~1.8e19 and wraps **negative** — and a
  // negative `needed` is smaller than any size, so the guard passes the handle it exists to refuse.
  //
  // Here that is not a near miss. There is no allocator between the bypass and the pointer
  // arithmetic, so `LumaAt` indexes a 16 KB span with a stride of 8,589,934,588 and the process
  // dies. Found by a reviewer of the registration engine, which copied this function's guard —
  // which is why a one-line fix in a file this branch does not otherwise touch is in this commit.
  const FrameRef honest = Frame([](int32_t, int32_t) -> uint8_t { return 128; });
  FrameRef absurd = honest;
  absurd.width = std::numeric_limits<int32_t>::max();
  absurd.height = std::numeric_limits<int32_t>::max();
  absurd.stride = 0;

  const Result<QualityScore> scored = engine.Score(absurd, PoseSample{}, NodeContext{});
  ASSERT_FALSE(scored.ok());
  EXPECT_EQ(scored.status.code, StatusCode::InvalidArgument) << scored.status.detail;
}

TEST_F(FrameQuality, APlanarHandleClaimingChromaIsPictureIsRefused) {
  // The guard that stops this went into `FeatureRegistrationEngine` and not into here, which is the
  // wrong way round: this is the engine that ships, and `CaptureSessionManager::OfferFrame` is
  // `@facade`, so the caller's `FrameRef` reaches it from the page unexamined.
  //
  // It is a wrong number rather than a crash, which is worse for this engine in particular, because
  // this number is what picks which frame of a burst survives. Measured on the frame below, whose
  // luma is flat and whose chroma is full of edges: **0.0 honestly, 8,532.0 through a handle
  // claiming half again as many rows.** Not a drift — a frame with no detail in it reported as the
  // sharpest thing in the burst.
  //
  // An earlier version of this comment carried 102,297 against 38,337, which are numbers from a
  // reviewer's probe on a different frame, quoted here above a test that allocated its frame and
  // never painted it — so the real answer under both handles was 0.000 and the comment described
  // nothing. The commit that wrote it had, in the same diff, rewritten another comment to warn
  // against exactly that.
  const FrameRef honest = PlanarFrame(kWidth, kHeight);
  FrameRef overclaimed = honest;
  overclaimed.height = kHeight + kHeight / 2;

  const Result<QualityScore> scored = engine.Score(overclaimed, PoseSample{}, NodeContext{});
  ASSERT_FALSE(scored.ok());
  EXPECT_EQ(scored.status.code, StatusCode::InvalidArgument) << scored.status.detail;
}

TEST_F(FrameQuality, AFrameWithNoPixelsInItIsRefused) {
  // **This holds the behaviour, not the guard, and the difference is worth stating.** Removing
  // `width <= 0 || height <= 0` from this engine leaves this test passing, because the grid-size
  // rule further down refuses a frame with no pixels anyway and refuses it with the same code. So
  // this is a contract test — a frame with no pixels is an `InvalidArgument`, by whatever route —
  // and it cannot tell you which line did it.
  //
  // The guard is kept regardless, for the same reason as the single-row branch above it: the two
  // engines' guards are kept in step, and its redundancy here rests on an unrelated rule about grid
  // sizes that is not this guard's to depend on. In `FeatureRegistrationEngine` the same line is
  // load-bearing — removing it there turns a refusal a caller can branch on into OpenCV's assertion
  // text arriving as `Internal` — and that is where the sabotage bites.
  //
  // I found this out by sabotaging both engines at once and watching only one test fail, which is
  // the trap this file exists to avoid and which I walked into while closing it.
  const FrameRef source = Frame([](int32_t, int32_t) -> uint8_t { return 128; });
  for (const auto& [width, height] : std::vector<std::pair<int32_t, int32_t>>{
           {0, kHeight}, {kWidth, 0}, {-1, kHeight}, {kWidth, -1}}) {
    FrameRef empty = source;
    empty.width = width;
    empty.height = height;
    const Result<QualityScore> scored = engine.Score(empty, PoseSample{}, NodeContext{});
    ASSERT_FALSE(scored.ok()) << width << "x" << height;
    EXPECT_EQ(scored.status.code, StatusCode::InvalidArgument)
        << width << "x" << height << ": " << scored.status.detail;
    // The *detail*, not only the code, which is what makes this bite after all: both guards refuse
    // with `InvalidArgument`, so the code alone cannot say which answered, and the comment above
    // said this test could not tell them apart. One string does. The precedent is already in this
    // repository — `frame_store_spill_test.cpp` distinguishes two refusals the same way.
    EXPECT_NE(scored.status.detail.find("no pixels"), std::string::npos)
        << "refused, but by the grid-size rule rather than the guard this test is for: "
        << scored.status.detail;
  }
}

TEST_F(FrameQuality, AFrameTooSmallToHoldALaplacianIsRefused) {
  // The rule the guard above is redundant with, and which `grep` finds no test for anywhere. It is
  // not spare: with it disabled a real 2x2 checkerboard is *accepted* and scored 0.0 — the value of
  // the empty sum its own comment says it exists to avoid, and a zero that a selection policy would
  // read as "no detail" rather than "not a photograph".
  const FrameRef tiny = Frame([](int32_t x, int32_t y) -> uint8_t {
    return static_cast<uint8_t>(((x + y) % 2) == 0 ? 0 : 255);
  }, 2, 2);

  const Result<QualityScore> scored = engine.Score(tiny, PoseSample{}, NodeContext{});
  ASSERT_FALSE(scored.ok()) << "a 2x2 frame is not a photograph; scoring it reports an empty sum";
  EXPECT_EQ(scored.status.code, StatusCode::InvalidArgument) << scored.status.detail;
}

TEST_F(FrameQuality, APlanarHandleWithAWideStrideIsRefused) {
  // The packed-rows half of the planar guard, which had no test here while the identical block in
  // `FeatureRegistrationEngine` had two. Deleting it leaves every test in the repository green, and
  // it is load-bearing: measured on the frame below, **0.0 honestly and 26,940.9 through the
  // smuggled handle**.
  //
  // The frame is 640x480 rather than the fixture's 64x64 because at 64x64 this claim is refused by
  // the size bound before the packing check is reached — which would have made this a test of the
  // wrong guard.
  //
  // The two engines' guards are kept in step, which is exactly what hid this — "the fix went into
  // one engine and not the other" became "the test went into one and not the other". Not
  // *identical*: they differ in the `Result` type each returns and in one local's name, so a `diff`
  // between them is not a check anybody can run.
  const FrameRef honest = PlanarFrame(640, 480);
  FrameRef smuggled = honest;
  smuggled.width = 400;
  smuggled.height = 720;
  smuggled.stride = 640;

  const Result<QualityScore> scored = engine.Score(smuggled, PoseSample{}, NodeContext{});
  ASSERT_FALSE(scored.ok()) << "chroma was scored as picture";
  EXPECT_EQ(scored.status.code, StatusCode::InvalidArgument) << scored.status.detail;
}

TEST_F(FrameQuality, AFormatWithNoPixelsToReadIsRefused) {
  // Encoded frames are coming — the frame store's own comment says a JPEG tier is what makes a
  // sphere fit — and scoring one as if it were raster would read the header as image data.
  auto allocated = store.Allocate(kWidth, kHeight, PixelFormat::RGBA8);
  ASSERT_TRUE(allocated.ok());
  FrameRef encoded = allocated.value;
  encoded.format = PixelFormat::EncodedJpeg;
  EXPECT_EQ(engine.Score(encoded, PoseSample{}, NodeContext{}).status.code,
            StatusCode::Unsupported);
}

TEST_F(FrameQuality, GreyFramesAreScoredWithoutNeedingColour) {
  // The browser hands over RGBA today, but a camera port that produced Gray8 or NV12 would be
  // reading luma directly — and luma is all this measure ever wanted.
  auto allocated = store.Allocate(kWidth, kHeight, PixelFormat::Gray8);
  ASSERT_TRUE(allocated.ok());
  auto pinned = store.Pin(allocated.value);
  ASSERT_TRUE(pinned.ok());
  for (size_t i = 0; i < pinned.value.size(); ++i) {
    pinned.value[i] = ((i / 4) + (i / (kWidth * 4))) % 2 == 0 ? 0 : 255;
  }
  ASSERT_TRUE(store.Release(allocated.value).ok());

  auto scored = engine.Score(allocated.value, PoseSample{}, NodeContext{});
  ASSERT_TRUE(scored.ok()) << scored.status.detail;
  EXPECT_GT(scored.value.sharpness, 0.0);
}

// ------------------------------------------------------------------ exposure agreement

TEST_F(FrameQuality, TheFirstFrameOfABurstAgreesWithItselfPerfectly) {
  // Nothing to disagree with. Reporting anything less would penalise the frame that happened to
  // be taken first, which is a property of the burst rather than of the picture.
  auto scored = engine.Score(Checkerboard(), PoseSample{}, NodeContext{});
  ASSERT_TRUE(scored.ok());
  EXPECT_DOUBLE_EQ(scored.value.exposureAgreement, 1.0);
}

TEST_F(FrameQuality, FramesOfTheSameBrightnessAgree) {
  const FrameRef sibling = Flat(128);
  std::vector<Candidate> siblings{CandidateOf(1, sibling, 0.0)};
  NodeContext context;
  context.siblings = siblings;

  auto scored = engine.Score(Flat(128), PoseSample{}, context);
  ASSERT_TRUE(scored.ok());
  EXPECT_GT(scored.value.exposureAgreement, 0.99);
}

TEST_F(FrameQuality, AFrameThatDriftedInBrightnessDisagreesWithItsBurst) {
  // What a burst captured without an exposure lock produces, and the reason the field exists: a
  // frame metered differently from its siblings does not belong in the same blend as them.
  const FrameRef sibling = Flat(40);
  std::vector<Candidate> siblings{CandidateOf(1, sibling, 0.0)};
  NodeContext context;
  context.siblings = siblings;

  auto agreeing = engine.Score(Flat(40), PoseSample{}, context);
  auto drifted = engine.Score(Flat(220), PoseSample{}, context);
  ASSERT_TRUE(agreeing.ok() && drifted.ok());
  EXPECT_LT(drifted.value.exposureAgreement, agreeing.value.exposureAgreement);
}

TEST_F(FrameQuality, ABgraFrameIsReadInItsOwnChannelOrder) {
  // The same picture in the two byte orders has the same luma, so the two frames must agree on
  // exposure. Reading BGRA as if it were RGBA swaps the red and blue coefficients — 0.299 against
  // 0.114 — so a blue frame read the wrong way looks two and a half times brighter than it is.
  //
  // Within one burst every frame comes from one camera in one format, so this could not change a
  // ranking today. It could as soon as OfferFrame puts an externally sourced frame in the same
  // cell as a burst — a file import, a replayed dataset — and then two frames of the same scene
  // would disagree on exposure because of how their bytes were laid out.
  const int32_t width = 32;
  const int32_t height = 32;
  const auto paint = [&](PixelFormat format, uint8_t r, uint8_t g, uint8_t b) {
    auto allocated = store.Allocate(width, height, format);
    EXPECT_TRUE(allocated.ok());
    auto pinned = store.Pin(allocated.value);
    EXPECT_TRUE(pinned.ok());
    for (size_t at = 0; at + 3 < pinned.value.size(); at += 4) {
      // Byte order, not colour order: BGRA puts blue first, so the same visual colour is written
      // with its components swapped.
      pinned.value[at] = format == PixelFormat::BGRA8 ? b : r;
      pinned.value[at + 1] = g;
      pinned.value[at + 2] = format == PixelFormat::BGRA8 ? r : b;
      pinned.value[at + 3] = 255;
    }
    EXPECT_TRUE(store.Release(allocated.value).ok());
    return allocated.value;
  };

  const FrameRef asRgba = paint(PixelFormat::RGBA8, 0, 0, 255);
  const FrameRef asBgra = paint(PixelFormat::BGRA8, 0, 0, 255);
  std::vector<Candidate> siblings{CandidateOf(1, asRgba, 0.0)};
  NodeContext context;
  context.siblings = siblings;

  auto scored = engine.Score(asBgra, PoseSample{}, context);
  ASSERT_TRUE(scored.ok()) << scored.status.detail;
  EXPECT_GT(scored.value.exposureAgreement, 0.999)
      << "the same blue in two byte orders was read as two different brightnesses";
}

// ------------------------------------------------------------------ ranking

TEST_F(FrameQuality, RanksTheSharpestFirst) {
  const FrameRef blurry = SoftCheckerboard();
  const FrameRef sharp = Checkerboard();
  std::vector<Candidate> candidates{
      CandidateOf(1, blurry, Sharpness(blurry)),
      CandidateOf(2, sharp, Sharpness(sharp)),
  };

  auto ranked = engine.Rank(candidates, SelectionPolicy{});
  ASSERT_TRUE(ranked.ok());
  ASSERT_EQ(ranked.value.size(), 2u);
  EXPECT_EQ(ranked.value.front().value, 2u);
}

TEST_F(FrameQuality, RankingIsAPermutationOfWhatItWasGiven) {
  // Not a filter. A ranking that dropped a candidate would silently shrink the evidence pool the
  // retake feature is built on, and one that invented an id would name a frame nothing holds.
  std::vector<Candidate> candidates;
  for (uint64_t id = 1; id <= 5; ++id) {
    candidates.push_back(CandidateOf(id, Flat(static_cast<uint8_t>(50 + id * 20)), id * 1.5));
  }

  auto ranked = engine.Rank(candidates, SelectionPolicy{});
  ASSERT_TRUE(ranked.ok());
  std::vector<uint64_t> got;
  for (const auto& id : ranked.value) got.push_back(id.value);
  std::sort(got.begin(), got.end());
  EXPECT_EQ(got, (std::vector<uint64_t>{1, 2, 3, 4, 5}));
}

TEST_F(FrameQuality, TiesBreakOnIdentitySoTheSameCandidatesBuildTheSameSphere) {
  // Determinism is the property that matters here rather than which of two equals wins: the
  // build graph is keyed on the selection, so a ranking that reordered equal candidates between
  // runs would invalidate cached stages for nothing.
  std::vector<Candidate> candidates{
      CandidateOf(7, Flat(), 4.0), CandidateOf(3, Flat(), 4.0), CandidateOf(5, Flat(), 4.0),
  };

  auto ranked = engine.Rank(candidates, SelectionPolicy{});
  ASSERT_TRUE(ranked.ok());
  ASSERT_EQ(ranked.value.size(), 3u);
  EXPECT_EQ(ranked.value[0].value, 3u);
  EXPECT_EQ(ranked.value[1].value, 5u);
  EXPECT_EQ(ranked.value[2].value, 7u);
}

TEST_F(FrameQuality, ThePolicyDecidesWhatBestMeans) {
  // The reason SelectionPolicy is a parameter rather than a compiled-in weighting. One candidate
  // is sharper, the other agrees better on exposure; which wins is the caller's to say.
  std::vector<Candidate> candidates(2);
  candidates[0].id = CandidateId{1};
  candidates[0].quality.sharpness = 10.0;
  candidates[0].quality.exposureAgreement = 0.0;
  candidates[1].id = CandidateId{2};
  candidates[1].quality.sharpness = 0.0;
  candidates[1].quality.exposureAgreement = 1.0;

  SelectionPolicy sharpnessMatters;
  sharpnessMatters.weightSharpness = 1.0;
  sharpnessMatters.weightExposure = 0.0;
  auto bySharpness = engine.Rank(candidates, sharpnessMatters);
  ASSERT_TRUE(bySharpness.ok());
  EXPECT_EQ(bySharpness.value.front().value, 1u);

  SelectionPolicy exposureMatters;
  exposureMatters.weightSharpness = 0.0;
  exposureMatters.weightExposure = 1.0;
  auto byExposure = engine.Rank(candidates, exposureMatters);
  ASSERT_TRUE(byExposure.ok());
  EXPECT_EQ(byExposure.value.front().value, 2u);
}

TEST_F(FrameQuality, SharpnessIsWeighedAgainstExposureRatherThanDrowningIt) {
  // Sharpness has no natural scale — it is a variance in luma units and runs into the hundreds —
  // while agreement is a fraction. Weighting them as they come would let the units decide instead
  // of the policy: every exposure weight short of a hundred would be arithmetically irrelevant,
  // and `SelectionPolicy` would be a set of knobs where only one of them turned anything.
  //
  // So a frame that is a little less sharp but agrees with its burst beats one that is marginally
  // sharper and does not — which is the trade the default weights describe, and is also the right
  // answer: a frame metered differently from its neighbours shows up as a seam.
  std::vector<Candidate> candidates(2);
  candidates[0].id = CandidateId{1};
  candidates[0].quality.sharpness = 100.0;
  candidates[0].quality.exposureAgreement = 0.0;
  candidates[1].id = CandidateId{2};
  candidates[1].quality.sharpness = 80.0;
  candidates[1].quality.exposureAgreement = 1.0;

  auto ranked = engine.Rank(candidates, SelectionPolicy{});
  ASSERT_TRUE(ranked.ok());
  EXPECT_EQ(ranked.value.front().value, 2u)
      << "raw sharpness swamped the exposure term, so the policy's weights mean nothing";
}

TEST_F(FrameQuality, AFrameRefThatLiesAboutItsSizeIsRefusedRatherThanRead) {
  // A `FrameRef` is a plain value the caller passes in, and `Find` keys on `id` alone — so the
  // geometry on the handle is the *caller's* claim, and nothing upstream makes it describe the
  // allocation. `Pin` hands back the entry's real span; every read below used to index it with the
  // claim.
  //
  // Reproduced by a reviewer under AddressSanitizer: allocate 640x480 RGBA8 honestly, then score
  // the same id claiming `{4096, 4096, stride 16384}` — `heap-buffer-overflow READ 0 bytes after a
  // 1228800-byte region`. `wire::GetInteger` bounds the *cast* of those numbers at the boundary
  // and says nothing about what they describe, which is a different question and the one that
  // matters here.
  //
  // No shipped caller builds a handle by hand today; `OfferFrame` is the door, `Candidates()`
  // publishes every frame id to the client, and the callers that door was written for — file
  // import, replay, a manual shutter — are exactly the ones that would.
  const FrameRef real = Flat(128);

  for (const auto& [label, lying] : {
           std::pair{"width", Claiming(real, kWidth * 8, kHeight, 0)},
           std::pair{"height", Claiming(real, kWidth, kHeight * 8, 0)},
           std::pair{"stride", Claiming(real, kWidth, kHeight, kWidth * 4 * 8)},
           std::pair{"everything", Claiming(real, 4096, 4096, 16384)},
       }) {
    auto scored = engine.Score(lying, PoseSample{}, NodeContext{});
    EXPECT_FALSE(scored.ok()) << "a frame claiming more " << label << " than it has was read";
    if (!scored.ok()) {
      EXPECT_EQ(scored.status.code, StatusCode::InvalidArgument) << label;
    }
  }
}

TEST_F(FrameQuality, AStrideNarrowerThanARowIsRefused) {
  // Rows that overlap are not a frame anybody allocated, and a stride below the row's own width is
  // the only way to ask for one. It reads inside the allocation, so a bounds check alone would let
  // it past — and what it scores is a shear of the picture rather than the picture.
  auto scored = engine.Score(Claiming(Flat(128), kWidth, kHeight, kWidth * 4 - 4),
                             PoseSample{}, NodeContext{});
  EXPECT_FALSE(scored.ok());
  if (!scored.ok()) {
    EXPECT_EQ(scored.status.code, StatusCode::InvalidArgument);
  }
}

TEST_F(FrameQuality, RankingNothingIsNotAFailure) {
  // A cell with no candidates is a cell nobody has captured yet, which the manager asks about
  // routinely. Refusing would make "empty" and "broken" the same answer.
  auto ranked = engine.Rank({}, SelectionPolicy{});
  ASSERT_TRUE(ranked.ok());
  EXPECT_TRUE(ranked.value.empty());
}

}  // namespace
}  // namespace sphanorama
