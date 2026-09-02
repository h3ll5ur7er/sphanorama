#pragma once

#include "engines/composition_engine/null_composition_engine.h"
#include "engines/coverage_planner_engine/rings_coverage_planner_engine.h"
#include "engines/frame_quality_engine/null_frame_quality_engine.h"
#include "engines/pose_engine/null_pose_engine.h"
#include "engines/registration_engine/null_registration_engine.h"
#include "managers/capture_session_manager/capture_session_manager.h"
#include "managers/panorama_build_manager/panorama_build_manager.h"
#include "managers/project_manager/project_manager.h"
#include "resource_access/camera_access/null_camera_access.h"
#include "resource_access/frame_store_access/null_frame_store_access.h"
#include "resource_access/motion_sensor_access/null_motion_sensor_access.h"
#if defined(__EMSCRIPTEN__)
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
  NullPoseEngine pose_;
  NullFrameQualityEngine quality_;
  // Registration and composition are constructed but not yet handed to a manager: the build
  // pipeline has nothing to run them over until Phase 2.
  NullRegistrationEngine registration_;
  NullCompositionEngine composition_;

  // Camera, motion, frame store and export live in the browser and reach the core through ports
  // that are not built yet. Null implementations refuse honestly rather than pretending, so a
  // capture session started today fails with a reason instead of producing empty frames.
  NullCameraAccess camera_;
  NullMotionSensorAccess motion_;
  NullFrameStoreAccess frames_;

  // The one contract with a real implementation on both platforms: a browser port backed by the
  // page's document host, and an in-memory store natively. Both are held to the same contract
  // suite (ADR 0010), which is what makes swapping them here safe.
#if defined(__EMSCRIPTEN__)
  BrowserProjectStoreAccess projects_;
#else
  MemoryProjectStoreAccess projects_;
#endif

  CaptureSessionManager capture_session_{planner_, pose_, quality_, camera_, motion_, frames_,
                                         projects_};
  PanoramaBuildManager panorama_build_;
  ProjectManager project_{projects_};
};

}  // namespace sphanorama::bridge
