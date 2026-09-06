#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// V10 — where motion data comes from. Reporting MotionCapability::None is a normal outcome, not
// an error: iOS requires a user gesture and the user may decline.
//
// Not marked @boundary: this contract moves bytes through the shared heap rather than
// through marshalled values, so its TypeScript adapter is written against the shared-heap
// protocol rather than mirroring this signature. See ADR 0009.
class IMotionSensorAccess {
 public:
  virtual ~IMotionSensorAccess() = default;

  // What motion data *this session* can get, which is not the same as what the hardware has.
  //
  // A device with a gyroscope whose permission the user declined answers `None`, and so does one
  // with no sensors at all — because the caller's question is whether a capture can know which
  // way the camera is pointing, and both answer it the same way. `Start`'s status is where the
  // two are told apart, and the shell puts that reason on its motion row (ADR 0025).
  //
  // The distinction stopped being cosmetic with ADR 0044: `ICaptureSessionManager::Begin` refuses
  // a session on `None`, so a port answering it about hardware while the session was in fact
  // available would refuse a capture that could have run.
  virtual Result<MotionCapability> Capabilities() = 0;
  virtual Status Start(int32_t requestedHz) = 0;

  // Copies out of the shared ring buffer; returns how many samples were written.
  virtual Result<int32_t> Drain(std::span<ImuSample> out) = 0;

  virtual Status Stop() = 0;
};

}  // namespace sphanorama
