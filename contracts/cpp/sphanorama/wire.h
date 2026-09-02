#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// Wire primitives for the generated boundary codec.
//
// Hand-written on purpose: this is the small, fiddly layer where a bounds check either exists or
// does not, and generating it would mean reviewing generated bounds checks. Everything above it —
// the per-type encoders — is generated from the contracts so the two sides cannot disagree.
//
// Little-endian, because every platform this runs on is, and because the JavaScript side reads
// through DataView with littleEndian=true.
//
// Reader never throws and never reads past the end: a truncated or hostile payload sets `failed`
// and every subsequent read is a no-op returning zero. Callers check ok() once at the end rather
// than after every field (ADR 0006 — no exceptions cross this boundary).
namespace sphanorama::wire {

class Writer {
 public:
  void PutBool(bool v) { bytes_.push_back(v ? 1u : 0u); }

  void PutI32(int32_t v) { Raw(&v, sizeof(v)); }
  void PutU64(uint64_t v) { Raw(&v, sizeof(v)); }
  void PutF64(double v) { Raw(&v, sizeof(v)); }

  void PutString(std::string_view v) {
    PutI32(static_cast<int32_t>(v.size()));
    bytes_.insert(bytes_.end(), v.begin(), v.end());
  }

  void PutBytes(const std::vector<uint8_t>& v) {
    PutI32(static_cast<int32_t>(v.size()));
    bytes_.insert(bytes_.end(), v.begin(), v.end());
  }

  void PutCount(size_t count) { PutI32(static_cast<int32_t>(count)); }

  const std::vector<uint8_t>& bytes() const { return bytes_; }

 private:
  void Raw(const void* source, size_t size) {
    const size_t at = bytes_.size();
    bytes_.resize(at + size);
    std::memcpy(bytes_.data() + at, source, size);
  }

  std::vector<uint8_t> bytes_;
};

class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool ok() const { return !failed_; }

  bool GetBool() {
    const uint8_t byte = RawByte();
    return byte != 0u;
  }

  int32_t GetI32() { return Raw<int32_t>(); }
  uint64_t GetU64() { return Raw<uint64_t>(); }
  double GetF64() { return Raw<double>(); }

  std::string GetString() {
    const int32_t length = GetI32();
    if (failed_ || length < 0 || static_cast<size_t>(length) > size_ - at_) {
      failed_ = true;
      return {};
    }
    std::string out(reinterpret_cast<const char*>(data_ + at_), static_cast<size_t>(length));
    at_ += static_cast<size_t>(length);
    return out;
  }

  std::vector<uint8_t> GetBytes() {
    const int32_t length = GetI32();
    if (failed_ || length < 0 || static_cast<size_t>(length) > size_ - at_) {
      failed_ = true;
      return {};
    }
    std::vector<uint8_t> out(data_ + at_, data_ + at_ + static_cast<size_t>(length));
    at_ += static_cast<size_t>(length);
    return out;
  }

  // A count is checked against the bytes remaining before anything is reserved: a hostile or
  // corrupt length prefix would otherwise turn one bad byte into a multi-gigabyte allocation.
  size_t GetCount(size_t minimumBytesPerElement) {
    const int32_t count = GetI32();
    if (failed_ || count < 0) {
      failed_ = true;
      return 0;
    }
    const size_t needed = static_cast<size_t>(count) * (minimumBytesPerElement ? minimumBytesPerElement : 1u);
    if (needed > size_ - at_) {
      failed_ = true;
      return 0;
    }
    return static_cast<size_t>(count);
  }

 private:
  uint8_t RawByte() {
    if (failed_ || at_ + 1 > size_) {
      failed_ = true;
      return 0;
    }
    return data_[at_++];
  }

  template <typename T>
  T Raw() {
    if (failed_ || at_ + sizeof(T) > size_) {
      failed_ = true;
      return T{};
    }
    T value{};
    std::memcpy(&value, data_ + at_, sizeof(T));
    at_ += sizeof(T);
    return value;
  }

  const uint8_t* data_;
  size_t size_;
  size_t at_ = 0;
  bool failed_ = false;
};

}  // namespace sphanorama::wire
