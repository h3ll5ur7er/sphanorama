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

/** The first frame's payload, read by an independent parser, so the loader is checked not echoed. */
std::vector<uint8_t> PayloadOf(const fs::path& ppm, int32_t* width, int32_t* height) {
  std::ifstream in(ppm, std::ios::binary);
  std::string magic;
  int w = 0;
  int h = 0;
  int maxValue = 0;
  in >> magic >> w >> h >> maxValue;
  in.get();   // the single whitespace byte after the maximum, which the payload starts after
  *width = w;
  *height = h;
  std::vector<uint8_t> bytes(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
  in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return bytes;
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

  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument);
}

TEST_F(Dataset, RefusesAFileThatIsNotANetpbmAtAll) {
  Scratch scratch;
  std::ofstream out(scratch.file("frame_0000.ppm"), std::ios::binary | std::ios::trunc);
  out << "P5\n48 36\n255\n";   // greyscale Netpbm: a real format, and not the one written here
  out.close();

  const Result<SyntheticDataset> loaded = LoadSyntheticDataset(store, scratch.path());
  EXPECT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status.code, StatusCode::InvalidArgument);
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
