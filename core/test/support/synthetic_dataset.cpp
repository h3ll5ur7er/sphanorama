#include "support/synthetic_dataset.h"

#include <opencv2/core.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace sphanorama {
namespace {

namespace fs = std::filesystem;

constexpr const char* kComponent = "SyntheticDataset";

/**
 * Every frame allocated so far, given back unless the load commits them.
 *
 * The same shape `FeatureRegistrationEngine` uses, and safe for the same reason: these frames are
 * this call's, created here and seen by nobody, so forgetting them drops nothing a caller could
 * still want. A partial dataset handed back would be worse than a refusal — a measurement over
 * three frames of a four-frame ring is a number, and a wrong one.
 */
class OwnedFrames {
 public:
  explicit OwnedFrames(IFrameStoreAccess& store) : store_(store) {}
  ~OwnedFrames() {
    if (committed_) return;
    for (const FrameRef& frame : frames_) (void)store_.Forget(frame);
  }
  OwnedFrames(const OwnedFrames&) = delete;
  OwnedFrames& operator=(const OwnedFrames&) = delete;

  void Keep(const FrameRef& frame) { frames_.push_back(frame); }
  void Commit() { committed_ = true; }

 private:
  IFrameStoreAccess& store_;
  std::vector<FrameRef> frames_;
  bool committed_ = false;
};

/**
 * The next Netpbm token: whitespace-separated, with `#` comments running to end of line.
 *
 * Comments are part of the format even though this generator never writes one, and a reader that
 * choked on them would be refusing a valid file. Everything else about the header is checked
 * strictly, so accepting the one thing the standard requires costs nothing.
 */
bool ReadToken(std::istream& in, std::string* token) {
  token->clear();
  int c = in.get();
  while (in.good()) {
    if (c == '#') {
      while (in.good() && c != '\n') c = in.get();
      continue;
    }
    if (!std::isspace(static_cast<unsigned char>(c))) break;
    c = in.get();
  }
  if (!in.good()) return false;
  while (in.good() && !std::isspace(static_cast<unsigned char>(c))) {
    token->push_back(static_cast<char>(c));
    c = in.get();
  }
  // The whitespace that ended the token is put back, not swallowed. Netpbm separates the header
  // from the payload with **exactly one** whitespace byte, so the caller has to consume that byte
  // itself — and a reader that had already eaten it went on to take the first pixel as the
  // separator, which shortens every frame by one byte and fails at the last row with a message
  // about the header. That is what the first version of this did.
  if (in.good()) in.unget();
  return !token->empty();
}

bool ReadNumber(std::istream& in, int64_t* value) {
  std::string token;
  if (!ReadToken(in, &token)) return false;
  if (token.find_first_not_of("0123456789") != std::string::npos) return false;
  try {
    *value = std::stoll(token);
  } catch (...) {
    return false;
  }
  return true;
}

/** One P6 file, read into a frame of `lens`'s shape. */
Result<FrameRef> ReadFrame(IFrameStoreAccess& store, const fs::path& path, const Intrinsics& lens) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return Err<FrameRef>(StatusCode::NotFound, kComponent, "cannot open " + path.string());

  std::string magic;
  if (!ReadToken(in, &magic) || magic != "P6") {
    return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                         path.filename().string() + " is not a P6 Netpbm; the generator writes "
                         "binary colour and nothing here reads another kind");
  }
  int64_t width = 0;
  int64_t height = 0;
  int64_t maxValue = 0;
  if (!ReadNumber(in, &width) || !ReadNumber(in, &height) || !ReadNumber(in, &maxValue)) {
    return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                         path.filename().string() + " has a header this reader cannot parse");
  }
  // Exactly one whitespace byte separates the header from the payload, and it is *not* skipped as
  // ordinary whitespace: a payload may legitimately begin with a byte that looks like one.
  in.get();

  if (maxValue != 255) {
    return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                         "only 8-bit samples are read; this frame's maximum is " +
                             std::to_string(maxValue) + ", which is two bytes a sample");
  }
  if (width != lens.width || height != lens.height) {
    // Every frame in a dataset is rendered through one lens, so a frame of another size is either
    // stale or belongs to another dataset. Reading it against the recorded intrinsics would put
    // every feature in the wrong place while the file looked perfectly well-formed.
    return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                         path.filename().string() + " is " + std::to_string(width) + "x" +
                             std::to_string(height) + " and truth.json records a " +
                             std::to_string(lens.width) + "x" + std::to_string(lens.height) +
                             " lens");
  }

  const Result<FrameRef> allocated = store.Allocate(lens.width, lens.height, PixelFormat::RGBA8);
  if (!allocated.ok()) return allocated;
  const FrameRef frame = allocated.value;

  const Result<std::span<uint8_t>> pinned = store.Pin(frame);
  if (!pinned.ok()) {
    (void)store.Forget(frame);
    return Err<FrameRef>(pinned.status.code, kComponent, pinned.status.detail);
  }

  // Row at a time, straight into the pinned span. Reading the whole file first and copying would
  // hold a second image of every frame at once, which on a sixty-frame dataset of real captures is
  // the difference between a measurement and an out-of-memory.
  std::vector<uint8_t> row(static_cast<size_t>(lens.width) * 3);
  bool short_read = false;
  for (int32_t y = 0; y < lens.height && !short_read; ++y) {
    in.read(reinterpret_cast<char*>(row.data()), static_cast<std::streamsize>(row.size()));
    if (in.gcount() != static_cast<std::streamsize>(row.size())) {
      short_read = true;
      break;
    }
    uint8_t* out = pinned.value.data() + static_cast<size_t>(y) * frame.stride;
    for (int32_t x = 0; x < lens.width; ++x) {
      const size_t at = static_cast<size_t>(x) * 3;
      out[x * 4] = row[at];
      out[x * 4 + 1] = row[at + 1];
      out[x * 4 + 2] = row[at + 2];
      out[x * 4 + 3] = 255;
    }
  }

  // Strict in both directions. A file with bytes to spare is not the frame its header describes,
  // and taking the first `w * h * 3` of it succeeds silently while reading a different picture.
  const bool trailing = !short_read && in.peek() != std::char_traits<char>::eof();
  (void)store.Release(frame);
  if (short_read || trailing) {
    (void)store.Forget(frame);
    return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                         path.filename().string() + (short_read
                             ? " ends before the pixels its header promises"
                             : " carries more bytes than its header accounts for"));
  }
  return Ok(frame);
}

double NodeDouble(const cv::FileNode& node, const char* name, bool* ok) {
  const cv::FileNode field = node[name];
  if (field.empty() || !field.isReal()) {
    // An integer in the file is a real number that happened to be written without a point, which
    // is how `0.0` arrives after `json.dumps`.
    if (!field.empty() && field.isInt()) return static_cast<double>(static_cast<int>(field));
    *ok = false;
    return 0.0;
  }
  return static_cast<double>(field);
}

}  // namespace

Result<SyntheticDataset> LoadSyntheticDataset(IFrameStoreAccess& store,
                                              const std::string& directory) {
  std::error_code ignored;
  if (!fs::is_directory(fs::path(directory), ignored)) {
    return Err<SyntheticDataset>(StatusCode::NotFound, kComponent,
                                 "no such dataset directory: " + directory);
  }
  const fs::path truthPath = fs::path(directory) / "truth.json";
  if (!fs::is_regular_file(truthPath, ignored)) {
    return Err<SyntheticDataset>(StatusCode::NotFound, kComponent,
                                 "no truth.json in " + directory +
                                     "; a directory of frames with nothing saying where they were "
                                     "taken is not a dataset this can score against");
  }

  // OpenCV's reader rather than a JSON parser written here. It is already a dependency of every
  // consumer of this file — registration exists only where OpenCV does (ADR 0052) — and a
  // hand-rolled parser is a second thing to get wrong for no gain. Measured against the real
  // `truth.json` before it was chosen: it reads the nested `intrinsics` object, the `frames`
  // sequence and each `rotation` correctly.
  cv::FileStorage file;
  try {
    if (!file.open(truthPath.string(), cv::FileStorage::READ | cv::FileStorage::FORMAT_JSON)) {
      return Err<SyntheticDataset>(StatusCode::InvalidArgument, kComponent,
                                   "truth.json could not be opened as JSON");
    }
  } catch (const cv::Exception& thrown) {
    return Err<SyntheticDataset>(StatusCode::InvalidArgument, kComponent,
                                 std::string("truth.json is not readable JSON: ") + thrown.what());
  }

  SyntheticDataset dataset;
  OwnedFrames owned(store);
  try {
    const cv::FileNode intrinsics = file["intrinsics"];
    if (intrinsics.empty()) {
      return Err<SyntheticDataset>(StatusCode::InvalidArgument, kComponent,
                                   "truth.json records no intrinsics, so nothing knows what lens "
                                   "these frames were rendered through");
    }
    bool ok = true;
    dataset.lens.fx = NodeDouble(intrinsics, "fx", &ok);
    dataset.lens.fy = NodeDouble(intrinsics, "fy", &ok);
    dataset.lens.cx = NodeDouble(intrinsics, "cx", &ok);
    dataset.lens.cy = NodeDouble(intrinsics, "cy", &ok);
    dataset.lens.k1 = NodeDouble(intrinsics, "k1", &ok);
    dataset.lens.k2 = NodeDouble(intrinsics, "k2", &ok);
    dataset.lens.k3 = NodeDouble(intrinsics, "k3", &ok);
    dataset.lens.p1 = NodeDouble(intrinsics, "p1", &ok);
    dataset.lens.p2 = NodeDouble(intrinsics, "p2", &ok);
    const cv::FileNode widthNode = intrinsics["width"];
    const cv::FileNode heightNode = intrinsics["height"];
    if (!ok || widthNode.empty() || heightNode.empty()) {
      return Err<SyntheticDataset>(StatusCode::InvalidArgument, kComponent,
                                   "truth.json's intrinsics are missing a field this reader needs");
    }
    dataset.lens.width = static_cast<int32_t>(static_cast<int>(widthNode));
    dataset.lens.height = static_cast<int32_t>(static_cast<int>(heightNode));
    if (dataset.lens.width <= 0 || dataset.lens.height <= 0) {
      return Err<SyntheticDataset>(StatusCode::InvalidArgument, kComponent,
                                   "truth.json records a lens with no pixels in it");
    }

    const cv::FileNode frames = file["frames"];
    if (!frames.isSeq()) {
      return Err<SyntheticDataset>(StatusCode::InvalidArgument, kComponent,
                                   "truth.json records no sequence of frames");
    }
    for (size_t at = 0; at < frames.size(); ++at) {
      const cv::FileNode entry = frames[static_cast<int>(at)];
      const cv::FileNode fileNode = entry["file"];
      const cv::FileNode rotation = entry["rotation"];
      if (fileNode.empty() || !fileNode.isString() || rotation.empty()) {
        return Err<SyntheticDataset>(StatusCode::InvalidArgument, kComponent,
                                     "a frame entry in truth.json has no file or no rotation");
      }
      bool spelled = true;
      SyntheticFrame frame;
      frame.trueRotation.w = NodeDouble(rotation, "w", &spelled);
      frame.trueRotation.x = NodeDouble(rotation, "x", &spelled);
      frame.trueRotation.y = NodeDouble(rotation, "y", &spelled);
      frame.trueRotation.z = NodeDouble(rotation, "z", &spelled);
      if (!spelled) {
        return Err<SyntheticDataset>(StatusCode::InvalidArgument, kComponent,
                                     "a frame's rotation is missing a component");
      }

      const Result<FrameRef> read =
          ReadFrame(store, fs::path(directory) / static_cast<std::string>(fileNode), dataset.lens);
      if (!read.ok()) return Err<SyntheticDataset>(read.status.code, kComponent, read.status.detail);
      owned.Keep(read.value);
      frame.frame = read.value;
      dataset.frames.push_back(frame);
    }
  } catch (const cv::Exception& thrown) {
    return Err<SyntheticDataset>(StatusCode::InvalidArgument, kComponent,
                                 std::string("truth.json is shaped unexpectedly: ") + thrown.what());
  }

  owned.Commit();
  return Ok(std::move(dataset));
}

}  // namespace sphanorama
