#pragma once
#include "sphanorama/types.h"

namespace sphanorama {

// V14 — where heavy math executes. The one place that knows whether work runs on the GPU, on
// threads, or serially.
//
// Both backends must produce numerically equivalent results; a differential test enforces it,
// because the CPU path is the correctness reference for the GPU one.
//
// Not marked @boundary: this contract moves bytes through the shared heap rather than
// through marshalled values, so its TypeScript adapter is written against the shared-heap
// protocol rather than mirroring this signature. See ADR 0009.
class IComputeDeviceAccess {
 public:
  virtual ~IComputeDeviceAccess() = default;

  virtual Result<ComputeCapabilities> Capabilities() = 0;
  virtual Status Dispatch(Kernel, const KernelArgs&) = 0;
  virtual Status Fence() = 0;
};

}  // namespace sphanorama
