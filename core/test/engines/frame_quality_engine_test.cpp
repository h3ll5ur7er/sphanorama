// V6 — what makes a frame the best of its burst.
//
// The numbers this engine produces are not knowable in advance and would be meaningless if they
// were: variance of a Laplacian has no natural unit and its scale depends on contrast and
// resolution. So every test here asserts an *invariant* — an ordering, a floor, a permutation —
// rather than a value. That is the shape the skill asks for when the expected output cannot be
// written down, and it is the shape that survives the algorithm being tuned.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

TEST_F(FrameQuality, RankingNothingIsNotAFailure) {
  // A cell with no candidates is a cell nobody has captured yet, which the manager asks about
  // routinely. Refusing would make "empty" and "broken" the same answer.
  auto ranked = engine.Rank({}, SelectionPolicy{});
  ASSERT_TRUE(ranked.ok());
  EXPECT_TRUE(ranked.value.empty());
}

}  // namespace
}  // namespace sphanorama
