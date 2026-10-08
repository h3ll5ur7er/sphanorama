#pragma once

#include "sphanorama/types.h"

namespace sphanorama {

// The roll a coverage planner reports in `CaptureGuidance::rollErrorDeg`, in degrees and in
// (-180, 180] (ADR 0072). Shared by both engines because it is one policy, and a second copy of it
// would be a second policy.
//
// It is `RollFromLevel(current)`, faded out within 15 degrees of straight up or down and gone
// within 5, where level has no meaning. It takes no target on purpose: a roll measured against the
// target jumps wherever the target changes, which on a narrow lens is a few degrees from every
// cell, and cells are level, so at a cell the two agree.
double GuidanceRollDeg(const Quat& current);

}  // namespace sphanorama
