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
// Nothing legitimate in a Netpbm header is long: a magic number and three decimal integers. The cap
// is what stops a file with no whitespace byte in it from being read *whole* into memory before
// `magic != "P6"` ever runs — measured by a reviewer at 988 MB resident for a 512 MiB file, in a
// function whose own payload loop twenty lines below exists to avoid precisely that.
constexpr size_t kLongestHeaderToken = 32;

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
    if (token->size() >= kLongestHeaderToken) return false;
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

/**
 * One frame, released and forgotten unless the read commits it.
 *
 * `OwnedFrames` below covers the frames a *dataset* collected; this covers the window inside one
 * read, which the first version left open. Between `Pin` succeeding and the last row being copied
 * there is an allocation, and this translation unit is `-fexceptions` — so a `std::bad_alloc` there
 * escaped with the frame still **pinned**, which is worse than an ordinary leak: `Forget` refuses a
 * pinned frame and `Clear` refuses while any frame is pinned, so those bytes were unrecoverable.
 * `FeatureRegistrationEngine::OwnedFrame` exists for this exact window; the outer holder was
 * mirrored from it and the inner rollback was written by hand, which is how the gap got in.
 */
class HeldFrame {
 public:
  HeldFrame(IFrameStoreAccess& store, const FrameRef& frame) : store_(store), frame_(frame) {}
  ~HeldFrame() {
    if (pinned_) (void)store_.Release(frame_);
    if (!committed_) (void)store_.Forget(frame_);
  }
  HeldFrame(const HeldFrame&) = delete;
  HeldFrame& operator=(const HeldFrame&) = delete;

  Result<std::span<uint8_t>> Pin() {
    Result<std::span<uint8_t>> pinned = store_.Pin(frame_);
    if (pinned.ok()) pinned_ = true;
    return pinned;
  }
  void Release() {
    if (!pinned_) return;
    (void)store_.Release(frame_);
    pinned_ = false;
  }
  void Commit() { committed_ = true; }

 private:
  IFrameStoreAccess& store_;
  FrameRef frame_;
  bool pinned_ = false;
  bool committed_ = false;
};

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
  HeldFrame held(store, frame);

  const Result<std::span<uint8_t>> pinned = held.Pin();
  if (!pinned.ok()) return Err<FrameRef>(pinned.status.code, kComponent, pinned.status.detail);

  // **What the store actually handed over, against what the handle claims.** `Allocate` promises
  // nothing about stride, so the span and `frame.stride` are two different authorities and the
  // write loop below indexes by the second. A reviewer reproduced an ASan heap-buffer-*write*
  // overflow with a forwarding store that padded the stride by 64. Asked by subtraction rather than
  // by forming `height * stride`, which is the product this check exists to avoid trusting.
  const int64_t rowBytes = static_cast<int64_t>(lens.width) * 4;
  const int64_t held_bytes = static_cast<int64_t>(pinned.value.size());
  const int64_t rows = static_cast<int64_t>(lens.height) - 1;
  if (frame.stride < rowBytes || (rows > 0 && frame.stride > (held_bytes - rowBytes) / rows) ||
      (rows == 0 && rowBytes > held_bytes)) {
    return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                         "the store handed back fewer bytes than a frame of this shape needs");
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
  held.Release();
  if (short_read || trailing) {
    return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                         path.filename().string() + (short_read
                             ? " ends before the pixels its header promises"
                             : " carries more bytes than its header accounts for"));
  }
  held.Commit();
  return Ok(frame);
}

/**
 * An integer field, refusing anything that is not one.
 *
 * `static_cast<int>` of a `FileNode` that is not a number answers **`0x7FFFFFFF`** — measured, not
 * assumed — and `INT_MAX` sails past a `<= 0` guard. So `"width": "not a number"` reached
 * `Allocate(2147483647, 3, RGBA8)` and came back `FrameStoreExhausted`, telling the caller the store
 * was full when the truth was that the file was malformed, and breaking the `InvalidArgument` this
 * header promises. Every other intrinsic already went through `NodeDouble`'s flag; these two were
 * checked only for presence.
 */
int32_t NodeInt(const cv::FileNode& node, const char* name, bool* ok) {
  const cv::FileNode field = node[name];
  if (field.empty() || !field.isInt()) {
    *ok = false;
    return 0;
  }
  return static_cast<int32_t>(static_cast<int>(field));
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
    // **Kept, and no test reaches it — measured rather than assumed.** `open` returns `bool` in
    // OpenCV's API, and in 4.10.0 it does not use it for malformed input: an empty file, a file of
    // prose and a truncated object each *throw* and land in the catch below. Nothing this loader can
    // be handed reaches the `false`, and a permissions failure does not either where the tests run
    // as root. It stays because the signature is the contract — a version that returns `false`
    // instead would otherwise leave this reading an unopened `FileStorage` — and this comment is
    // here so the next reader can tell a guard that was considered from one nobody noticed.
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
    dataset.lens.width = NodeInt(intrinsics, "width", &ok);
    dataset.lens.height = NodeInt(intrinsics, "height", &ok);
    if (!ok) {
      return Err<SyntheticDataset>(StatusCode::InvalidArgument, kComponent,
                                   "truth.json's intrinsics are missing a field this reader needs, "
                                   "or one of them is not a number");
    }
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

      // A name, not a path. `fs::path(dir) / "/etc/passwd"` *replaces* rather than appends, so an
      // absolute `file` silently reads from outside the dataset, and `..` walks out of it. Nothing
      // writes such a dataset, and a directory is not a boundary anybody has promised to hold — but
      // a loader that follows whatever a file tells it to is the wrong default even in test support.
      const std::string named = static_cast<std::string>(fileNode);
      if (fs::path(named).has_parent_path() || named.find("..") != std::string::npos) {
        return Err<SyntheticDataset>(StatusCode::InvalidArgument, kComponent,
                                     "a frame entry names a path rather than a file in the dataset: "
                                         + named);
      }

      const Result<FrameRef> read = ReadFrame(store, fs::path(directory) / named, dataset.lens);
      if (!read.ok()) return Err<SyntheticDataset>(read.status.code, kComponent, read.status.detail);
      owned.Keep(read.value);
      frame.frame = read.value;
      dataset.frames.push_back(frame);
    }
  } catch (const cv::Exception& thrown) {
    return Err<SyntheticDataset>(StatusCode::InvalidArgument, kComponent,
                                 std::string("truth.json is shaped unexpectedly: ") + thrown.what());
  } catch (const std::exception& thrown) {
    // Not only OpenCV's. Every caller of this is compiled `-fno-exceptions`, so anything escaping
    // here is a terminate rather than a failure — and `push_back` on the frame vector allocates, so
    // `std::bad_alloc` is a real way out of this block and not a theoretical one. The engine this
    // boundary is modelled on catches both for the same reason.
    //
    // No test reaches this one, and that is a gap rather than a comfort: reaching it needs a
    // throwing allocator, which is how a reviewer demonstrated the leak this arm now closes. The
    // state is reachable in life — a sixty-frame dataset of real captures is where an allocation
    // fails — so the arm stays and the missing test is named here instead of being implied.
    return Err<SyntheticDataset>(StatusCode::Internal, kComponent,
                                 std::string("reading the dataset failed: ") + thrown.what());
  }

  // An empty `frames` array parsed cleanly and loaded as a *successful* dataset of nothing, which a
  // harness would go on to score: a median over no frames is a number nobody should be shown.
  if (dataset.frames.empty()) {
    return Err<SyntheticDataset>(StatusCode::InvalidArgument, kComponent,
                                 "truth.json names no frames, so there is nothing here to score");
  }

  owned.Commit();
  return Ok(std::move(dataset));
}

}  // namespace sphanorama
