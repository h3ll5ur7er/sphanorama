// ResourceAccess contracts: atomic business verbs over a resource.
//
// Declared here in C++ because they are part of the core's architecture; IMPLEMENTED in
// TypeScript for the browser (injected across the WASM boundary) and in C++ for the native
// bench and for tests. No business rules live below this line.
#pragma once

#include <functional>
#include <span>
#include "types.h"

namespace sphanorama {

// -------------------------------------------------------------------- camera
struct CameraCapabilities {
  int32_t maxWidth = 0, maxHeight = 0;
  double horizontalFovDeg = 0, verticalFovDeg = 0;   // 0 when the platform will not say
  bool supportsExposureLock = false;
  bool supportsFocusLock = false;
  bool supportsTorch = false;
  double maxBurstFps = 0;
};

struct CameraOpenSpec {
  int32_t preferredWidth = 0, preferredHeight = 0;
  bool preferRearCamera = true;
};

class ICameraAccess {
 public:
  virtual ~ICameraAccess() = default;
  virtual Result<CameraCapabilities> Open(const CameraOpenSpec&) = 0;
  virtual Status StartPreview() = 0;
  virtual Status StopPreview() = 0;
  // Latest preview frame, for pose correction and guidance. Borrowed, valid until the next call.
  virtual Result<FrameRef> PeekPreviewFrame() = 0;
  // Blocking on the core worker, asynchronous underneath. Frames are already in the frame store.
  virtual Result<std::vector<FrameRef>> CaptureBurst(const BurstSpec&) = 0;
  virtual Status SetLocks(bool exposure, bool whiteBalance, bool focus) = 0;
  virtual Status Close() = 0;
};

// ------------------------------------------------------------------- sensors
class IMotionSensorAccess {
 public:
  virtual ~IMotionSensorAccess() = default;
  virtual Result<MotionCapability> Capabilities() = 0;
  virtual Status Start(int32_t requestedHz) = 0;
  // Copies out of the shared ring buffer; returns how many samples were written.
  virtual Result<int32_t> Drain(std::span<ImuSample> out) = 0;
  virtual Status Stop() = 0;
};

// --------------------------------------------------------------- frame store
// Owns pixel residency across heap / GPU / encoded / OPFS spill. The only component that
// knows how much memory exists.
struct FrameStoreBudget {
  int64_t heapCeilingBytes = 0;    // measured at startup, not assumed
  int64_t heapUsedBytes = 0;
  int64_t spilledBytes = 0;
};

class IFrameStoreAccess {
 public:
  virtual ~IFrameStoreAccess() = default;
  virtual Result<FrameStoreBudget> Budget() = 0;
  virtual Result<FrameRef> Allocate(int32_t w, int32_t h, PixelFormat) = 0;
  // Pin promotes to HeapPinned (faulting in from spill if needed) and guarantees the mapping
  // until Release. Engines read pixels only between Pin and Release.
  virtual Result<std::span<uint8_t>> Pin(const FrameRef&) = 0;
  virtual Status Release(const FrameRef&) = 0;
  virtual Status Demote(const FrameRef&, Residency target) = 0;
  virtual Status Forget(const FrameRef&) = 0;
  virtual Result<uint64_t> ContentHash(const FrameRef&) = 0;
};

// ------------------------------------------------------------- project store
// Metadata only: documents, never pixels.
class IProjectStoreAccess {
 public:
  virtual ~IProjectStoreAccess() = default;
  virtual Result<std::vector<ProjectId>> ListProjects() = 0;
  virtual Result<std::string> ReadDocument(ProjectId, std::string_view key) = 0;
  virtual Status WriteDocument(ProjectId, std::string_view key, std::string_view value) = 0;
  virtual Status DeleteProject(ProjectId) = 0;
};

// -------------------------------------------------------------------- codecs
class IImageCodecAccess {
 public:
  virtual ~IImageCodecAccess() = default;
  virtual Result<FrameRef> Decode(std::span<const uint8_t> bytes) = 0;
  virtual Result<std::vector<uint8_t>> Encode(const FrameRef&, const EncodeSpec&) = 0;
};

// ------------------------------------------------------------ compute device
struct ComputeCapabilities {
  bool webgpu = false;
  bool simd = false;
  int32_t threads = 0;         // 0 == single-threaded; a supported mode, not a failure
  int64_t gpuMaxBufferBytes = 0;
};

enum class Kernel : uint16_t {
  WarpEquirect, GaussianPyramid, LaplacianPyramid, MultibandBlend,
  Downsample, AbsDiffMask, GainMap
};

struct KernelArgs {
  std::vector<FrameRef> inputs;
  std::vector<FrameRef> outputs;
  std::vector<double> scalars;
};

// The one place that knows whether work runs on the GPU, on threads, or serially.
// Both backends must produce numerically equivalent results; a differential test enforces it.
class IComputeDeviceAccess {
 public:
  virtual ~IComputeDeviceAccess() = default;
  virtual Result<ComputeCapabilities> Capabilities() = 0;
  virtual Status Dispatch(Kernel, const KernelArgs&) = 0;
  virtual Status Fence() = 0;
};

// -------------------------------------------------------------------- export
class IExportAccess {
 public:
  virtual ~IExportAccess() = default;
  virtual Status Save(std::string_view filename, std::string_view mimeType,
                      std::span<const uint8_t> bytes) = 0;
  virtual Result<bool> CanShare() = 0;
  virtual Status Share(std::string_view filename, std::string_view mimeType,
                       std::span<const uint8_t> bytes) = 0;
};

}  // namespace sphanorama
