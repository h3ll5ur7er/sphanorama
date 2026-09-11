#include "support/synthetic_dataset.h"

#include <opencv2/core.hpp>

#include <cmath>
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
    (void)Rollback();
  }
  OwnedFrames(const OwnedFrames&) = delete;
  OwnedFrames& operator=(const OwnedFrames&) = delete;

  /**
   * Room for `count` frames, taken before the first one is read.
   *
   * This is what makes `Keep` not allocate, and that is the whole point of it. A frame is this
   * function's property from the moment `ReadFrame` commits it to the moment `push_back` records
   * it — and a *growing* `push_back` allocates inside that window, so a `std::bad_alloc` there
   * leaves the frame held by nothing and the rollback below gives back every frame except the one
   * just read. Three reviewers found this window independently; a throwing allocator swept over a
   * full load of the committed fixture put it at 3 of 123 allocation points, each leaving exactly
   * one 48x36 frame — 6,912 bytes — in a store the caller was told was untouched.
   *
   * Round 1 closed this same window one level down, inside `ReadFrame`, and opened this one
   * directly above it. `dataset.frames` is reserved beside this for the same reason: it grows in
   * the same window, one line later.
   */
  void Reserve(size_t count) { frames_.reserve(count); }

  void Keep(const FrameRef& frame) { frames_.push_back(frame); }
  void Commit() { committed_ = true; }

  /**
   * Gives every frame back, and answers whether the store took them.
   *
   * `Forget` can refuse: `MemoryFrameStoreAccess` returns the sink's own refusal when `Drop` fails
   * and deliberately keeps the entry, so its totals go on accounting for bytes nobody holds a
   * handle to. The header's "a refusal allocates nothing" cannot be kept against a store like that,
   * and the caller — who is handed no frames — has no way to find out. So the refusal says so
   * instead of a `(void)` swallowing it.
   */
  bool Rollback() {
    bool all = true;
    for (const FrameRef& frame : frames_) {
      if (!store_.Forget(frame).ok()) all = false;
    }
    frames_.clear();
    return all;
  }

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
// `magic != "P6"` ever runs — in a function whose own payload loop twenty lines below exists to
// avoid precisely that.
//
// Measured, twice. A 512 MiB file of non-whitespace bytes takes the uncapped reader to a peak RSS
// of **986,740 KiB — 964 MiB**. The first version of this comment said "988 MB", which was
// `ru_maxrss` divided by 1000: `ru_maxrss` is in KiB, so the number was neither MiB nor MB. The
// guard was always justified; the unit was wrong, and a reviewer re-ran it.
constexpr size_t kLongestHeaderToken = 32;

/**
 * Why a token could not be read, so a refusal can name the guard that produced it.
 *
 * Three different things make `ReadToken` answer `false`, and while the caller turned all three
 * into one sentence, the test for the length cap was satisfied by whichever of them fired — the
 * same defect, in the same file, that round 1 found and round 1's own fix reproduced.
 */
enum class TokenTrouble { kNone, kNothingThere, kTooLong };

bool ReadToken(std::istream& in, std::string* token, TokenTrouble* why) {
  *why = TokenTrouble::kNone;
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
  if (!in.good()) {
    *why = TokenTrouble::kNothingThere;
    return false;
  }
  while (in.good() && !std::isspace(static_cast<unsigned char>(c))) {
    if (token->size() >= kLongestHeaderToken) {
      *why = TokenTrouble::kTooLong;
      return false;
    }
    token->push_back(static_cast<char>(c));
    c = in.get();
  }
  // The whitespace that ended the token is put back, not swallowed. Netpbm separates the header
  // from the payload with **exactly one** whitespace byte, so the caller has to consume that byte
  // itself — and a reader that had already eaten it went on to take the first pixel as the
  // separator, which shortens every frame by one byte and fails at the last row with a message
  // about the header. That is what the first version of this did.
  if (in.good()) in.unget();
  if (token->empty()) {
    *why = TokenTrouble::kNothingThere;
    return false;
  }
  return true;
}

bool ReadNumber(std::istream& in, int64_t* value) {
  std::string token;
  TokenTrouble why = TokenTrouble::kNone;
  if (!ReadToken(in, &token, &why)) return false;
  if (token.find_first_not_of("0123456789") != std::string::npos) return false;
  try {
    *value = std::stoll(token);
  } catch (...) {
    // Reachable, and by a file anyone can write: all-digits is checked above but *width* is not
    // bounded, so a twenty-digit header number is a valid token that overflows `long long` and
    // `std::stoll` throws `std::out_of_range`. The cap on token length is 32, which leaves plenty
    // of room for one. A reviewer found this arm carrying no reason while its two neighbours
    // carried theirs, which is what a reason is for.
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
    // Retried here, because `Release` clears `pinned_` only when the store agreed. A destructor has
    // nowhere to report a second refusal, but a frame this leaves pinned is one `Forget` and
    // `Clear` will both go on refusing, so the retry is worth its line.
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

  /**
   * Drops the pin, and answers whether the store took it.
   *
   * The flag follows the store rather than the intention. The first version of this set
   * `pinned_ = false` whichever way `Release` went, which made a refused release look like a
   * finished one: the destructor then skipped its retry, `Commit` suppressed the `Forget`, and the
   * load *succeeded* — handing back a frame still pinned, which `Forget` refuses and which makes
   * `Clear` refuse for the whole store. A leak that arrives through the success path is worse than
   * one that arrives through a refusal, because nothing is looking.
   *
   * `MemoryFrameStoreAccess::Release` cannot fail after a successful `Pin`, so no real store
   * reaches this today; `IFrameStoreAccess::Release` returns a `Status` because an implementation
   * may, and a test store supplies one.
   */
  Status Release() {
    if (!pinned_) return Status::Ok();
    Status released = store_.Release(frame_);
    if (released.ok()) pinned_ = false;
    return released;
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

  // Two refusals rather than one, and the reason is a test rather than a taste. While `ReadToken`
  // failing and the magic being wrong shared a message, the test named for the token cap was
  // satisfied by the P6 check: a 40-byte first token is both over the cap and not "P6", and the
  // assertion could not tell which had answered. That is the same defect round 1 found in the P5
  // test, reproduced by round 1's own fix. A distinct `detail` is what makes a guard's test able to
  // name it, which is why every refusal below carries one and every test asserts it.
  std::string magic;
  TokenTrouble why = TokenTrouble::kNone;
  if (!ReadToken(in, &magic, &why)) {
    return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                         path.filename().string() +
                             (why == TokenTrouble::kTooLong
                                  ? " opens with a token longer than any real header token"
                                  : " has no Netpbm magic number at all"));
  }
  if (magic != "P6") {
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
  const Status released = held.Release();
  if (!released.ok()) {
    // The pixels are read and correct, and the frame is unusable anyway: it is still pinned, so the
    // caller can neither `Forget` it nor `Clear` the store. Answering `Ok` here is what turned this
    // into a leak through the success path.
    return Err<FrameRef>(released.code, kComponent,
                         path.filename().string() + " was read, but the store would not release "
                         "the pin taken to write it, so the frame cannot be handed over or given "
                         "back: " + released.detail);
  }
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
 * Records which field went wrong, and how — the first one only, since that is the one that stopped
 * the read.
 *
 * A `bool` was enough to *refuse* and not enough to *say*, and the gap showed up as a pair of tests
 * that could not fail: a missing `width` and a `width` that is not a number produced one sentence
 * between them, so either guard could answer for the other and deleting one left the suite green.
 * A reviewer found eleven guards in this file with that property. Naming the field and the reason
 * is what lets a test assert that its own guard is the one that spoke.
 */
void Blame(std::string* trouble, const char* name, const char* how) {
  if (trouble->empty()) *trouble = std::string(name) + " " + how;
}

/**
 * An integer field, refusing anything OpenCV did not parse *as* an integer.
 *
 * `static_cast<int>` of a `FileNode` that is not a number answers **`0x7FFFFFFF`** — measured, not
 * assumed — and `INT_MAX` sails past a `<= 0` guard, so `"width": "not a number"` used to become a
 * lens 2,147,483,647 pixels wide. Every other intrinsic already went through `NodeDouble`'s flag;
 * these two were checked only for presence.
 *
 * **A withdrawn claim.** This comment used to say that width then reached
 * `Allocate(2147483647, 3, RGBA8)` and came back `FrameStoreExhausted`. It does not and never did:
 * the frame/lens dimension check refuses twelve lines before the `Allocate`, in that commit and in
 * its parent, with `"frame_0000.ppm is 48x36 and truth.json records a 2147483647x36 lens"`. The
 * guard is right and the story told about it was not measured.
 *
 * **What this still cannot catch**, because OpenCV's parser has already thrown it away: an integer
 * outside `int`'s range is wrapped to 32 bits *before* the node exists and `isInt()` stays true, so
 * `"width": 4294967344` arrives here as 48 and reads as 48.0 as a double too. There is no accessor
 * on `cv::FileNode` that sees the original text, so this is documented and pinned by a test rather
 * than fixed. What saves it in practice is that the wrapped value still has to agree with the PPM
 * header, which is checked.
 */
int32_t NodeInt(const cv::FileNode& node, const char* name, std::string* trouble) {
  const cv::FileNode field = node[name];
  if (field.empty()) {
    Blame(trouble, name, "is not in the file");
    return 0;
  }
  if (!field.isInt()) {
    Blame(trouble, name, "is not a whole number");
    return 0;
  }
  return static_cast<int32_t>(static_cast<int>(field));
}

double NodeDouble(const cv::FileNode& node, const char* name, std::string* trouble) {
  const cv::FileNode field = node[name];
  if (field.empty() || !field.isReal()) {
    // An integer in the file is a real number that happened to be written without a point, which
    // is how `0.0` arrives after `json.dumps`.
    if (!field.empty() && field.isInt()) return static_cast<double>(static_cast<int>(field));
    Blame(trouble, name, field.empty() ? "is not in the file" : "is not a number");
    return 0.0;
  }
  const double value = static_cast<double>(field);
  if (!std::isfinite(value)) {
    // `"cx": 1e400` parses as a real node holding `inf` — measured — and an infinite principal
    // point would travel silently into every projection the harness computes. A NaN cannot be
    // spelled in JSON, but `isfinite` costs nothing and does not have to argue about that.
    //
    // The sibling case is **not** fixable here and is recorded rather than guarded: JSON `true`
    // becomes an INT node holding 1, which is indistinguishable from `1`, so `"fx": true` loads a
    // focal length of 1.
    Blame(trouble, name, "is not a finite number");
    return 0.0;
  }
  return value;
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

  SyntheticDataset dataset;
  OwnedFrames owned(store);

  // One refusal shape for the whole function, because a refusal here has two jobs: say what was
  // wrong with the file, and give back the frames read before the failure. The second job used to
  // belong to `~OwnedFrames` alone, which throws away the store's answer — and `Forget` can refuse,
  // so the header's "a refusal allocates nothing" could quietly not be true with nobody able to
  // find out. Now the caller is told.
  auto refuse = [&owned](StatusCode code, std::string detail) {
    if (!owned.Rollback()) {
      detail += " (and the store then refused to give back a frame read before this failure, so "
                "its totals still account for bytes no handle names — only Clear recovers them)";
    }
    return Err<SyntheticDataset>(code, kComponent, std::move(detail));
  };

  // `cv::FileStorage`, rather than a parser written here. It is already linked wherever this can be
  // built — the OpenCV-backed registration engine is the only other consumer of this file, and
  // registration exists only where OpenCV does (ADR 0052) — and a hand-rolled parser is a second
  // thing to get wrong for no gain. Measured against the real `truth.json` before it was chosen: it
  // reads the nested `intrinsics` object, the `frames` sequence and each `rotation` correctly.
  cv::FileStorage file;
  try {
    // **Kept, and no test reaches it — and the reason is not the one first written here.** That
    // said malformed input never reaches the `false`, which is true: in OpenCV 4.10.0 an empty
    // file, a file of prose and a truncated object each *throw* and land in the catch below. But
    // `open` does return `false`, for a path it cannot open at all — and the only reason this
    // loader never sees that is the `is_regular_file` check twenty lines above, not anything about
    // OpenCV. A guard held up by a different guard is worth saying out loud, because deleting the
    // other one is what would make this reachable.
    if (!file.open(truthPath.string(), cv::FileStorage::READ | cv::FileStorage::FORMAT_JSON)) {
      return refuse(StatusCode::InvalidArgument, "truth.json could not be opened as JSON");
    }
  } catch (const cv::Exception& thrown) {
    return refuse(StatusCode::InvalidArgument,
                  std::string("truth.json is not readable JSON: ") + thrown.what());
  } catch (const std::exception& thrown) {
    // Widened to match the traversal's arm below, which had it and this did not. `open` parses the
    // whole file, so every allocation the parser makes is an escape route from here: a throwing
    // allocator swept over a full load reached the caller from 69 of 123 allocation points, most of
    // them in this call. Under `-fno-exceptions`, which is what every consumer other than this
    // file's own test is compiled with, an escape is a terminate rather than a failure.
    return refuse(StatusCode::Internal,
                  std::string("reading truth.json failed: ") + thrown.what());
  }

  try {
    const cv::FileNode intrinsics = file["intrinsics"];
    if (intrinsics.empty()) {
      return refuse(StatusCode::InvalidArgument,
                    "truth.json records no intrinsics, so nothing knows what lens these frames "
                    "were rendered through");
    }
    std::string trouble;
    dataset.lens.fx = NodeDouble(intrinsics, "fx", &trouble);
    dataset.lens.fy = NodeDouble(intrinsics, "fy", &trouble);
    dataset.lens.cx = NodeDouble(intrinsics, "cx", &trouble);
    dataset.lens.cy = NodeDouble(intrinsics, "cy", &trouble);
    dataset.lens.k1 = NodeDouble(intrinsics, "k1", &trouble);
    dataset.lens.k2 = NodeDouble(intrinsics, "k2", &trouble);
    dataset.lens.k3 = NodeDouble(intrinsics, "k3", &trouble);
    dataset.lens.p1 = NodeDouble(intrinsics, "p1", &trouble);
    dataset.lens.p2 = NodeDouble(intrinsics, "p2", &trouble);
    dataset.lens.width = NodeInt(intrinsics, "width", &trouble);
    dataset.lens.height = NodeInt(intrinsics, "height", &trouble);
    if (!trouble.empty()) {
      return refuse(StatusCode::InvalidArgument, "truth.json's intrinsics: " + trouble);
    }
    if (dataset.lens.width <= 0 || dataset.lens.height <= 0) {
      return refuse(StatusCode::InvalidArgument, "truth.json records a lens with no pixels in it");
    }

    // **The one string in this file whose violation is invisible.** `truth.json` carries a
    // `convention` block the generator writes and, until a reviewer's key-set assertion found it,
    // nothing in C++ read a word of it — including `rotation`, which says which way round the
    // quaternions go. Get that wrong and nothing breaks: the frames load, the scorer runs, and every
    // accuracy number it produces is wrong by the inverse of the rotation. That is the failure this
    // whole harness exists to make impossible, arriving through the file it reads.
    //
    // Checked by exact text rather than parsed, because there is nothing to parse: it is prose, and
    // prose that changed meaning is exactly what a reader must not accept quietly. Rewording it is
    // a decision, so it fails here and again in `tools/test_synth_dataset.py`, on the writer's side.
    //
    // The other `convention` entries — camera space, image space, principal point, equirectangular
    // layout, pixel encoding — are carried and not checked here. None of them is consumed by this
    // loader, which copies bytes; the first thing to compute on those pixels has to check the one it
    // relies on, and that is `pixel_encoding` for whatever reads a frame as signed components.
    const cv::FileNode convention = file["convention"];
    const cv::FileNode spelledRotation = convention.isMap() ? convention["rotation"] : cv::FileNode();
    static constexpr const char* kRotationConvention =
        "device -> world, unit quaternion, matching sphanorama::Quat";
    if (spelledRotation.empty() || !spelledRotation.isString() ||
        static_cast<std::string>(spelledRotation) != kRotationConvention) {
      return refuse(StatusCode::InvalidArgument,
                    std::string("truth.json does not state the rotation convention this reader "
                                "assumes; it must spell convention.rotation as \"") +
                        kRotationConvention + "\"");
    }

    const cv::FileNode frames = file["frames"];
    if (!frames.isSeq()) {
      return refuse(StatusCode::InvalidArgument, "truth.json records no sequence of frames");
    }
    // Reserved before the first frame is read, and walked by iterator rather than by index. The
    // reserve is what keeps `Keep` from allocating in the window where a frame is owned by nothing
    // (see `OwnedFrames::Reserve`); `dataset.frames` grows in the same window and gets the same
    // treatment. The iterator is a separate point: `frames[i]` on a `cv::FileNode` sequence walks
    // from the front every time, so indexing a sequence in a loop is quadratic — a reviewer watched
    // 200,000 entries fail to finish in two minutes where 50,000 took seconds.
    owned.Reserve(frames.size());
    dataset.frames.reserve(frames.size());
    for (cv::FileNodeIterator at = frames.begin(); at != frames.end(); ++at) {
      const cv::FileNode entry = *at;
      const cv::FileNode fileNode = entry["file"];
      const cv::FileNode rotation = entry["rotation"];
      if (fileNode.empty() || !fileNode.isString() || rotation.empty()) {
        return refuse(StatusCode::InvalidArgument,
                      "a frame entry in truth.json has no file or no rotation");
      }
      std::string spelling;
      SyntheticFrame frame;
      frame.trueRotation.w = NodeDouble(rotation, "w", &spelling);
      frame.trueRotation.x = NodeDouble(rotation, "x", &spelling);
      frame.trueRotation.y = NodeDouble(rotation, "y", &spelling);
      frame.trueRotation.z = NodeDouble(rotation, "z", &spelling);
      if (!spelling.empty()) {
        return refuse(StatusCode::InvalidArgument, "a frame's rotation: " + spelling);
      }

      // A name, not a path. `fs::path(dir) / "/etc/passwd"` *replaces* rather than appends, so an
      // absolute `file` silently reads from outside the dataset, and `..` walks out of it. Nothing
      // writes such a dataset, and a directory is not a boundary anybody has promised to hold — but
      // a loader that follows whatever a file tells it to is the wrong default even in test support.
      const std::string named = static_cast<std::string>(fileNode);
      if (fs::path(named).has_parent_path() || named.find("..") != std::string::npos) {
        return refuse(StatusCode::InvalidArgument,
                      "a frame entry names a path rather than a file in the dataset: " + named);
      }

      const Result<FrameRef> read = ReadFrame(store, fs::path(directory) / named, dataset.lens);
      if (!read.ok()) return refuse(read.status.code, read.status.detail);
      owned.Keep(read.value);
      frame.frame = read.value;
      dataset.frames.push_back(frame);
    }
  } catch (const cv::Exception& thrown) {
    return refuse(StatusCode::InvalidArgument,
                  std::string("truth.json is shaped unexpectedly: ") + thrown.what());
  } catch (const std::exception& thrown) {
    // Not only OpenCV's. Every consumer of this file other than its own test is compiled
    // `-fno-exceptions`, so anything escaping here is a terminate rather than a failure — and
    // `push_back` on the frame vector allocates, so `std::bad_alloc` is a real way out of this
    // block and not a theoretical one. The engine this boundary is modelled on catches both for the
    // same reason.
    //
    // No test in the suite reaches this one. `synthetic_dataset_alloc_test` does, from outside it:
    // it replaces global `operator new` and sweeps a throw across every allocation of a full load,
    // which is the only instrument that reaches these arms at all and which found the two windows
    // round 2 closed.
    return refuse(StatusCode::Internal,
                  std::string("reading the dataset failed: ") + thrown.what());
  }

  // An empty `frames` array parsed cleanly and loaded as a *successful* dataset of nothing, which a
  // harness would go on to score: a median over no frames is a number nobody should be shown.
  if (dataset.frames.empty()) {
    return refuse(StatusCode::InvalidArgument,
                  "truth.json names no frames, so there is nothing here to score");
  }

  owned.Commit();
  return Ok(std::move(dataset));
}

}  // namespace sphanorama
