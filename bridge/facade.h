#pragma once

#include <cstdint>

#include "export.h"

// The call surface of the core: one entry point, one result buffer.
//
// Hand-written because the ABI is the stable part; the dispatch behind it is generated
// (facade.generated.cpp), because a hand-written switch over a growing method list is a place to
// forget a case, and a forgotten case is a call that exists on one side only.
//
// Method ids are dense and published by name, so a client resolves names at startup rather than
// hard-coding ids that shift the day a method is inserted above them.
extern "C" {

int32_t sph_facade_method_count();

// Name of method `id`, or nullptr if out of range.
const char* sph_facade_method_name(int32_t id);

// Decodes `args`, dispatches, and leaves the encoded Result in the internal buffer. Returns the
// result length in bytes; the buffer is valid until the next call. Never throws (ADR 0006): a
// malformed payload comes back as an encoded failure, not a trap.
int32_t sph_facade_call(int32_t methodId, const uint8_t* args, int32_t argsLen);

const uint8_t* sph_facade_result();

}  // extern "C"
