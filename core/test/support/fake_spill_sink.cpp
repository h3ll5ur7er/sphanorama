#include "support/fake_spill_sink.h"

#include <algorithm>

namespace sphanorama {
namespace {
constexpr const char* kComponent = "FakeSpillSink";
}  // namespace

Status FakeSpillSink::Write(uint64_t frame, std::span<const uint8_t> bytes) {
  if (fail_writes_) {
    // Refused *and* stored nothing, which is the case worth modelling: a sink that half-wrote and
    // reported failure would leave the store choosing between two wrong answers.
    return Fail(StatusCode::FrameStoreExhausted, kComponent, write_refusal_);
  }
  held_[frame].assign(bytes.begin(), bytes.end());
  ++writes_;
  return Status::Ok();
}

Status FakeSpillSink::Read(uint64_t frame, std::span<uint8_t> bytes) {
  if (fail_reads_) return Fail(StatusCode::Internal, kComponent, "the handle would not read");
  const auto it = held_.find(frame);
  if (it == held_.end()) return Fail(StatusCode::NotFound, kComponent, "nothing spilled here");
  if (it->second.size() != bytes.size()) {
    return Fail(StatusCode::Internal, kComponent, "the spilled frame is a different size");
  }
  std::copy(it->second.begin(), it->second.end(), bytes.begin());
  return Status::Ok();
}

Status FakeSpillSink::Drop(uint64_t frame) {
  // Refused before the erase, deliberately: a real handle that cannot free a slot still holds the
  // bytes, and the store's own answer to that is to keep the entry so the budget keeps accounting
  // for them. A fake that dropped the frame anyway would let that be true only here.
  if (fail_drops_) return Fail(StatusCode::Internal, kComponent, "the handle would not free this");
  ++drops_;
  held_.erase(frame);
  return Status::Ok();
}

Status FakeSpillSink::Clear() {
  // Refused before the erase, for the same reason Drop is: the store's answer to a tier that will
  // not let go is to keep its entries, and a fake that emptied itself anyway would make the
  // store's refusal path untestable — every assertion after it would pass for the wrong reason.
  if (fail_clears_) return Fail(StatusCode::Internal, kComponent, "the handle would not empty");
  ++clears_;
  held_.clear();
  // A refused clear leaves this alone, which the early return above is what provides: a tier that
  // did not empty is still holding the capture its token names, and moving it on would strand
  // that capture's document over pixels that are still there.
  ++generation_;
  return Status::Ok();
}

Result<uint64_t> FakeSpillSink::Generation() {
  if (fail_generations_) {
    return Err<uint64_t>(StatusCode::Unsupported, kComponent,
                         "this tier cannot say which capture it holds");
  }
  return Ok(generation_);
}

}  // namespace sphanorama
