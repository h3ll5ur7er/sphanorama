// The loader that joins the harness's two halves, and the tests that say it joined them.
//
// `tools/synth_dataset.py` renders frames and writes the rotation each was taken at;
// `support/rotation_scoring` says how wrong a set of estimated rotations is. Until this file
// nothing in C++ read the first, so the two halves had never met and the accuracy number Phase 2
// exits on could not be computed at all.
#include "support/synthetic_dataset.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "resource_access/frame_store_access/memory_frame_store_access.h"

namespace sphanorama {
namespace {

namespace fs = std::filesystem;

/** The dataset the real generator wrote, committed so the loader is read against its own writer. */
std::string Fixture() { return std::string(SPHANORAMA_TEST_DATA_DIR) + "/synthetic-ring-4"; }

/**
 * A scratch copy of the fixture, for the cases that need it damaged.
 *
 * Copied rather than regenerated: the point of every refusal below is what the loader does with
 * bytes that are *nearly* right, and the nearest thing to nearly-right is the real dataset with one
 * thing wrong with it.
 */
class Scratch {
 public:
  Scratch() {
    // Not `std::tmpnam`: it may answer `nullptr`, and `fs::path(nullptr)` is undefined behaviour
    // rather than an error anybody would see. A counter is enough here — these directories live
    // inside one test process and are removed by the destructor below.
    static int serial = 0;
    root_ = fs::temp_directory_path() /
            ("sphanorama-dataset-" + std::to_string(serial++) + "-" +
             std::to_string(static_cast<long long>(::getpid())));
    fs::remove_all(root_);
    fs::create_directories(root_);
    try {
      fs::copy(Fixture(), root_, fs::copy_options::recursive);
    } catch (...) {
      // A constructor that throws gets no destructor, so the directory it had already made would
      // outlive the run. `fs::copy` throws — this translation unit is `-fexceptions` precisely
      // because of that — so the window is real and one line wide.
      std::error_code ignored;
      fs::remove_all(root_, ignored);
      throw;
    }
  }
  ~Scratch() { std::error_code ignored; fs::remove_all(root_, ignored); }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;

  std::string path() const { return root_.string(); }
  fs::path file(const std::string& name) const { return root_ / name; }

 private:
  fs::path root_;
};

/**
 * A frame's payload, read by an independent parser, so the loader is checked and not echoed.
 *
 * **Loud on failure, because it was silent and that mattered.** Pointed at a name that is not there
 * it used to hand back `w = h = 0` and an empty vector, and the three refusal cases built from its
 * output then wrote headers like `P6\n-1 0\n255\n` — refused, but by guards they were not written
 * for. A reviewer found it by breaking the path and watching only one test fail.
 */
std::vector<uint8_t> PayloadOf(const fs::path& ppm, int32_t* width, int32_t* height) {
  std::ifstream in(ppm, std::ios::binary);
  EXPECT_TRUE(in.good()) << "no such frame to read: " << ppm;
  std::string magic;
  int w = 0;
  int h = 0;
  int maxValue = 0;
  in >> magic >> w >> h >> maxValue;
  EXPECT_EQ(magic, "P6") << ppm;
  EXPECT_GT(w, 0) << ppm;
  EXPECT_GT(h, 0) << ppm;
  EXPECT_EQ(maxValue, 255) << ppm;
  in.get();   // the single whitespace byte after the maximum, which the payload starts after
  *width = w;
  *height = h;
  std::vector<uint8_t> bytes(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
  in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  EXPECT_EQ(in.gcount(), static_cast<std::streamsize>(bytes.size()))
      << "the fixture this test builds on is itself short: " << ppm;
  return bytes;
}

/**
 * A store that forwards to a real one and can be told to refuse, or to lie about stride.
 *
 * Thirteen refusal branches in the loader were reached by no test at all — a reviewer put
 * `std::abort()` in each and the suite passed, exit 0. Two of them carry the weight: `Allocate`
 * refusing is the only thing in life that makes `OwnedFrames` matter, and the stride lie is the one
 * the span guard exists for. Neither is reachable through `MemoryFrameStoreAccess`, which is why
 * they went untested and why this exists.
 */
class AwkwardStore final : public IFrameStoreAccess {
 public:
  explicit AwkwardStore(IFrameStoreAccess& real) : real_(real) {}

  int refuseAllocateAfter = -1;   // -1 never refuses
  int refusePinAfter = -1;
  int refuseReleaseAfter = -1;
  // Refuses the first N releases and then behaves. Distinct from `refuseReleaseAfter`, which
  // refuses for ever: a store that declines once and relents leaves nothing behind, and a refusal
  // message claiming otherwise is wrong for exactly that case.
  int refuseReleaseTimes = 0;
  int refuseForgetAfter = -1;
  // Refuses the first N forgets and then behaves — the counterpart of `refuseReleaseTimes`, and the
  // only way to see whether a rollback retries what the store declined or simply drops it.
  int refuseForgetTimes = 0;
  // Signed on purpose. A positive pad makes the handle claim a *wider* row than the span holds, and
  // a negative one makes it claim a narrower one — which is the quieter failure of the two: the
  // write loop then overlaps its rows and produces a sheared frame instead of crashing.
  int32_t padStrideBy = 0;
  // Bytes withheld from the pinned span, so the store can hand back less than a frame of this shape
  // needs. Nothing else can reach the guard's one-row arm, where the row itself is the whole bound.
  size_t shortenPinBy = 0;

  Result<FrameRef> Allocate(int32_t width, int32_t height, PixelFormat format) override {
    if (refuseAllocateAfter >= 0 && allocations_++ >= refuseAllocateAfter) {
      return Err<FrameRef>(StatusCode::FrameStoreExhausted, "AwkwardStore", "refused on purpose");
    }
    Result<FrameRef> allocated = real_.Allocate(width, height, format);
    if (allocated.ok()) allocated.value.stride += padStrideBy;
    return allocated;
  }
  Result<std::span<uint8_t>> Pin(const FrameRef& frame) override {
    if (refusePinAfter >= 0 && pins_++ >= refusePinAfter) {
      return Err<std::span<uint8_t>>(StatusCode::Internal, "AwkwardStore", "refused on purpose");
    }
    FrameRef honest = frame;
    honest.stride -= padStrideBy;
    Result<std::span<uint8_t>> pinned = real_.Pin(honest);
    if (pinned.ok() && shortenPinBy > 0 && pinned.value.size() > shortenPinBy) {
      pinned.value = pinned.value.first(pinned.value.size() - shortenPinBy);
    }
    return pinned;
  }
  Status Release(const FrameRef& frame) override {
    if (refuseReleaseTimes > 0) {
      --refuseReleaseTimes;
      return Fail(StatusCode::Internal, "AwkwardStore", "refused on purpose, once");
    }
    if (refuseReleaseAfter >= 0 && releases_++ >= refuseReleaseAfter) {
      return Fail(StatusCode::Internal, "AwkwardStore", "refused on purpose");
    }
    FrameRef honest = frame;
    honest.stride -= padStrideBy;
    return real_.Release(honest);
  }
  Status Forget(const FrameRef& frame) override {
    if (refuseForgetTimes > 0) {
      --refuseForgetTimes;
      return Fail(StatusCode::FailedPrecondition, "AwkwardStore", "refused on purpose, once");
    }
    if (refuseForgetAfter >= 0 && forgets_++ >= refuseForgetAfter) {
      return Fail(StatusCode::FailedPrecondition, "AwkwardStore", "refused on purpose");
    }
    FrameRef honest = frame;
    honest.stride -= padStrideBy;
    return real_.Forget(honest);
  }
  Result<FrameStoreBudget> Budget() override { return real_.Budget(); }
  Result<Residency> ResidencyOf(const FrameRef& frame) override { return real_.ResidencyOf(frame); }
  Status Demote(const FrameRef& frame, Residency target) override {
    return real_.Demote(frame, target);
  }
  Status Adopt(const FrameRef& frame) override { return real_.Adopt(frame); }
  Status Clear() override { return real_.Clear(); }
  Result<uint64_t> TierGeneration() override { return real_.TierGeneration(); }
  Result<uint64_t> ContentHash(const FrameRef& frame) override { return real_.ContentHash(frame); }

 private:
  IFrameStoreAccess& real_;
  int allocations_ = 0;
  int pins_ = 0;
  int releases_ = 0;
  int forgets_ = 0;
};

/**
 * A refusal that names itself.
 *
 * Asserting the code alone proves that *something* refused, and this loader has a dozen ways to
 * answer `InvalidArgument`. A reviewer deleted eleven of its guards one at a time and the suite
 * stayed exit 0 every time, because the next guard downstream produced the same pair — nine of the
 * tests that could not fail were added by round 1, which is what makes this a pattern rather than
 * an oversight. Every refusal in the loader now carries a phrase only its own guard writes, and
 * every test below asserts that phrase.
 */
::testing::AssertionResult RefusedWith(const Result<SyntheticDataset>& loaded, StatusCode code,
                                       const std::string& phrase) {
  if (loaded.ok()) return ::testing::AssertionFailure() << "the load succeeded";
  if (loaded.status.code != code) {
    return ::testing::AssertionFailure()
           << "refused with code " << static_cast<int>(loaded.status.code) << " rather than "
           << static_cast<int>(code) << "; it said: " << loaded.status.detail;
  }
  if (loaded.status.detail.find(phrase) == std::string::npos) {
    return ::testing::AssertionFailure()
           << "the right code came from the wrong guard.\n  it said: " << loaded.status.detail
           << "\n  expected to contain: " << phrase;
  }
  return ::testing::AssertionSuccess();
}

class Dataset : public ::testing::Test {
 protected:
  MemoryFrameStoreAccess store{1 << 24};

  int64_t HeapUsed() {
    const Result<FrameStoreBudget> budget = store.Budget();
    EXPECT_TRUE(budget.ok());
    return budget.value.heapUsedBytes;
  }

  void ForgetAll(const SyntheticDataset& dataset) {
    for (const SyntheticFrame& frame : dataset.frames) {
      EXPECT_TRUE(store.Forget(frame.frame).ok());
    }
  }
};

/** The fixture's `truth.json`, with one thing about it changed. */
void WriteTruth(const Scratch& scratch, const std::string& json) {
  std::ofstream out(scratch.file("truth.json"), std::ios::binary | std::ios::trunc);
  out << json;
}

TEST_F(Dataset, ReadsEveryFrameAndTheLensThatMadeThem) {
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, Fixture());
  ASSERT_TRUE(loaded.ok()) << loaded.status.detail;
  ASSERT_EQ(loaded.value.frames.size(), 4U);

  // The generator's own numbers, not round ones: a fixture whose lens was all zeroes and whose
  // rotations were all identity would be read correctly by a loader that returned a default.
  EXPECT_DOUBLE_EQ(loaded.value.lens.fx, 36.95675913154999);
  EXPECT_DOUBLE_EQ(loaded.value.lens.fy, 38.60112456917206);
  EXPECT_DOUBLE_EQ(loaded.value.lens.cx, 24.0);
  EXPECT_DOUBLE_EQ(loaded.value.lens.cy, 18.0);
  EXPECT_EQ(loaded.value.lens.width, 48);
  EXPECT_EQ(loaded.value.lens.height, 36);
  // Not written by the generator, and left at the value that means "not known" rather than invented.
  EXPECT_DOUBLE_EQ(loaded.value.lens.rollingShutterLineTimeNs, 0.0);
  EXPECT_FALSE(loaded.value.lens.estimated) << "these are the true intrinsics, not an estimate";

  ForgetAll(loaded.value);
}

TEST_F(Dataset, RecordsTheRotationTheFileGivesRatherThanOneItPrefers) {
  // A ring of four is a quarter turn a frame about +Y, and the fourth is written with a *negative*
  // scalar part — the double cover, the same rotation spelled the other way. A loader that tidied
  // that into a positive `w` would be doing unasked-for work on the one field the whole accuracy
  // measurement is compared against, and `ScoreRotations` handles the sign itself.
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, Fixture());
  ASSERT_TRUE(loaded.ok()) << loaded.status.detail;
  ASSERT_EQ(loaded.value.frames.size(), 4U);

  const double half = 0.7071067811865476;
  const std::vector<Quat> expected = {
      {1.0, 0.0, 0.0, 0.0}, {half, 0.0, half, 0.0}, {0.0, 0.0, 1.0, 0.0}, {-half, 0.0, half, 0.0}};
  for (size_t at = 0; at < expected.size(); ++at) {
    const Quat& got = loaded.value.frames[at].trueRotation;
    EXPECT_NEAR(got.w, expected[at].w, 1e-9) << "frame " << at << " w";
    EXPECT_NEAR(got.x, expected[at].x, 1e-9) << "frame " << at << " x";
    EXPECT_NEAR(got.y, expected[at].y, 1e-9) << "frame " << at << " y";
    EXPECT_NEAR(got.z, expected[at].z, 1e-9) << "frame " << at << " z";
  }
  ForgetAll(loaded.value);
}

TEST_F(Dataset, ThePixelsInTheStoreAreThePixelsOnDisk) {
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, Fixture());
  ASSERT_TRUE(loaded.ok()) << loaded.status.detail;
  ASSERT_FALSE(loaded.value.frames.empty());

  int32_t width = 0;
  int32_t height = 0;
  const std::vector<uint8_t> onDisk =
      PayloadOf(fs::path(Fixture()) / "frame_0000.ppm", &width, &height);
  ASSERT_EQ(width, 48);
  ASSERT_EQ(height, 36);

  const FrameRef frame = loaded.value.frames[0].frame;
  EXPECT_EQ(frame.width, width);
  EXPECT_EQ(frame.height, height);
  EXPECT_EQ(frame.format, PixelFormat::RGBA8) << "the four-channel path a camera port produces";

  const Result<std::span<uint8_t>> pinned = store.Pin(frame);
  ASSERT_TRUE(pinned.ok()) << pinned.status.detail;
  // Row for row and channel for channel, against a parser that shares no code with the loader.
  // Counting differing bytes rather than asserting per pixel keeps a failure readable.
  int64_t wrong = 0;
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      const uint8_t* px = pinned.value.data() + static_cast<size_t>(y) * frame.stride + x * 4;
      const size_t at = (static_cast<size_t>(y) * width + x) * 3;
      if (px[0] != onDisk[at] || px[1] != onDisk[at + 1] || px[2] != onDisk[at + 2]) ++wrong;
      if (px[3] != 255) ++wrong;
    }
  }
  EXPECT_EQ(wrong, 0) << wrong << " of " << width * height << " pixels differ from the file";
  EXPECT_TRUE(store.Release(frame).ok());
  ForgetAll(loaded.value);
}

TEST_F(Dataset, RefusesADirectoryThatIsNotThere) {
  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, Fixture() + "-does-not-exist");
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::NotFound, "no such dataset directory"));
  EXPECT_EQ(HeapUsed(), before);
}

TEST_F(Dataset, RefusesADatasetWithNoTruthToDescribeIt) {
  Scratch scratch;
  fs::remove(scratch.file("truth.json"));
  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::NotFound, "no truth.json in"));
  EXPECT_EQ(HeapUsed(), before);
}

TEST_F(Dataset, RefusesAFrameTheTruthNamesAndTheDirectoryDoesNotHold) {
  // The half-written dataset `write_dataset` goes to some trouble to make impossible. Worth
  // refusing anyway: the generator's staging dance protects its own writes, not a directory
  // somebody edited afterwards.
  Scratch scratch;
  fs::remove(scratch.file("frame_0002.ppm"));
  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::NotFound, "cannot open"));
  EXPECT_EQ(HeapUsed(), before)
      << "the two frames it had already read were left in the store by a refusal";
}

TEST_F(Dataset, RefusesAFrameWhoseHeaderDisagreesWithTheLens) {
  // The one disagreement that is silent otherwise: every frame in a dataset is rendered through one
  // lens, so a frame of a different size is either a stale file or a lens that is not the one that
  // made it — and reading it against the recorded intrinsics would put every feature in the wrong
  // place while looking perfectly well-formed.
  Scratch scratch;
  int32_t width = 0;
  int32_t height = 0;
  const std::vector<uint8_t> payload = PayloadOf(scratch.file("frame_0001.ppm"), &width, &height);
  std::ofstream out(scratch.file("frame_0001.ppm"), std::ios::binary | std::ios::trunc);
  out << "P6\n" << (width - 1) << " " << height << "\n255\n";
  out.write(reinterpret_cast<const char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
  out.close();

  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  // "and truth.json records a", not "truth.json records a": the shorter phrase is also written by
  // the no-pixels guard, and it was the one assertion out of twenty-five whose phrase two guards
  // could produce — so `RefusedWith`'s whole promise did not hold for it.
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "and truth.json records a"));
  EXPECT_EQ(HeapUsed(), before);
}

TEST_F(Dataset, RefusesAFrameWithFewerPixelsThanItsHeaderPromises) {
  Scratch scratch;
  int32_t width = 0;
  int32_t height = 0;
  const std::vector<uint8_t> payload = PayloadOf(scratch.file("frame_0003.ppm"), &width, &height);
  std::ofstream out(scratch.file("frame_0003.ppm"), std::ios::binary | std::ios::trunc);
  out << "P6\n" << width << " " << height << "\n255\n";
  // One row short. A truncated read that went unnoticed would leave the last row as whatever the
  // allocation held, which is zeroes — a black stripe that a detector will happily find edges along.
  out.write(reinterpret_cast<const char*>(payload.data()),
            static_cast<std::streamsize>(payload.size() - static_cast<size_t>(width) * 3));
  out.close();

  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "ends before the pixels its header promises"));
  EXPECT_EQ(HeapUsed(), before);
}

TEST_F(Dataset, RefusesAFrameWithMorePixelsThanItsHeaderPromises) {
  // Strict in both directions, and the asymmetry would be the dangerous one: a file with bytes to
  // spare is not the frame its header describes, and reading the first `w * h * 3` of it silently
  // succeeds while quietly picking a different picture than the one on disk.
  Scratch scratch;
  int32_t width = 0;
  int32_t height = 0;
  const std::vector<uint8_t> payload = PayloadOf(scratch.file("frame_0002.ppm"), &width, &height);
  std::ofstream out(scratch.file("frame_0002.ppm"), std::ios::binary | std::ios::trunc);
  out << "P6\n" << width << " " << height << "\n255\n";
  out.write(reinterpret_cast<const char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
  out.put('\n');   // one byte more than the header accounts for
  out.close();

  // The heap, like every other refusal here. This was the one case without the assertion, and the
  // one that had two frames already read when it failed — so it was passing on a 13,824-byte leak.
  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "carries more bytes than its header accounts for"));
  EXPECT_EQ(HeapUsed(), before);
}

TEST_F(Dataset, RefusesAFileThatIsNotANetpbmAtAll) {
  // **With a full payload**, which is the whole of this test. The first version wrote a P5 header and
  // no pixels, so the short-read guard refused it and the magic-number check was never reached — a
  // reviewer widened the check to accept P5 and all eleven tests stayed green. A greyscale file that
  // is otherwise well-formed is the input that tells the two apart, and a loader that accepted it
  // would read one channel's worth of luma as interleaved colour.
  Scratch scratch;
  int32_t width = 0;
  int32_t height = 0;
  const std::vector<uint8_t> payload = PayloadOf(scratch.file("frame_0000.ppm"), &width, &height);
  std::ofstream out(scratch.file("frame_0000.ppm"), std::ios::binary | std::ios::trunc);
  out << "P5\n" << width << " " << height << "\n255\n";
  out.write(reinterpret_cast<const char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
  out.close();

  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "is not a P6 Netpbm"));
  EXPECT_EQ(HeapUsed(), before);
}

// ---------------------------------------------------------------- refusals nothing reached
//
// A reviewer put `std::abort()` at the head of every refusal branch in the loader and ran the
// suite: **exit 0, eleven of eleven**. Thirteen branches were reached by no test at all, including
// the two that carry weight — the store refusing an allocation, which is the only thing in life
// that makes the rollback matter, and the `cv::Exception` catch that is the entire justification for
// compiling this translation unit with exceptions. What follows reaches them.

const char* kLens = R"("intrinsics": {"fx": 36.95675913154999, "fy": 38.60112456917206,
  "cx": 24.0, "cy": 18.0, "k1": 0.0, "k2": 0.0, "k3": 0.0, "p1": 0.0, "p2": 0.0,
  "width": 48, "height": 36})";

// The rotation convention the loader insists on. Spelled here so the damaged-truth cases below
// carry a valid one and are refused by the thing they are named for rather than by this.
const char* kConvention =
    R"("convention": {"rotation": "device -> world, unit quaternion, matching sphanorama::Quat"})";

std::string TruthWith(const std::string& lens, const std::string& frames) {
  return "{" + std::string(kConvention) + ", " + lens + ", \"frames\": " + frames + "}";
}
const char* kOneFrame = R"([{"file": "frame_0000.ppm", "rotation": {"w": 1.0, "x": 0.0, "y": 0.0, "z": 0.0}}])";

TEST_F(Dataset, RefusesAHeaderNumberThatIsNotOne) {
  Scratch scratch;
  std::ofstream out(scratch.file("frame_0000.ppm"), std::ios::binary | std::ios::trunc);
  out << "P6\nwide 36\n255\n";
  out.close();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument,
                          "has a header field that is not a number"));
}

TEST_F(Dataset, RefusesAFrameWhoseSamplesAreTwoBytesWide) {
  // A 16-bit Netpbm is a real format with a payload twice this size, so reading it as 8-bit would
  // take the high byte of every other sample and call it a picture.
  Scratch scratch;
  int32_t width = 0;
  int32_t height = 0;
  const std::vector<uint8_t> payload = PayloadOf(scratch.file("frame_0000.ppm"), &width, &height);
  std::ofstream out(scratch.file("frame_0000.ppm"), std::ios::binary | std::ios::trunc);
  out << "P6\n" << width << " " << height << "\n65535\n";
  out.write(reinterpret_cast<const char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
  out.close();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "only 8-bit samples are read"));
}

TEST_F(Dataset, RefusesAHeaderTokenLongerThanAnyRealOne) {
  // The file with no whitespace in it. Unbounded, this read the whole thing into a string before the
  // magic number was ever compared — **986,740 KiB, which is 964 MiB**, for a 512 MiB file, in a
  // loader whose payload loop exists to avoid exactly that. A kilobyte is enough to prove the cap.
  //
  // This comment said "988 MB" until round 4. The loader's own comment had withdrawn that figure a
  // round earlier, twenty lines from here in a neighbouring file, and this copy went on asserting
  // it — so the same directory both claimed and denied the number. `ru_maxrss` is in KiB, and even
  // `ru_maxrss / 1000` is 987.
  Scratch scratch;
  std::ofstream out(scratch.file("frame_0000.ppm"), std::ios::binary | std::ios::trunc);
  out << std::string(1024, 'P');
  out.close();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument,
                          "opens with a token longer than any real header token"));
}

TEST_F(Dataset, GivesBackEveryFrameItHadWhenTheStoreRefusesAnAllocation) {
  // `FrameStoreExhausted` part way through is the only arrangement in which the rollback does
  // anything, and it was reached by nothing. Two frames in, then the store says no.
  AwkwardStore awkward{store};
  awkward.refuseAllocateAfter = 2;
  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(awkward, Fixture());
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::FrameStoreExhausted, "refused on purpose"));
  EXPECT_EQ(HeapUsed(), before) << "the two frames it had already read were left behind";
}

TEST_F(Dataset, GivesBackTheFrameWhenTheStoreRefusesToPinIt) {
  AwkwardStore awkward{store};
  awkward.refusePinAfter = 0;
  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(awkward, Fixture());
  // Through the same helper as every other refusal, and asserting the store's own words. Asserting
  // the code alone left this green when `Pin`'s status was flattened to a hard-coded `Internal`
  // with a generic detail — the code matched and the forwarding did not happen, which is the thing
  // the comment claims is being tested.
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::Internal, "refused on purpose"));
  EXPECT_EQ(HeapUsed(), before) << "the allocation it could not pin was left charged";
}

TEST_F(Dataset, RefusesAStoreThatPadsAStrideItNeverPromised) {
  // `Allocate` promises nothing about stride, so the handle and the span are two authorities. With
  // the stride padded and the span the honest size, the write loop runs off the end of the last row
  // — a heap-buffer-overflow a reviewer reproduced under AddressSanitizer.
  AwkwardStore awkward{store};
  awkward.padStrideBy = 64;
  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(awkward, Fixture());
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "the store handed back fewer bytes"));
  EXPECT_EQ(HeapUsed(), before);
}

TEST_F(Dataset, RefusesTruthThatIsNotJsonAtAll) {
  Scratch scratch;
  WriteTruth(scratch, "this is not JSON, and was never going to be");
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "truth.json is not readable JSON"));
}

TEST_F(Dataset, RefusesAScalarWhereAMapBelongsWithoutQuotingOpenCvAtTheUser) {
  // **What this test used to be, and why it changed.** It fed `{"intrinsics": 5}` in to reach the
  // `cv::Exception` arm around the traversal, which until then had converted nothing — `operator[]`
  // asserted inside OpenCV with "Assertion failed (isMap())" and the arm turned that text into the
  // refusal the caller saw, after OpenCV printed its own error to stderr.
  //
  // Every indexing site now checks the node's shape first, so this input refuses in our own words
  // and the arm has no input left. That is the outcome the `isSeq` guard's own thread argued for a
  // round earlier — the guard's value is partly the *silence* — and it means the arm is a backstop
  // rather than a route. It is kept for the reason named where it sits: the guards enumerate what
  // today's OpenCV asserts on, and an upgrade may assert somewhere new.
  //
  // So this asserts the property that actually matters and survives that change: whatever refuses,
  // the user is never handed an OpenCV assertion as the explanation.
  for (const char* truth : {R"("intrinsics": 5)", R"("intrinsics": [1, 2])"}) {
    Scratch scratch;
    WriteTruth(scratch, "{" + std::string(kConvention) + ", " + truth + R"(, "frames": )" +
                            kOneFrame + "}");
    const int64_t before = HeapUsed();
    const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
    EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument,
                            "truth.json's intrinsics are not a JSON object")) << truth;
    EXPECT_EQ(loaded.status.detail.find("Assertion failed"), std::string::npos)
        << loaded.status.detail;
    EXPECT_EQ(HeapUsed(), before);
  }
}

TEST_F(Dataset, RefusesARotationThatIsNotAJsonObject) {
  // `false` is here because the prose about the norm guard said for three rounds that "a
  // `truth.json` whose rotations are `false`" loaded `Ok` with four zeros. It does not: `false` is
  // an INT node, so it is not a map, and *this* guard refuses it — the zeros need each component
  // spelled `false` individually. Two guards, two inputs that look alike in a sentence, and the
  // difference is which one the reader would go and write.
  for (const char* rotation : {"5", "false", R"("identity")", "[1, 0, 0, 0]"}) {
    Scratch scratch;
    WriteTruth(scratch, TruthWith(kLens, std::string(R"([{"file": "frame_0000.ppm", "rotation": )") +
                                             rotation + "}]"));
    const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
    EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument,
                            "a frame's rotation in truth.json is not a JSON object"))
        << rotation;
  }
}

TEST_F(Dataset, RefusesTruthThatDescribesNoLens) {
  Scratch scratch;
  WriteTruth(scratch, "{" + std::string(kConvention) + ", \"frames\": " + kOneFrame + "}");
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "records no intrinsics"));
}

TEST_F(Dataset, RefusesALensMissingAFieldTheReaderNeeds) {
  Scratch scratch;
  WriteTruth(scratch, TruthWith(R"("intrinsics": {"fx": 36.9, "width": 48, "height": 36})",
                                kOneFrame));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "intrinsics: fy is not in the file"));
}

TEST_F(Dataset, RefusesAWidthThatIsNotANumber) {
  // `static_cast<int>` of a non-numeric node answers `0x7FFFFFFF` — measured — which walked past
  // the `<= 0` guard and made a lens 2,147,483,647 pixels wide.
  //
  // **The rest of what this comment used to say is withdrawn.** It claimed the width then reached
  // `Allocate(2147483647, 3)` and came back `FrameStoreExhausted`. It did not: the frame/lens
  // dimension check refuses first, in this commit and in the one that wrote the claim. The guard is
  // right; the story about it was not measured, and a reviewer was the one who ran it.
  Scratch scratch;
  WriteTruth(scratch, TruthWith(R"("intrinsics": {"fx": 36.9, "fy": 38.6, "cx": 24.0, "cy": 18.0,
    "k1": 0.0, "k2": 0.0, "k3": 0.0, "p1": 0.0, "p2": 0.0,
    "width": "not a number", "height": 36})", kOneFrame));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "intrinsics: width is not a whole number")) << "a malformed file must not be reported as a full store";
}

TEST_F(Dataset, RefusesALensWithNoPixelsInIt) {
  Scratch scratch;
  WriteTruth(scratch, TruthWith(R"("intrinsics": {"fx": 36.9, "fy": 38.6, "cx": 24.0, "cy": 18.0,
    "k1": 0.0, "k2": 0.0, "k3": 0.0, "p1": 0.0, "p2": 0.0, "width": 0, "height": 36})", kOneFrame));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "records a lens with no pixels in it"));
}

TEST_F(Dataset, RefusesTruthWhoseFramesAreNotASequence) {
  Scratch scratch;
  WriteTruth(scratch, TruthWith(kLens, R"({"file": "frame_0000.ppm"})"));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "records no sequence of frames"));
}

TEST_F(Dataset, RefusesAFrameEntryMissingItsFileOrItsRotation) {
  // The third case is the `!fileNode.isString()` arm, which no case reached: a `file` that is
  // present and is not a string. Without it that disjunct could be deleted with the suite green.
  for (const char* frames : {R"([{"rotation": {"w": 1.0, "x": 0.0, "y": 0.0, "z": 0.0}}])",
                             R"([{"file": "frame_0000.ppm"}])",
                             R"([{"file": 7, "rotation": {"w": 1.0, "x": 0.0, "y": 0.0, "z": 0.0}}])"}) {
    Scratch scratch;
    WriteTruth(scratch, TruthWith(kLens, frames));
    const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
    EXPECT_FALSE(loaded.ok()) << frames;
    EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "has no file or no rotation")) << frames;
  }
}

TEST_F(Dataset, RefusesARotationMissingAComponent) {
  Scratch scratch;
  WriteTruth(scratch, TruthWith(kLens,
      R"([{"file": "frame_0000.ppm", "rotation": {"w": 1.0, "x": 0.0, "y": 0.0}}])"));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "rotation: z is not in the file"));
}

TEST_F(Dataset, RefusesAFrameEntryThatNamesAPathRatherThanAFile) {
  // `fs::path(dir) / "/etc/hosts"` *replaces* rather than appends, so an absolute name reads from
  // outside the dataset entirely. Nothing writes such a file; a loader that followed one anyway is
  // the wrong default even in test support.
  for (const char* named : {"/etc/hosts", "../frame_0000.ppm", "nested/frame_0000.ppm"}) {
    Scratch scratch;
    WriteTruth(scratch, TruthWith(kLens, std::string(R"([{"file": ")") + named +
                                         R"(", "rotation": {"w": 1.0, "x": 0.0, "y": 0.0, "z": 0.0}}])"));
    const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
    EXPECT_FALSE(loaded.ok()) << named;
    EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "names a path rather than a file")) << named;
  }
}

TEST_F(Dataset, RefusesADatasetWithNoFramesInIt) {
  // Parsed cleanly and loaded as a *successful* dataset of nothing, which a harness would go on to
  // score: a median over no frames is a number nobody should be shown.
  Scratch scratch;
  WriteTruth(scratch, TruthWith(kLens, "[]"));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "names no frames"));
}

TEST_F(Dataset, ReadsAnIntrinsicWrittenWithoutADecimalPoint) {
  // `json.dumps` always writes `0.0`, so the reader's integer branch was unreachable and its comment
  // was wrong about why it existed. It is kept because a `truth.json` is a text file a person can
  // edit, and `0` is what a person writes — so the branch is reachable in life, and now in a test.
  Scratch scratch;
  WriteTruth(scratch, TruthWith(R"("intrinsics": {"fx": 36.95675913154999, "fy": 38.60112456917206,
    "cx": 24, "cy": 18, "k1": 0, "k2": 0, "k3": 0, "p1": 0, "p2": 0, "width": 48, "height": 36})",
                                kOneFrame));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  ASSERT_TRUE(loaded.ok()) << loaded.status.detail;
  EXPECT_DOUBLE_EQ(loaded.value.lens.cx, 24.0);
  EXPECT_DOUBLE_EQ(loaded.value.lens.cy, 18.0);
  ForgetAll(loaded.value);
}

TEST_F(Dataset, GivesTheFramesBackToACallerThatForgetsThem) {
  // The ownership rule this loader shares with `ExtractFeatures`: the frames are the caller's, and
  // forgetting each returns every byte. A harness that leaked a dataset per run would run out of
  // heap somewhere in the middle of a measurement and report that instead.
  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, Fixture());
  ASSERT_TRUE(loaded.ok()) << loaded.status.detail;
  EXPECT_GT(HeapUsed(), before) << "four frames were supposed to cost something";
  ForgetAll(loaded.value);
  EXPECT_EQ(HeapUsed(), before);
}


// ---------------------------------------------------------------- what the numbers land in
//
// Everything above asks whether the loader *refuses* the right files. These ask whether the numbers
// it accepts go where they say they go — which nothing asked before, and which is the only thing
// Phase 2 actually consumes. A reviewer set all five distortion coefficients to 42.0 inside the
// loader and the whole suite stayed green, because every fixture spells them 0.0; the same reviewer
// transposed `x` and `z` in every rotation and nothing failed, because the committed ring turns
// about `+Y` and both are zero in all four frames. A field that is zero everywhere is a field no
// test is reading.

TEST_F(Dataset, EveryIntrinsicLandsInTheFieldItIsNamedFor) {
  // Eleven values, all different, none zero, and none a plausible substitute for another. Any
  // transposition, any dropped field and any constant substituted for the file's value shows up as
  // a specific failure rather than as a suite that stays green.
  Scratch scratch;
  WriteTruth(scratch, TruthWith(R"("intrinsics": {"fx": 11.5, "fy": 22.5, "cx": 33.5, "cy": 44.5,
    "k1": 0.11, "k2": 0.22, "k3": 0.33, "p1": 0.44, "p2": 0.55, "width": 48, "height": 36})",
                                kOneFrame));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  ASSERT_TRUE(loaded.ok()) << loaded.status.detail;
  const Intrinsics& lens = loaded.value.lens;
  EXPECT_DOUBLE_EQ(lens.fx, 11.5);
  EXPECT_DOUBLE_EQ(lens.fy, 22.5);
  EXPECT_DOUBLE_EQ(lens.cx, 33.5);
  EXPECT_DOUBLE_EQ(lens.cy, 44.5);
  EXPECT_DOUBLE_EQ(lens.k1, 0.11);
  EXPECT_DOUBLE_EQ(lens.k2, 0.22);
  EXPECT_DOUBLE_EQ(lens.k3, 0.33);
  EXPECT_DOUBLE_EQ(lens.p1, 0.44);
  EXPECT_DOUBLE_EQ(lens.p2, 0.55);
  EXPECT_EQ(lens.width, 48);
  EXPECT_EQ(lens.height, 36);
  // The two fields the file does not carry, which the header promises are left meaning "not known".
  EXPECT_EQ(lens.rollingShutterLineTimeNs, 0);
  EXPECT_FALSE(lens.estimated);
  ForgetAll(loaded.value);
}

TEST_F(Dataset, EveryRotationComponentLandsInTheFieldItIsNamedFor) {
  // Two frames, eight distinct components, no zeros and no repeats — so a transposition of any pair
  // within a frame, and any confusion between the two frames, is visible.
  //
  // **And unit, which they were not.** The first version used 0.11…0.88 with the reasoning that the
  // loader records what the file spells and normalising is not its job. That is still true of the
  // *sign* — the double cover is preserved, and frame 1 keeps a negative scalar part to prove it —
  // but a quaternion that is not unit is not a rotation, and a reviewer showed the loader accepted
  // one: `{"w": false, "x": false, "y": false, "z": false}` loaded `Ok` with four zeros, after
  // which `ScoreRotations` answers
  // `valid = false` and `medianDeg = 0`, which reads as a perfect score to anyone who checks the
  // median and not the flag. The loader refuses a non-unit norm now, so these are
  // `(1,2,3,4)/√30` and `(-5,6,-7,8)/√174`: all eight still distinct, still mixed in sign.
  Scratch scratch;
  WriteTruth(scratch, TruthWith(kLens,
      R"([{"file": "frame_0000.ppm", "rotation": {"w": 0.18257418583505536, "x": 0.3651483716701107,
           "y": 0.5477225575051661, "z": 0.7302967433402214}},
          {"file": "frame_0001.ppm", "rotation": {"w": -0.3790490217894517, "x": 0.454858826147342,
           "y": -0.5306686305052324, "z": 0.6064784348631227}}])"));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  ASSERT_TRUE(loaded.ok()) << loaded.status.detail;
  ASSERT_EQ(loaded.value.frames.size(), 2u);
  EXPECT_DOUBLE_EQ(loaded.value.frames[0].trueRotation.w, 0.18257418583505536);
  EXPECT_DOUBLE_EQ(loaded.value.frames[0].trueRotation.x, 0.3651483716701107);
  EXPECT_DOUBLE_EQ(loaded.value.frames[0].trueRotation.y, 0.5477225575051661);
  EXPECT_DOUBLE_EQ(loaded.value.frames[0].trueRotation.z, 0.7302967433402214);
  EXPECT_DOUBLE_EQ(loaded.value.frames[1].trueRotation.w, -0.3790490217894517);
  EXPECT_DOUBLE_EQ(loaded.value.frames[1].trueRotation.x, 0.454858826147342);
  EXPECT_DOUBLE_EQ(loaded.value.frames[1].trueRotation.y, -0.5306686305052324);
  EXPECT_DOUBLE_EQ(loaded.value.frames[1].trueRotation.z, 0.6064784348631227);
  ForgetAll(loaded.value);
}

TEST_F(Dataset, RefusesAnIntrinsicThatIsNotAFiniteNumber) {
  // `"cx": 1e400` parses as a real node holding `inf` — measured, not assumed — and an infinite
  // principal point would travel into every projection the harness computes without a word.
  Scratch scratch;
  WriteTruth(scratch, TruthWith(R"("intrinsics": {"fx": 36.9, "fy": 38.6, "cx": 1e400, "cy": 18.0,
    "k1": 0.0, "k2": 0.0, "k3": 0.0, "p1": 0.0, "p2": 0.0, "width": 48, "height": 36})", kOneFrame));
  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "cx is not a finite number"));
  EXPECT_EQ(HeapUsed(), before);
}

TEST_F(Dataset, ReadsAWidthOpenCvHasAlreadyWrappedAndCannotBeToldAbout) {
  // **A limitation pinned, not a behaviour wanted.** OpenCV's JSON parser wraps an integer outside
  // `int`'s range to 32 bits before the node exists, and `isInt()` stays true — so 4294967344
  // arrives at `NodeInt` as 48, and as 48.0 if read as a double. No accessor on `cv::FileNode` sees
  // the original text, so there is nothing here that could refuse it.
  //
  // It loads, and this test says so, because the alternative is a reader that looks guarded and is
  // not. What saves it in practice is the line below: the wrapped width still has to agree with the
  // PPM header, and a 48-wide frame is the only thing that will.
  Scratch scratch;
  WriteTruth(scratch, TruthWith(R"("intrinsics": {"fx": 36.9, "fy": 38.6, "cx": 24.0, "cy": 18.0,
    "k1": 0.0, "k2": 0.0, "k3": 0.0, "p1": 0.0, "p2": 0.0,
    "width": 4294967344, "height": 36})", kOneFrame));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  ASSERT_TRUE(loaded.ok()) << loaded.status.detail;
  EXPECT_EQ(loaded.value.lens.width, 48) << "if this ever refuses instead, the limitation is gone "
                                            "and this test should become a refusal";
  ForgetAll(loaded.value);
}

TEST_F(Dataset, RefusesAHeaderNumberThatOnlyStartsOutAsOne) {
  // `ReadNumber`'s digit check, which the "wide" case above does not reach: `std::stoll` throws on
  // `"wide"` and returns the same `false`, so that test proves the `catch`, not the check. The
  // difference is an *outcome*, not a message — `std::stoll("48x")` returns 48 without complaint, so
  // without the pre-check this header parses as a perfectly ordinary 48-wide frame and the file is
  // read as something it does not say it is.
  Scratch scratch;
  std::ofstream out(scratch.file("frame_0000.ppm"), std::ios::binary | std::ios::trunc);
  out << "P6\n48x 36\n255\n";
  out.close();
  WriteTruth(scratch, TruthWith(kLens, kOneFrame));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument,
                          "has a header field that is not a number"));
}

TEST_F(Dataset, RefusesAHeaderNumberTooLargeForTheTypeThatHoldsIt) {
  // `ReadNumber`'s `catch (...)` had no test and no written reason while its two neighbours had
  // theirs. It is reachable from a file anyone can write: all-digits passes the character check,
  // width is bounded only by the 32-byte token cap, and twenty digits overflow `long long` so
  // `std::stoll` throws `std::out_of_range`.
  Scratch scratch;
  std::ofstream out(scratch.file("frame_0000.ppm"), std::ios::binary | std::ios::trunc);
  out << "P6\n99999999999999999999 36\n255\n";
  out.close();
  WriteTruth(scratch, TruthWith(kLens, kOneFrame));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument,
                          "has a header number too large for the type that holds it"));
}

TEST_F(Dataset, RefusesAFrameFileWithNoMagicNumberAtAll) {
  // The other half of `ReadToken`'s refusal, separated from the length cap so that the cap's test
  // cannot be satisfied by this one. An empty file has no token; a 40-byte first token has one that
  // is too long; before they were split, both said the same thing and neither test could tell which
  // guard had answered.
  Scratch scratch;
  std::ofstream out(scratch.file("frame_0000.ppm"), std::ios::binary | std::ios::trunc);
  out.close();
  WriteTruth(scratch, TruthWith(kLens, kOneFrame));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "has no Netpbm magic number at all"));
}

TEST_F(Dataset, RefusesWhenTheStoreWillNotReleaseThePinItTook) {
  // A leak that arrives through the *success* path, which is the worst kind because nothing is
  // looking. `HeldFrame::Release` used to clear its flag whichever way the store went, so a refused
  // release looked finished: the destructor skipped its retry, `Commit` suppressed the `Forget`, and
  // the load returned four frames that were still pinned — which `Forget` refuses and which makes
  // `Clear` refuse for the whole store. No real store reaches this; `IFrameStoreAccess::Release`
  // returns a `Status` because an implementation may, so a test store supplies one.
  AwkwardStore awkward{store};
  awkward.refuseReleaseAfter = 0;
  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(awkward, Fixture());
  ASSERT_FALSE(loaded.ok()) << "a frame nobody can release was handed over as if it were usable";
  EXPECT_NE(loaded.status.detail.find("would not release the pin"), std::string::npos)
      << loaded.status.detail;

  // **And the consequence, which this test asserted nothing about for six rounds.** The header
  // calls a permanently unreleasable frame the worst kind of refusal — `Forget` refuses a pinned
  // frame and so does `Clear`, so those bytes are charged for the life of the store — and that was
  // prose alone here, in the one test that reaches the state it describes. Its transient twin
  // `AStoreThatRefusesOneReleaseAndRelentsLeavesNothingBehind` asserts the store's totals and this
  // one did not, which is the same asymmetry round 5 removed from the `Forget` pair.
  //
  // **What these two can and cannot catch, because I got that wrong when I wrote them.** I
  // predicted both would fail under a `HeldFrame::Release` that believes a refusal
  // (`pinned_ = false` regardless); neither did, because the *store's* pin count is untouched by
  // that sabotage, so `Forget` still refuses and the bytes stay charged. Two other tests caught it.
  // So these assert the state the header describes rather than the loader's own arithmetic: with
  // `refuseReleaseAfter = 0` the store can never let go, and the only way `HeapUsed()` comes back
  // to `before` is the loader never pinning at all. That is the arrangement check round 6 said
  // every test of this shape needs — it fails if this test stops reaching the state it is named
  // for — and it is not a check on the rollback, which is what the twin below is.
  EXPECT_GT(HeapUsed(), before)
      << "nothing is charged, so this test never got as far as pinning a frame and proves nothing "
         "about a refused release";
  EXPECT_FALSE(store.Clear().ok())
      << "Clear is supposed to refuse while anything is pinned, which is what makes these bytes "
         "unrecoverable rather than merely stranded — if it succeeds, the frame is not pinned and "
         "the arrangement is gone";
}

TEST_F(Dataset, SaysSoWhenTheStoreWillNotTakeItsFramesBack) {
  // `Forget` is allowed to refuse — `MemoryFrameStoreAccess` returns a spill sink's refusal and
  // keeps the entry, so its totals go on accounting for bytes nobody holds a handle to. The caller
  // of a failed load holds no handles at all, so a rollback that quietly failed would be
  // unobservable. The header promises a refusal gives every frame back *and says so when it cannot*.
  AwkwardStore awkward{store};
  awkward.refuseForgetAfter = 0;
  // Two frames read, then a third entry naming a file that is not there: the rollback has something
  // to give back and the store will not take it.
  Scratch scratch;
  WriteTruth(scratch, TruthWith(kLens,
      R"([{"file": "frame_0000.ppm", "rotation": {"w": 1.0, "x": 0.0, "y": 0.0, "z": 0.0}},
          {"file": "frame_0001.ppm", "rotation": {"w": 1.0, "x": 0.0, "y": 0.0, "z": 0.0}},
          {"file": "absent.ppm", "rotation": {"w": 1.0, "x": 0.0, "y": 0.0, "z": 0.0}}])"));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(awkward, scratch.path());
  ASSERT_FALSE(loaded.ok());
  EXPECT_NE(loaded.status.detail.find("refused to give back a frame read before this failure"),
            std::string::npos)
      << loaded.status.detail;
  EXPECT_TRUE(store.Clear().ok());
}

TEST_F(Dataset, RefusesADatasetThatDoesNotStateTheRotationConventionThisReaderAssumes) {
  // The one field whose violation is invisible. `truth.json` has always carried a `convention`
  // block, and until a reviewer asserted the *set* of its keys rather than the presence of the two
  // the loader wanted, nothing in C++ had ever read a word of it — including the line saying which
  // way round the quaternions go. A dataset written the other way round loads perfectly, scores
  // perfectly, and every number is wrong by an inverse.
  for (const char* convention : {R"("convention": {"rotation": "world -> device"})",
                                 R"("convention": {"rotation": 7})",
                                 R"("convention": {})",
                                 R"("convention": "device -> world")"}) {
    Scratch scratch;
    WriteTruth(scratch, "{" + std::string(convention) + ", " + kLens + ", \"frames\": " +
                            kOneFrame + "}");
    const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
    EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument,
                            "does not state the rotation convention this reader assumes"))
        << convention;
  }
}

TEST_F(Dataset, RefusesADatasetWithNoConventionBlockAtAll) {
  Scratch scratch;
  WriteTruth(scratch, "{" + std::string(kLens) + ", \"frames\": " + kOneFrame + "}");
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument,
                          "does not state the rotation convention this reader assumes"));
}

TEST_F(Dataset, RefusesAStoreThatHandsBackANarrowerRowThanItPromised) {
  // The stride guard's *first* arm, which `padStrideBy` could not reach while it only ever widened.
  // A store under-reporting stride is the quieter of the two failures: the write loop overlaps its
  // rows and hands back a sheared frame rather than crashing, so every feature is in the wrong place
  // and nothing says so.
  AwkwardStore awkward{store};
  awkward.padStrideBy = -64;
  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(awkward, Fixture());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument,
                          "the store handed back fewer bytes"));
  EXPECT_EQ(HeapUsed(), before);
}

TEST_F(Dataset, RefusesAShortSpanForALensOfOneRow) {
  // The guard's *third* arm. It exists because the middle arm divides by `height - 1` and so says
  // nothing at all when the height is one — the row is then the whole bound. Reaching it needs both
  // a one-row lens and a store that hands back less than it allocated, which is why `AwkwardStore`
  // grew `shortenPinBy`: no real store can be persuaded to do this.
  Scratch scratch;
  {
    std::ofstream out(scratch.file("frame_0000.ppm"), std::ios::binary | std::ios::trunc);
    out << "P6\n48 1\n255\n";
    const std::vector<uint8_t> row(48 * 3, 17);
    out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
  }
  WriteTruth(scratch, TruthWith(R"("intrinsics": {"fx": 36.9, "fy": 38.6, "cx": 24.0, "cy": 0.5,
    "k1": 0.0, "k2": 0.0, "k3": 0.0, "p1": 0.0, "p2": 0.0, "width": 48, "height": 1})", kOneFrame));

  AwkwardStore awkward{store};
  const int64_t before = HeapUsed();
  {
    // First without the lie, so the arrangement is known to be otherwise valid — a refusal test
    // whose input is malformed in some second way proves only the second way.
    const Result<SyntheticDataset> fine = LoadSyntheticDataset(awkward, scratch.path());
    ASSERT_TRUE(fine.ok()) << fine.status.detail;
    ForgetAll(fine.value);
  }
  awkward.shortenPinBy = 4;
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(awkward, scratch.path());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument,
                          "the store handed back fewer bytes"));
  EXPECT_EQ(HeapUsed(), before);
}

TEST_F(Dataset, ReadsAFrameWhoseHeaderCarriesTheCommentNetpbmAllows) {
  // `ReadToken`'s comment branch could be deleted outright with the suite green, because the
  // generator never writes one and every damaged copy in this file is hand-written without one. The
  // docstring claims a reader that choked on a comment would be refusing a valid file; this is the
  // only case here that is green *because* the branch is present rather than because something else
  // refused first.
  Scratch scratch;
  int32_t width = 0;
  int32_t height = 0;
  const std::vector<uint8_t> payload = PayloadOf(scratch.file("frame_0000.ppm"), &width, &height);
  {
    std::ofstream out(scratch.file("frame_0000.ppm"), std::ios::binary | std::ios::trunc);
    out << "P6\n# rendered by hand, which Netpbm allows anywhere in the header\n"
        << width << " " << height << "\n255\n";
    out.write(reinterpret_cast<const char*>(payload.data()),
              static_cast<std::streamsize>(payload.size()));
  }
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  ASSERT_TRUE(loaded.ok()) << loaded.status.detail;
  ForgetAll(loaded.value);
}

// ------------------------------------------------- refusing in our own words, at every site

TEST_F(Dataset, RefusesTruthWhoseTopLevelIsNotAnObject) {
  // **This is the `open` boundary, not a guard of ours, and round 3 got that wrong.** A guard on
  // `file.root().isMap()` was added here and was unreachable: `cv::FileStorage::open` throws first
  // for every non-object top level, which this file had measured a round earlier and written down
  // twenty lines from where the guard went. The guard is gone; what remains is the behaviour, which
  // is worth a test either way.
  //
  // Through `RefusedWith` like every other refusal, so it asserts our own sentence rather than
  // merely the absence of the word "Assertion" — OpenCV's text is appended after our prefix, and a
  // test that only excluded one phrase of it was asserting almost nothing.
  for (const char* truth : {"[1, 2, 3]", "5", "\"hello\"", "[]", "true"}) {
    Scratch scratch;
    WriteTruth(scratch, truth);
    const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
    EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument,
                            "truth.json is not readable JSON")) << truth;
  }
}

TEST_F(Dataset, RefusesIntrinsicsThatAreNotAJsonObject) {
  // `{"intrinsics": 5}` used to refuse by letting `operator[]` assert inside OpenCV and converting
  // the assertion text into our refusal — and printing OpenCV's error to stderr on the way. Round 2
  // put an `isMap()` guard on `convention` and left this site and three others forwarding.
  Scratch scratch;
  WriteTruth(scratch, "{" + std::string(kConvention) + R"(, "intrinsics": 5, "frames": )" +
                          kOneFrame + "}");
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument,
                          "truth.json's intrinsics are not a JSON object"));
}

TEST_F(Dataset, RefusesAFrameEntryThatIsNotAJsonObject) {
  Scratch scratch;
  WriteTruth(scratch, TruthWith(kLens, "[5]"));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument,
                          "a frame entry in truth.json is not a JSON object"));
}

TEST_F(Dataset, EachWayAHeaderNumberCanFailSaysWhichOneItWas) {
  // Four structurally different files that all used to produce "has a header this reader cannot
  // parse". `ReadNumber` declared a reason and discarded it, so no test could tell them apart and
  // any one of the four guards could answer for the others.
  struct Case { const char* header; const char* says; };
  for (const Case& one : {Case{"P6\n48", "stops before its three numbers"},
                          Case{"P6\n123456789012345678901234567890123456 36\n255\n",
                               "field longer than any real one"},
                          Case{"P6\n48x 36\n255\n", "field that is not a number"},
                          Case{"P6\n99999999999999999999 36\n255\n",
                               "number too large for the type that holds it"}}) {
    Scratch scratch;
    {
      std::ofstream out(scratch.file("frame_0000.ppm"), std::ios::binary | std::ios::trunc);
      out << one.header;
    }
    WriteTruth(scratch, TruthWith(kLens, kOneFrame));
    const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
    EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, one.says)) << one.header;
  }
}

// ------------------------------------------------- what a refusal leaves behind, and what it says

TEST_F(Dataset, SaysSoWhenTheStoreKeepsTheFrameItWasReading) {
  // The frame being read is in no `OwnedFrames` — it joins the dataset's list only once the read
  // commits — so the loader's rollback never covered it, and the header's "says so when it cannot"
  // was broken for exactly one frame: the one in hand when the failure happened. A reviewer probed
  // it at 6,912 bytes charged with the refusal saying nothing.
  AwkwardStore awkward{store};
  awkward.refusePinAfter = 0;
  awkward.refuseForgetAfter = 0;
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(awkward, Fixture());
  ASSERT_FALSE(loaded.ok());
  EXPECT_NE(loaded.status.detail.find("refused to give this frame back"), std::string::npos)
      << loaded.status.detail;
  EXPECT_TRUE(store.Clear().ok());
}

TEST_F(Dataset, AStoreThatRefusesOneReleaseAndRelentsLeavesNothingBehind) {
  // The transient case, which the permanent one cannot distinguish. `Rollback` retries the release,
  // so a store that declines once and then behaves leaves the store exactly as it was — and the
  // refusal must not claim the frame is stranded, which the first wording did unconditionally
  // because its only test used a store that refuses for ever.
  AwkwardStore awkward{store};
  awkward.refuseReleaseTimes = 1;
  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(awkward, Fixture());
  ASSERT_FALSE(loaded.ok()) << "a frame whose pin was refused must not be handed over";
  EXPECT_EQ(HeapUsed(), before) << "the retry should have given this frame back";
  EXPECT_EQ(loaded.status.detail.find("refused to give this frame back"), std::string::npos)
      << "nothing was left behind, so the refusal must not say there was: " << loaded.status.detail;
  EXPECT_TRUE(store.Clear().ok()) << "and the store must still be clearable";
}

TEST_F(Dataset, ARollbackKeepsWhatTheStoreRefusedSoTheBackstopCanTryAgain) {
  // `OwnedFrames::Rollback` used to clear its list whatever `Forget` answered, which made
  // `~OwnedFrames` — the backstop the class documents — a no-op over an empty vector. A store that
  // declines one `Forget` and then behaves is the only arrangement that can tell the two apart:
  // with the frames kept, the destructor's second pass gives them back and the store ends where it
  // started; with the list cleared, those bytes stay charged for ever.
  //
  // Permanent-refusal tests cannot see this, which is why the bug survived a round: the message is
  // identical either way, and only the store's totals afterwards differ.
  const int64_t before = HeapUsed();
  {
    AwkwardStore awkward{store};
    awkward.refuseForgetTimes = 1;
    Scratch scratch;
    WriteTruth(scratch, TruthWith(kLens,
        R"([{"file": "frame_0000.ppm", "rotation": {"w": 1.0, "x": 0.0, "y": 0.0, "z": 0.0}},
            {"file": "frame_0001.ppm", "rotation": {"w": 1.0, "x": 0.0, "y": 0.0, "z": 0.0}},
            {"file": "absent.ppm", "rotation": {"w": 1.0, "x": 0.0, "y": 0.0, "z": 0.0}}])"));
    const Result<SyntheticDataset> loaded = LoadSyntheticDataset(awkward, scratch.path());
    ASSERT_FALSE(loaded.ok());
    // **The arrangement, asserted.** Without this the test is green when `refuseForgetTimes` does
    // nothing at all — a reviewer made the knob inert and the suite stayed exit 0, which is the one
    // of `AwkwardStore`'s six knobs with that property. A test for a retry that never provokes a
    // refusal proves the store can forget, which nothing doubted.
    EXPECT_NE(loaded.status.detail.find("refused to give back a frame"), std::string::npos)
        << "the store never actually declined, so there was no retry to observe: "
        << loaded.status.detail;
    // **And the message must not claim the bytes are lost — here, where that would be false.**
    // Asserted in the *positive*, and that is the whole point. Round 4 put this assertion on the
    // permanent-refusal test, where the over-claim would have been *true* and no wording could
    // discriminate; round 5 moved it to this arrangement, which can contradict it, but left it
    // forbidding two literal phrases — and a reviewer then rewrote the suffix into a *stronger*
    // over-claim in different words ("those bytes are gone for good and nothing can ever recover
    // them") with all 56 tests green. Forbidding the wordings we happen to have written before is
    // not forbidding the claim. Requiring the hedge is, because there is no way to say the bytes
    // are certainly lost while also saying they may or may not be.
    EXPECT_NE(loaded.status.detail.find("may or may not still be charged"), std::string::npos)
        << "the refusal must not promise an outcome the retry below has not decided yet: "
        << loaded.status.detail;
  }
  EXPECT_EQ(HeapUsed(), before)
      << "the frame the store declined once was dropped instead of retried";
}

TEST_F(Dataset, TheRollbackRetriesARefusedReleaseRatherThanBelievingIt) {
  // Two refusals, not one, and the number is the whole test. With one, `ReadFrame`'s explicit
  // `Release` consumes the refusal and the rollback's own call succeeds either way — so a rollback
  // that *believed* a refused release would look identical. With two, the difference is everything:
  //
  //   keeping the flag honest: the rollback's release is refused too, so `pinned_` stays true and
  //     `Forget` is skipped (it refuses a pinned frame anyway); the destructor tries once more, the
  //     store has stopped refusing, and the frame goes back.
  //   believing the refusal: `pinned_` is cleared while the frame is still pinned, `Forget` is
  //     attempted and refused, and the destructor — seeing nothing pinned — never releases. The
  //     bytes are charged for ever and `Clear` refuses for the life of the store.
  //
  // A sabotage of the flag left the one-refusal test green, which is how this gap was found.
  const int64_t before = HeapUsed();
  {
    AwkwardStore awkward{store};
    awkward.refuseReleaseTimes = 2;
    const Result<SyntheticDataset> loaded = LoadSyntheticDataset(awkward, Fixture());
    ASSERT_FALSE(loaded.ok());
  }
  EXPECT_EQ(HeapUsed(), before) << "the destructor's retry never happened, so the frame is pinned "
                                   "for ever and Clear will refuse for the life of this store";
  EXPECT_TRUE(store.Clear().ok());
}

TEST_F(Dataset, RefusesARotationThatIsNotAUnitQuaternion) {
  // The failure this exists for is not a crash. A `truth.json` spelling each rotation component
  // as `false` parses
  // as four zeros; the loader used to accept it, and `ScoreRotations` then reports `valid = false`
  // with `medianDeg = 0` — which is what a perfect reconstruction also reports, to anyone reading
  // the median rather than the flag. That median is Phase 2's exit criterion.
  //
  // `false` first, because it is the shape a reviewer actually found. Then a plain zero quaternion,
  // and a scaled one, so the guard is tested on the norm rather than on a JSON quirk.
  for (const char* rotation : {R"({"w": false, "x": false, "y": false, "z": false})",
                               R"({"w": 0.0, "x": 0.0, "y": 0.0, "z": 0.0})",
                               R"({"w": 2.0, "x": 0.0, "y": 0.0, "z": 0.0})",
                               R"({"w": 0.5, "x": 0.5, "y": 0.5, "z": 0.0})"}) {
    Scratch scratch;
    WriteTruth(scratch, TruthWith(kLens, std::string(R"([{"file": "frame_0000.ppm", "rotation": )") +
                                             rotation + "}]"));
    const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
    EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "is not a unit quaternion"))
        << rotation;
  }
}

TEST_F(Dataset, TheDoubleCoverStillLoadsAndSoDoesTheFixture) {
  // The other half of the guard above, and the reason its bound is on the norm rather than on any
  // component: a negative scalar part is a unit quaternion and must still load. The committed
  // fixture's fourth frame is exactly that, which is why this reads the fixture rather than a
  // hand-written file — a guard that refused the generator's own output would be caught here.
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, Fixture());
  ASSERT_TRUE(loaded.ok()) << loaded.status.detail;
  ASSERT_EQ(loaded.value.frames.size(), 4u);
  EXPECT_LT(loaded.value.frames[3].trueRotation.w, 0.0) << "the fixture no longer spells one";
  ForgetAll(loaded.value);
}

TEST_F(Dataset, EveryGuardBeforeTheReadGivesBackTheFramesAlreadyReadAndSaysSoWhenItCannot) {
  // **The frames loop refuses in six places before it ever calls `ReadFrame`, and every test for
  // all six put the bad entry first** — where `OwnedFrames` is empty, so the rollback has nothing
  // to give back and cannot be wrong. A reviewer found this on the norm guard, round 5's newest;
  // it was true of the other five as well, which makes it one gap rather than six.
  //
  // This is round 1's finding wearing new clothes: *"a refusal on the first frame leaves
  // `OwnedFrames` empty and proves nothing about it."* That was said of the Netpbm guards, the
  // fixture was moved off `frame_0000`, and the guards added since were written back into the
  // shape it warned about.
  //
  // **Two stores per case, because the heap alone cannot see this.** The first version of this test
  // asserted only that the heap came back, and a sabotage replacing the norm guard's `refuse()`
  // with a bare `Err` — dropping the explicit rollback entirely — left it green: `~OwnedFrames` is
  // the backstop and gives the frames back either way. That is the destructor doing its job, and it
  // is exactly why the *message* is the discriminating assertion. Only `refuse()` composes the
  // suffix; the destructor's rollback is silent. So each guard is driven twice — once against a
  // clean store, where the bytes must come back, and once against a store that will not take them,
  // where the refusal must say so.
  struct Case { const char* third; const char* says; };
  const Case cases[] = {
      {"7", "a frame entry in truth.json is not a JSON object"},
      {R"({"rotation": {"w": 1.0, "x": 0.0, "y": 0.0, "z": 0.0}})",
       "a frame entry in truth.json has no file or no rotation"},
      {R"({"file": "frame_0002.ppm", "rotation": 5})",
       "a frame's rotation in truth.json is not a JSON object"},
      {R"({"file": "frame_0002.ppm", "rotation": {"w": 1.0, "x": 0.0, "y": 0.0}})",
       "a frame's rotation: z is not in the file"},
      {R"({"file": "frame_0002.ppm", "rotation": {"w": 0.0, "x": 0.0, "y": 0.0, "z": 0.0}})",
       "is not a unit quaternion"},
      {R"({"file": "../frame_0002.ppm",
           "rotation": {"w": 1.0, "x": 0.0, "y": 0.0, "z": 0.0}})",
       "names a path rather than a file in the dataset"},
  };
  for (const Case& one : cases) {
    Scratch scratch;
    WriteTruth(scratch, TruthWith(kLens,
        std::string(R"([{"file": "frame_0000.ppm", "rotation": {"w": 1.0, "x": 0.0, "y": 0.0, "z": 0.0}},
            {"file": "frame_0001.ppm", "rotation": {"w": 1.0, "x": 0.0, "y": 0.0, "z": 0.0}}, )") +
        one.third + "]"));

    // A store that takes its frames back: the two already read must be gone.
    const int64_t before = HeapUsed();
    const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
    EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, one.says)) << one.third;
    EXPECT_EQ(HeapUsed(), before)
        << "two frames were read before this guard fired: " << one.third;

    // A store that will not: the refusal must report it, which only `refuse()` does.
    AwkwardStore awkward{store};
    awkward.refuseForgetAfter = 0;
    const Result<SyntheticDataset> kept = LoadSyntheticDataset(awkward, scratch.path());
    EXPECT_TRUE(RefusedWith(kept, StatusCode::InvalidArgument, one.says)) << one.third;
    EXPECT_NE(kept.status.detail.find("refused to give back a frame read before this failure"),
              std::string::npos)
        << "this guard refused without reporting that the store kept the frames: " << one.third
        << " -> " << kept.status.detail;
    EXPECT_TRUE(store.Clear().ok());
  }
}

TEST_F(Dataset, TheUnitBoundIsTightEnoughToRefuseARotationThatIsNearlyOne) {
  // **The bound needs a case near it, or it is not a bound.** A reviewer loosened `1e-6` by five
  // orders of magnitude with the suite green, because the tightest input any test offered was
  // `{0.5, 0.5, 0.5, 0.0}` — norm 0.866, so anything under 0.134 passed. A quaternion wrong in the
  // fourth decimal is the shape a real generator bug produces, and it is the one this guard is for.
  Scratch scratch;
  WriteTruth(scratch, TruthWith(kLens,
      R"([{"file": "frame_0000.ppm", "rotation": {"w": 1.0001, "x": 0.0, "y": 0.0, "z": 0.0}}])"));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, "is not a unit quaternion"));
}

TEST_F(Dataset, EachIntrinsicComplaintNamesItsOwnFieldAndItsOwnReason) {
  // `Blame` writes four sentences and only two were asserted anywhere — a reviewer swapped each of
  // the other two for the wrong guard's wording with the whole suite green. Four fields, four
  // reasons, one test.
  struct Case { const char* intrinsics; const char* says; };
  for (const Case& one : {
           // absent, read by NodeDouble
           Case{R"("intrinsics": {"fy": 38.6, "cx": 24.0, "cy": 18.0, "k1": 0.0, "k2": 0.0,
                 "k3": 0.0, "p1": 0.0, "p2": 0.0, "width": 48, "height": 36})",
                "fx is not in the file"},
           // present and not a number, read by NodeDouble
           Case{R"("intrinsics": {"fx": "no", "fy": 38.6, "cx": 24.0, "cy": 18.0, "k1": 0.0,
                 "k2": 0.0, "k3": 0.0, "p1": 0.0, "p2": 0.0, "width": 48, "height": 36})",
                "fx is not a number"},
           // absent, read by NodeInt
           Case{R"("intrinsics": {"fx": 36.9, "fy": 38.6, "cx": 24.0, "cy": 18.0, "k1": 0.0,
                 "k2": 0.0, "k3": 0.0, "p1": 0.0, "p2": 0.0, "height": 36})",
                "width is not in the file"},
           // present and not whole, read by NodeInt
           Case{R"("intrinsics": {"fx": 36.9, "fy": 38.6, "cx": 24.0, "cy": 18.0, "k1": 0.0,
                 "k2": 0.0, "k3": 0.0, "p1": 0.0, "p2": 0.0, "width": "no", "height": 36})",
                "width is not a whole number"},
       }) {
    Scratch scratch;
    WriteTruth(scratch, TruthWith(one.intrinsics, kOneFrame));
    const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
    EXPECT_TRUE(RefusedWith(loaded, StatusCode::InvalidArgument, one.says)) << one.intrinsics;
  }
}

TEST_F(Dataset, ReadFrameAlsoStopsShortOfClaimingTheBytesAreLost) {
  // The inner `refuse`'s twin of the outer one. Round 4 stopped `LoadSyntheticDataset`'s suffix
  // claiming "its totals still account for bytes no handle names"; `ReadFrame`'s kept saying it for
  // another round, which is the third consecutive time a correction reached one half of a pair.
  //
  // The arrangement is what makes this able to fail, and the permanent-refusal test cannot: `Pin`
  // refused once so the frame is never pinned, `Forget` refused **once** so the inner rollback
  // reports failure and appends the suffix, and then `~HeldFrame` retries and the store relents. So
  // the bytes are all back while the sentence saying the store declined has already been written —
  // exactly the case an over-strong suffix gets wrong.
  const int64_t before = HeapUsed();
  {
    AwkwardStore awkward{store};
    awkward.refusePinAfter = 0;
    awkward.refuseForgetTimes = 1;
    const Result<SyntheticDataset> loaded = LoadSyntheticDataset(awkward, Fixture());
    ASSERT_FALSE(loaded.ok());
    EXPECT_NE(loaded.status.detail.find("refused to give this frame back"), std::string::npos)
        << "the inner rollback did not report, so this test is not reaching its own subject: "
        << loaded.status.detail;
    // The positive form, for the reason spelled out on the twin above: two negative assertions
    // here pinned literals that appear nowhere in the loader, so any reworded over-claim passed.
    EXPECT_NE(loaded.status.detail.find("may or may not still be charged"), std::string::npos)
        << "the inner refusal must not promise an outcome the destructor's retry decides: "
        << loaded.status.detail;
  }
  EXPECT_EQ(HeapUsed(), before) << "the retry should have given this frame back";
}
}  // namespace
}  // namespace sphanorama
