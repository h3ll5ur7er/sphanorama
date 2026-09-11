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
    root_ = fs::temp_directory_path() / fs::path(std::tmpnam(nullptr)).filename();
    fs::create_directories(root_);
    fs::copy(Fixture(), root_, fs::copy_options::recursive);
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
  int32_t padStrideBy = 0;

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
    return real_.Pin(honest);
  }
  Status Release(const FrameRef& frame) override {
    FrameRef honest = frame;
    honest.stride -= padStrideBy;
    return real_.Release(honest);
  }
  Status Forget(const FrameRef& frame) override {
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
};

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
  EXPECT_EQ(loaded.status.code, StatusCode::NotFound);
  EXPECT_EQ(HeapUsed(), before);
}

TEST_F(Dataset, RefusesADatasetWithNoTruthToDescribeIt) {
  Scratch scratch;
  fs::remove(scratch.file("truth.json"));
  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status.code, StatusCode::NotFound);
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
  EXPECT_EQ(loaded.status.code, StatusCode::NotFound);
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
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument);
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
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument);
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
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument);
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
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument);
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

std::string TruthWith(const std::string& lens, const std::string& frames) {
  return "{" + lens + ", \"frames\": " + frames + "}";
}
const char* kOneFrame = R"([{"file": "frame_0000.ppm", "rotation": {"w": 1.0, "x": 0.0, "y": 0.0, "z": 0.0}}])";

TEST_F(Dataset, RefusesAHeaderNumberThatIsNotOne) {
  Scratch scratch;
  std::ofstream out(scratch.file("frame_0000.ppm"), std::ios::binary | std::ios::trunc);
  out << "P6\nwide 36\n255\n";
  out.close();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument);
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
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument);
}

TEST_F(Dataset, RefusesAHeaderTokenLongerThanAnyRealOne) {
  // The file with no whitespace in it. Unbounded, this read the whole thing into a string before the
  // magic number was ever compared — a reviewer measured 988 MB resident for a 512 MiB file, in a
  // loader whose payload loop exists to avoid exactly that. A kilobyte is enough to prove the cap.
  Scratch scratch;
  std::ofstream out(scratch.file("frame_0000.ppm"), std::ios::binary | std::ios::trunc);
  out << std::string(1024, 'P');
  out.close();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument);
}

TEST_F(Dataset, GivesBackEveryFrameItHadWhenTheStoreRefusesAnAllocation) {
  // `FrameStoreExhausted` part way through is the only arrangement in which the rollback does
  // anything, and it was reached by nothing. Two frames in, then the store says no.
  AwkwardStore awkward{store};
  awkward.refuseAllocateAfter = 2;
  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(awkward, Fixture());
  EXPECT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status.code, StatusCode::FrameStoreExhausted);
  EXPECT_EQ(HeapUsed(), before) << "the two frames it had already read were left behind";
}

TEST_F(Dataset, GivesBackTheFrameWhenTheStoreRefusesToPinIt) {
  AwkwardStore awkward{store};
  awkward.refusePinAfter = 0;
  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(awkward, Fixture());
  EXPECT_FALSE(loaded.ok());
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
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument);
  EXPECT_EQ(HeapUsed(), before);
}

TEST_F(Dataset, RefusesTruthThatIsNotJsonAtAll) {
  Scratch scratch;
  WriteTruth(scratch, "this is not JSON, and was never going to be");
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument);
}

TEST_F(Dataset, RefusesTruthWhoseShapeMakesOpenCvThrowPartWayThrough) {
  // The `cv::Exception` catch around the traversal, which is the entire justification for compiling
  // this translation unit with exceptions (ADR 0052, 0053) and which a reviewer showed had never
  // converted one. `FileStorage` opens this file happily and then throws on `operator[]` — measured:
  // "Assertion failed (isMap())" — because `intrinsics` is a scalar where a map was indexed.
  Scratch scratch;
  WriteTruth(scratch, std::string(R"({"intrinsics": 5, "frames": )") + kOneFrame + "}");
  const int64_t before = HeapUsed();
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok()) << "an OpenCV assertion escaped as an exception instead of a Result";
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument);
  EXPECT_EQ(HeapUsed(), before);
}

TEST_F(Dataset, RefusesTruthThatDescribesNoLens) {
  Scratch scratch;
  WriteTruth(scratch, std::string("{\"frames\": ") + kOneFrame + "}");
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument);
}

TEST_F(Dataset, RefusesALensMissingAFieldTheReaderNeeds) {
  Scratch scratch;
  WriteTruth(scratch, TruthWith(R"("intrinsics": {"fx": 36.9, "width": 48, "height": 36})",
                                kOneFrame));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument);
}

TEST_F(Dataset, RefusesAWidthThatIsNotANumber) {
  // `static_cast<int>` of a non-numeric node answers `0x7FFFFFFF` — measured — which walked past a
  // `<= 0` guard and reached `Allocate(2147483647, 3)`. The caller was then told the store was
  // exhausted, when the truth was that the file was malformed.
  Scratch scratch;
  WriteTruth(scratch, TruthWith(R"("intrinsics": {"fx": 36.9, "fy": 38.6, "cx": 24.0, "cy": 18.0,
    "k1": 0.0, "k2": 0.0, "k3": 0.0, "p1": 0.0, "p2": 0.0,
    "width": "not a number", "height": 36})", kOneFrame));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument)
      << "a malformed file must not be reported as a full store";
}

TEST_F(Dataset, RefusesALensWithNoPixelsInIt) {
  Scratch scratch;
  WriteTruth(scratch, TruthWith(R"("intrinsics": {"fx": 36.9, "fy": 38.6, "cx": 24.0, "cy": 18.0,
    "k1": 0.0, "k2": 0.0, "k3": 0.0, "p1": 0.0, "p2": 0.0, "width": 0, "height": 36})", kOneFrame));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument);
}

TEST_F(Dataset, RefusesTruthWhoseFramesAreNotASequence) {
  Scratch scratch;
  WriteTruth(scratch, TruthWith(kLens, R"({"file": "frame_0000.ppm"})"));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument);
}

TEST_F(Dataset, RefusesAFrameEntryMissingItsFileOrItsRotation) {
  for (const char* frames : {R"([{"rotation": {"w": 1.0, "x": 0.0, "y": 0.0, "z": 0.0}}])",
                             R"([{"file": "frame_0000.ppm"}])"}) {
    Scratch scratch;
    WriteTruth(scratch, TruthWith(kLens, frames));
    const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
    EXPECT_FALSE(loaded.ok()) << frames;
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument) << frames;
  }
}

TEST_F(Dataset, RefusesARotationMissingAComponent) {
  Scratch scratch;
  WriteTruth(scratch, TruthWith(kLens,
      R"([{"file": "frame_0000.ppm", "rotation": {"w": 1.0, "x": 0.0, "y": 0.0}}])"));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument);
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
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument) << named;
  }
}

TEST_F(Dataset, RefusesADatasetWithNoFramesInIt) {
  // Parsed cleanly and loaded as a *successful* dataset of nothing, which a harness would go on to
  // score: a median over no frames is a number nobody should be shown.
  Scratch scratch;
  WriteTruth(scratch, TruthWith(kLens, "[]"));
  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument);
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

}  // namespace
}  // namespace sphanorama
