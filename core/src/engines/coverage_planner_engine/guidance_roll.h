#pragma once

#include "sphanorama/types.h"

namespace sphanorama {

// The roll a coverage planner reports in `CaptureGuidance::rollErrorDeg`, in degrees and in
// (-180, 180] (ADR 0072). Shared by both engines because it is one policy, and a second copy of it
// would be a second policy.
//
// Near the cell it is `RollBetween(current, target)`: the twist left after the shortest turn onto
// the cell, which is the roll the frame will have once the phone is aimed. Away from the cell that
// twist is no guide — it winds two full turns around the point behind the cell, and anywhere off the
// horizon the shortest turn tips the horizon as it goes — so there it is `RollFromLevel(current)`,
// which depends on the phone alone. Between the two it blends, because the references part fast
// enough that switching would jump the horizon by up to 30 degrees within 15 of a high cell.
double GuidanceRollDeg(const Quat& current, const Quat& target);

}  // namespace sphanorama
