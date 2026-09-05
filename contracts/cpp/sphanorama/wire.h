#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
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

// Whether a double can become a uint64_t identifier without undefined behaviour.
//
// Every id that reaches C++ from JavaScript is a double, because JavaScript has no other number
// type — and it arrives through more than one door: the facade decodes ids off the wire, and the
// project-store port reads them from whatever keys IndexedDB happens to hold, which may predate
// this build. NaN, a negative, a fraction and 1e300 are all values either door can present, and
// converting any of them is undefined before a manager ever sees the id.
//
// The upper bound is the largest double below 2^64, which is the last value the conversion is
// defined for.
inline bool IsRepresentableId(double raw) {
  return raw >= 0.0 && raw <= 18446744073709549568.0 && raw == std::trunc(raw);
}

// Whether a double can become an integer of type T without undefined behaviour.
//
// The same hazard as IsRepresentableId, one door along. Every number crosses as a double because
// JavaScript has no other kind, so an `int32_t` parameter is a double that has to become one —
// and NaN, an infinity, a fraction and anything outside T's range are all undefined to cast.
//
// Both bounds are exact here, which is why this can compare directly where an identifier cannot:
// for an integer of four bytes or fewer every representable value is a double, so `lowest()` and
// `max()` convert without rounding. Wider types are refused at compile time rather than checked
// approximately — 2^63 is not a double, and a bound that rounds the wrong way is a check that
// passes the one value it exists to stop. `GetId` is the shape a 64-bit reader wants.
template <typename T>
inline bool IsRepresentableInteger(double raw) {
  static_assert(std::is_integral_v<T>, "this is for integers");
  static_assert(sizeof(T) <= 4, "a wider integer needs GetId's treatment: its bounds are not "
                                "exactly representable as doubles");
  return raw == std::trunc(raw) &&
         raw >= static_cast<double>(std::numeric_limits<T>::lowest()) &&
         raw <= static_cast<double>(std::numeric_limits<T>::max());
}

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
  // An identifier, checked. See IsRepresentableId: a value that is not a whole non-negative
  // number in range fails the reader rather than being cast.
  // A number that has to become an integer, checked. See IsRepresentableInteger: a value the
  // conversion is not defined for fails the reader rather than being cast, which turns undefined
  // behaviour into the facade's own "malformed arguments".
  template <typename T>
  T GetInteger() {
    const double raw = GetF64();
    if (failed_) return 0;
    if (!IsRepresentableInteger<T>(raw)) {
      failed_ = true;
      return 0;
    }
    return static_cast<T>(raw);
  }

  uint64_t GetId() {
    const double raw = GetF64();
    if (failed_) return 0;
    if (!IsRepresentableId(raw)) {
      failed_ = true;
      return 0;
    }
    return static_cast<uint64_t>(raw);
  }

  size_t GetCount(size_t minimumBytesPerElement) {
    const int32_t count = GetI32();
    if (failed_ || count < 0) {
      failed_ = true;
      return 0;
    }
    // Divided, never multiplied. `count * minimum` overflows — and does it most easily where it
    // is hardest to notice: size_t is 32 bits on wasm32, so a 114-byte element and a count of
    // 37675152 multiply to 2^32 + 32, wrap to 32, and let a 40-byte payload ask for a vector of
    // 37 million. Dividing the space that exists cannot overflow at any width.
    const size_t minimum = minimumBytesPerElement ? minimumBytesPerElement : 1u;
    if (static_cast<size_t>(count) > (size_ - at_) / minimum) {
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
