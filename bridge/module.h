#pragma once

#include <cstdint>

// The C ABI the JavaScript side calls. Declared in a header so the native test suite can exercise
// it directly: the boundary is where a mistake is most expensive to find, and finding it in a
// browser is the slowest way to find anything.
//
// Not Embind — see ADR 0012. Structured results are written into caller-allocated buffers that
// JavaScript reads through HEAP32.
extern "C" {

// Number of int32 slots sph_probe_runtime writes.
int32_t sph_probe_field_count();

// Name of slot `index`, or nullptr if out of range.
//
// The boundary describes its own layout so that the JavaScript side never re-declares field
// order. A reordering on one side only would otherwise read plausible nonsense rather than fail.
const char* sph_probe_field_name(int32_t index);

// Writes sph_probe_field_count() int32 values into `out`. Returns 0 on success, non-zero if the
// caller passed a null buffer — no exceptions cross this boundary (ADR 0006).
int32_t sph_probe_runtime(int32_t concurrency, int32_t crossOriginIsolated, int32_t* out);

}  // extern "C"
