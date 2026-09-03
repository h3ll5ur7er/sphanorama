#pragma once

#include <cstdint>
#include <map>
#include <span>
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

  // Test affordances.
  bool Holds(uint64_t frame) const { return held_.count(frame) != 0; }
  const std::vector<uint8_t>& Held(uint64_t frame) const { return held_.at(frame); }
  int Writes() const { return writes_; }
  int Drops() const { return drops_; }
  void FailWrites(bool fail) { fail_writes_ = fail; }
  void FailReads(bool fail) { fail_reads_ = fail; }
  void FailDrops(bool fail) { fail_drops_ = fail; }

 private:
  std::map<uint64_t, std::vector<uint8_t>> held_;
  int writes_ = 0;
  int drops_ = 0;
  bool fail_writes_ = false;
  bool fail_reads_ = false;
  bool fail_drops_ = false;
};

}  // namespace sphanorama
