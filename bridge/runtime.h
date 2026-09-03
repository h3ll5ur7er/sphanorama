#pragma once

#include "engines/composition_engine/null_composition_engine.h"
#include "engines/coverage_planner_engine/rings_coverage_planner_engine.h"
#include "engines/frame_quality_engine/null_frame_quality_engine.h"
#include "engines/pose_engine/orientation_pose_engine.h"
#include "engines/registration_engine/null_registration_engine.h"
#include "managers/capture_session_manager/capture_session_manager.h"
#include "managers/panorama_build_manager/panorama_build_manager.h"
#include "managers/project_manager/project_manager.h"
#include "resource_access/camera_access/null_camera_access.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"
#include "resource_access/frame_store_access/null_frame_store_access.h"
#include "resource_access/motion_sensor_access/null_motion_sensor_access.h"
#include "utilities/clock.h"
#if defined(__EMSCRIPTEN__)
#include "resource_access/camera_access/browser_camera_access.h"
#include "resource_access/motion_sensor_access/browser_motion_sensor_access.h"
#include "resource_access/project_store_access/browser_project_store_access.h"
#else
#include "resource_access/project_store_access/memory_project_store_access.h"
#endif

namespace sphanorama::bridge {

// Composition root: the one place that decides which implementation each contract gets.
//
// Everything above it is wired by reference and never learns what it was given, which is what
// makes a manager testable against fakes and, later, swappable to a browser-backed port without
// touching a line of business logic.
//
// A singleton because the C ABI has nowhere to keep a handle. It is created on first call and
// lives for the module's lifetime; the core runs on its own worker, so there is one of it.
class Runtime {
 public:
  static Runtime& Instance();

  ICaptureSessionManager& captureSession() { return capture_session_; }
  IPanoramaBuildManager& panoramaBuild() { return panorama_build_; }
  IProjectManager& project() { return project_; }

 private:
  Runtime() = default;

  RingsCoveragePlannerEngine planner_;
  OrientationPoseEngine pose_;
  NullFrameQualityEngine quality_;
  // Registration and composition are constructed but not yet handed to a manager: the build
  // pipeline has nothing to run them over until Phase 2.
  NullRegistrationEngine registration_;
  NullCompositionEngine composition_;

  // Camera and motion reach the core through ports over state the page established. Their
  // lifecycle and capability calls are resident and synchronous; a burst is not, and refuses
  // until that is decided with measurements (ADR 0014).
#if defined(__EMSCRIPTEN__)
  BrowserCameraAccess camera_;
  BrowserMotionSensorAccess motion_;
#else
  NullCameraAccess camera_;
  NullMotionSensorAccess motion_;
#endif

  // Frames live in memory natively, where the bench runs and where a spill tier that is still
  // RAM is an honest description of the host. The browser keeps the null store: a phone is the
  // one place the difference matters, and telling it that spilling freed memory when it did not
  // is worse than telling it there is nowhere to put a frame. Its own store, over OPFS, is what
  // that branch is waiting for.
  //
  // The ceiling is stated rather than measured. `FrameStoreBudget::heapCeilingBytes` is
  // documented as "measured at startup, not assumed", and this is the assumption that stands in
  // until the probe exists — generous enough that the bench does not spill on a desktop, and far
  // enough below a real sphere of bursts that the spill path is still reached.
#if defined(__EMSCRIPTEN__)
  NullFrameStoreAccess frames_;
#else
  static constexpr int64_t kNativeHeapCeilingBytes = 512ll << 20;
  MemoryFrameStoreAccess frames_{kNativeHeapCeilingBytes};
#endif

  // The one contract with a real implementation on both platforms: a browser port backed by the
  // page's document host, and an in-memory store natively. Both are held to the same contract
  // suite (ADR 0010), which is what makes swapping them here safe.
#if defined(__EMSCRIPTEN__)
  BrowserProjectStoreAccess projects_;
#else
  MemoryProjectStoreAccess projects_;
#endif

  // The wall clock the burst interval is paced against (ADR 0018). Injected rather than called
  // directly so a replayed dataset can drive the same manager from recorded timestamps.
  SystemClock clock_;

  CaptureSessionManager capture_session_{planner_, pose_, quality_, camera_, motion_, frames_,
                                         projects_, clock_};
  PanoramaBuildManager panorama_build_;
  ProjectManager project_{projects_};
};

}  // namespace sphanorama::bridge
