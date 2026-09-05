#pragma once

#include "engines/composition_engine/null_composition_engine.h"
#include "engines/coverage_planner_engine/rings_coverage_planner_engine.h"
#include "engines/frame_preview_engine/box_frame_preview_engine.h"
#include "engines/frame_quality_engine/sharpness_frame_quality_engine.h"
#include "engines/pose_engine/orientation_pose_engine.h"
#include "engines/registration_engine/null_registration_engine.h"
#include "managers/capture_session_manager/capture_session_manager.h"
#include "managers/panorama_build_manager/panorama_build_manager.h"
#include "managers/project_manager/project_manager.h"
#include "resource_access/camera_access/null_camera_access.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"
#include "resource_access/motion_sensor_access/null_motion_sensor_access.h"
#include "utilities/clock.h"
#if defined(__EMSCRIPTEN__)
#include "resource_access/camera_access/browser_camera_access.h"
#include "resource_access/frame_store_access/opfs_spill_sink.h"
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
  // Registration and composition are constructed but not yet handed to a manager: the build
  // pipeline has nothing to run them over until Phase 2.
  NullRegistrationEngine registration_;
  NullCompositionEngine composition_;

  // Declared before the camera, which holds a reference to it: members are constructed in
  // declaration order, and a camera handed a store that had not been built yet would be reading
  // a ceiling of whatever was on the stack.
  //
  // One frame store on both platforms, differing by where a spilled frame's bytes go (ADR 0020).
  // Natively there is no sink, so there is no spill tier and the store refuses to demote into
  // one: with 512 MB to spend and a desktop underneath, a ceiling refusal is the honest answer.
  // In the browser the sink is an OPFS sync access handle the worker opened at startup, so a
  // spill actually frees memory — which is the whole reason the core is in a worker at all
  // (ADR 0019).
  //
  // The sink is handed over only if the worker installed a host. A browser without one — no
  // origin private file system, or a handle that would not open — gets a store with nowhere to
  // spill rather than a store that lies about having spilled, and a sphere capped at what fits in
  // RAM is degraded rather than broken.
  //
  // **The browser's ceiling is read from the device** — `navigator.deviceMemory` for a share of
  // the machine's RAM, clamped by what the module was linked to allow (`heap_budget.h` has the
  // arithmetic and the argument for why there is no allocation probe). A WASM heap that grows
  // past what a phone will give a tab is not an allocation failure the store can report, it is
  // the operating system killing the tab, so the ceiling has to sit below that and above what a
  // burst needs.
  //
  // **Natively it is still stated**, and that is not an oversight. The bench runs on a desktop
  // where the failure mode does not exist, and 512 MB is chosen to be generous enough not to
  // spill during a normal run and small enough that the spill path is still reached by a test.
#if defined(__EMSCRIPTEN__)
  static int64_t BrowserHeapCeilingBytes();
  OpfsSpillSink spill_;
  MemoryFrameStoreAccess frames_{BrowserHeapCeilingBytes(),
                                 OpfsSpillSink::Available() ? &spill_ : nullptr};
#else
  static constexpr int64_t kNativeHeapCeilingBytes = 512ll << 20;
  MemoryFrameStoreAccess frames_{kNativeHeapCeilingBytes};
#endif

  // Camera and motion reach the core through ports over state the page established (ADR 0014).
  // Every call on them is resident and synchronous, the burst included: it is paced by the
  // manager over the preview frame the page keeps, so nothing here has to wait (ADR 0018).
  //
  // The camera takes the frame store because a peeked frame *is* an allocation in it — that is
  // what a FrameRef means, for every implementation of the contract (ADR 0021).
#if defined(__EMSCRIPTEN__)
  BrowserCameraAccess camera_{frames_};
  BrowserMotionSensorAccess motion_;
#else
  NullCameraAccess camera_;
  NullMotionSensorAccess motion_;
#endif

  // Declared after the store because it reads pixels through it — one of the two resource
  // accesses an engine may touch (docs/03 §3.3 rule 5). Until now this was the null engine,
  // which gave every frame a default score and ranked by insertion order: a burst was five
  // frames and a coin flip.
  SharpnessFrameQualityEngine quality_{frames_};

  // Declared beside it and for the same reason: reading a stored frame back out is pixel work,
  // and pixel work reaches the store. This is what makes a review strip show the frames rather
  // than what the core knows about them (ADR 0038).
  BoxFramePreviewEngine preview_{frames_};

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

  CaptureSessionManager capture_session_{planner_, pose_, quality_, preview_, camera_,
                                         motion_, frames_, projects_, clock_};
  PanoramaBuildManager panorama_build_;
  ProjectManager project_{projects_};
};

}  // namespace sphanorama::bridge
