#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// V10 — where motion data comes from.
//
// Reporting MotionCapability::None is a normal thing for a port to answer — iOS requires a user
// gesture and the user may decline — and since ADR 0044 it is what refuses a capture session.
// Normal to say, in other words, and not survivable to hear: see `Capabilities()` below.
//
// Deliberately carries no boundary marker: this contract moves bytes through the shared heap
// rather than through marshalled values, so its TypeScript adapter is written against the
// shared-heap protocol rather than mirroring this signature. See ADR 0009.
//
// Worded without naming the marker, which is not fussiness. `tools/contract_gen.py` looks for the
// marker as a substring of the preceding comment lines, so a sentence *about* not being marked
// was read as the mark: this interface was mirrored into `contracts/ts/contracts.d.ts` as an
// unused declaration, with the sentence that triggered it swallowed and the two around it joined
// mid-clause. The generator's detection is the real defect and is worth its own change; this
// stops the wrong file shipping in the meantime.
class IMotionSensorAccess {
 public:
  virtual ~IMotionSensorAccess() = default;

  // What motion data *this session* can get, which is not the same as what the hardware has.
  //
  // A device with a gyroscope whose permission the user declined answers `None`, and so does one
  // with no sensors at all — because the caller's question is whether a capture can know which
  // way the camera is pointing, and both answer it the same way.
  //
  // No implementation of *this* contract tells the two apart, and a caller must not expect one
  // to: `Start` reports that it could not start, not why the platform said no. The distinction
  // survives one level out, in the adapter that owns the platform call — the shell's own motion
  // port keeps the reason and puts it on the motion row (ADR 0025), which is what made an iPhone
  // reading legible. By the time it reaches here it has been collapsed on purpose.
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
