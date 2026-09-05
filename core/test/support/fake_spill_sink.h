#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "resource_access/frame_store_access/spill_sink.h"

namespace sphanorama {

// A spill sink that keeps what it is given, so a test can assert the bytes actually left the
// store rather than only that a counter moved. Its failure switches exist because every
// interesting thing a real sink does — an OPFS handle on a phone with no quota left — is a
// failure, and a sink that cannot fail would let the store's recovery paths go untested.
class FakeSpillSink final : public ISpillSink {
 public:
  Status Write(uint64_t frame, std::span<const uint8_t> bytes) override;
  Status Read(uint64_t frame, std::span<uint8_t> bytes) override;
  Status Drop(uint64_t frame) override;
  Status Clear() override;
  Result<uint64_t> Generation() override;

  // Test affordances.
  bool Holds(uint64_t frame) const { return held_.count(frame) != 0; }
  const std::vector<uint8_t>& Held(uint64_t frame) const { return held_.at(frame); }
  int Writes() const { return writes_; }
  int Drops() const { return drops_; }
  // The detail carried by a refused write, settable to nothing on purpose: `Fail`'s detail
  // argument defaults to empty, so a sink that refuses and says nothing is conforming rather than
  // broken, and a store that cannot tell that from "no refusal at all" reports only the ceiling.
  void FailWrites(bool fail, std::string detail = "no room to spill this frame") {
    fail_writes_ = fail;
    write_refusal_ = std::move(detail);
  }
  void FailReads(bool fail) { fail_reads_ = fail; }
  void FailDrops(bool fail) { fail_drops_ = fail; }
  void FailClears(bool fail) { fail_clears_ = fail; }
  // A tier that cannot say which capture it holds, which is a page whose worker script is older
  // than the module calling it — the same half-updated page the browser sink's `clear` guards
  // against. Without it the store's and the manager's answers to that are unreachable.
  void FailGenerations(bool fail) { fail_generations_ = fail; }
  // How many frames are still down here. `Holds` answers about one frame; a clear is about all of
  // them, and a test that only checked the frame it knew about could not tell a sink that dropped
  // everything from one that dropped the entry it was asked about last.
  size_t HeldCount() const { return held_.size(); }
  int Clears() const { return clears_; }

 private:
  std::map<uint64_t, std::vector<uint8_t>> held_;
  int writes_ = 0;
  int drops_ = 0;
  bool fail_writes_ = false;
  std::string write_refusal_ = "no room to spill this frame";
  bool fail_reads_ = false;
  bool fail_drops_ = false;
  int clears_ = 0;
  bool fail_clears_ = false;
  // Counted rather than random, unlike the browser's: a test wants two tokens it can tell apart,
  // and the property under test is that a clear changes this — never what the number is. It
  // starts at 1 because zero is the store's answer for a tier that does not exist.
  uint64_t generation_ = 1;
  bool fail_generations_ = false;
};

}  // namespace sphanorama
