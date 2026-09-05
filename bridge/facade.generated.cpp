// GENERATED FILE — DO NOT EDIT. Produced by tools/contract_gen.py.
//
// Dispatch for every @boundary method: decode the arguments, call the manager the runtime holds,
// encode the Result. Generated because a hand-written switch over a growing method list is a
// place to forget a case, and a forgotten case is a call that exists on one side only.
//
// Method ids are dense and published by name (sph_facade_method_name), so the client resolves
// names rather than hard-coding ids that shift the day a method is inserted above them.
#include "facade.h"

#include <span>
#include <string>
#include <vector>

#include "sphanorama/codec.h"
#include "runtime.h"

namespace {

using sphanorama::wire::Reader;
using sphanorama::wire::Writer;

// The single result buffer the C ABI hands back. One call is in flight at a time by
// construction: the core runs on its own worker and the facade is synchronous.
std::vector<uint8_t> g_result;

void PutStatus(Writer& out, const sphanorama::Status& status) {
  out.PutI32(static_cast<int32_t>(status.code));
  out.PutString(status.component);
  out.PutString(status.detail);
}

const char* const kMethodNames[] = {
    "CaptureSessionManager.begin",
    "CaptureSessionManager.resume",
    "CaptureSessionManager.getPlan",
    "CaptureSessionManager.onMotion",
    "CaptureSessionManager.armBurst",
    "CaptureSessionManager.offerFrame",
    "CaptureSessionManager.coverage",
    "CaptureSessionManager.candidates",
    "CaptureSessionManager.candidatePreview",
    "CaptureSessionManager.requestRetake",
    "CaptureSessionManager.end",
    "PanoramaBuildManager.start",
    "PanoramaBuildManager.poll",
    "PanoramaBuildManager.panorama",
    "PanoramaBuildManager.ghosts",
    "PanoramaBuildManager.invalidate",
    "PanoramaBuildManager.cancel",
    "ProjectManager.list",
    "ProjectManager.create",
    "ProjectManager.delete",
    "ProjectManager.setSelection",
    "ProjectManager.export",
};

constexpr int32_t kMethodCount = 22;

}  // namespace

extern "C" {

SPH_EXPORT int32_t sph_facade_method_count() { return kMethodCount; }

SPH_EXPORT const char* sph_facade_method_name(int32_t id) {
  if (id < 0 || id >= kMethodCount) return nullptr;
  return kMethodNames[id];
}

SPH_EXPORT const uint8_t* sph_facade_result() { return g_result.data(); }

SPH_EXPORT int32_t sph_facade_call(int32_t methodId, const uint8_t* args,
                                   int32_t argsLen) {
  using namespace sphanorama;
  Reader in(args, argsLen < 0 ? 0u : static_cast<size_t>(argsLen));
  Writer out;
  auto& runtime = bridge::Runtime::Instance();
  (void)runtime;

  switch (methodId) {
    case 0: {  // CaptureSessionManager.begin
      ProjectId project{};
      project.value = in.GetId();
      CapturePlanSpec spec{};
      (void)codec::Decode(in, spec);
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      auto result = runtime.captureSession().Begin(project, spec);
      PutStatus(out, result.status);
      if (result.ok()) {
        out.PutF64(static_cast<double>(result.value.value));
      }
      break;
    }
    case 1: {  // CaptureSessionManager.resume
      ProjectId project{};
      project.value = in.GetId();
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      auto result = runtime.captureSession().Resume(project);
      PutStatus(out, result.status);
      if (result.ok()) {
        out.PutF64(static_cast<double>(result.value.value));
      }
      break;
    }
    case 2: {  // CaptureSessionManager.getPlan
      auto result = runtime.captureSession().GetPlan();
      PutStatus(out, result.status);
      if (result.ok()) {
        codec::Encode(out, result.value);
      }
      break;
    }
    case 3: {  // CaptureSessionManager.onMotion
      std::vector<ImuSample> samples;
      { const size_t count = in.GetCount(115);
    samples.clear();
    samples.resize(count);
    for (auto& item : samples) { (void)codec::Decode(in, item); } }
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      auto result = runtime.captureSession().OnMotion(std::span<const ImuSample>(samples));
      PutStatus(out, result.status);
      if (result.ok()) {
        codec::Encode(out, result.value);
      }
      break;
    }
    case 4: {  // CaptureSessionManager.armBurst
      NodeId node{};
      node.value = in.GetId();
      BurstSpec burst{};
      (void)codec::Decode(in, burst);
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      const Status status = runtime.captureSession().ArmBurst(node, burst);
      PutStatus(out, status);
      break;
    }
    case 5: {  // CaptureSessionManager.offerFrame
      NodeId node{};
      node.value = in.GetId();
      FrameRef frame{};
      (void)codec::Decode(in, frame);
      PoseSample pose{};
      (void)codec::Decode(in, pose);
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      auto result = runtime.captureSession().OfferFrame(node, frame, pose);
      PutStatus(out, result.status);
      if (result.ok()) {
        out.PutI32(static_cast<int32_t>(result.value));
      }
      break;
    }
    case 6: {  // CaptureSessionManager.coverage
      auto result = runtime.captureSession().Coverage();
      PutStatus(out, result.status);
      if (result.ok()) {
        codec::Encode(out, result.value);
      }
      break;
    }
    case 7: {  // CaptureSessionManager.candidates
      NodeId node{};
      node.value = in.GetId();
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      auto result = runtime.captureSession().Candidates(node);
      PutStatus(out, result.status);
      if (result.ok()) {
        out.PutCount(result.value.size());
  for (const auto& item : result.value) { codec::Encode(out, item); }
      }
      break;
    }
    case 8: {  // CaptureSessionManager.candidatePreview
      NodeId node{};
      node.value = in.GetId();
      CandidateId candidate{};
      candidate.value = in.GetId();
      int32_t maxEdge{};
      maxEdge = static_cast<decltype(maxEdge)>(in.GetF64());
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      auto result = runtime.captureSession().CandidatePreview(node, candidate, maxEdge);
      PutStatus(out, result.status);
      if (result.ok()) {
        codec::Encode(out, result.value);
      }
      break;
    }
    case 9: {  // CaptureSessionManager.requestRetake
      NodeId node{};
      node.value = in.GetId();
      bool replace{};
      replace = in.GetBool();
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      const Status status = runtime.captureSession().RequestRetake(node, replace);
      PutStatus(out, status);
      break;
    }
    case 10: {  // CaptureSessionManager.end
      const Status status = runtime.captureSession().End();
      PutStatus(out, status);
      break;
    }
    case 11: {  // PanoramaBuildManager.start
      SessionId session{};
      session.value = in.GetId();
      BuildSpec spec{};
      (void)codec::Decode(in, spec);
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      auto result = runtime.panoramaBuild().Start(session, spec);
      PutStatus(out, result.status);
      if (result.ok()) {
        out.PutF64(static_cast<double>(result.value.value));
      }
      break;
    }
    case 12: {  // PanoramaBuildManager.poll
      BuildId build{};
      build.value = in.GetId();
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      auto result = runtime.panoramaBuild().Poll(build);
      PutStatus(out, result.status);
      if (result.ok()) {
        codec::Encode(out, result.value);
      }
      break;
    }
    case 13: {  // PanoramaBuildManager.panorama
      BuildId build{};
      build.value = in.GetId();
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      auto result = runtime.panoramaBuild().Panorama(build);
      PutStatus(out, result.status);
      if (result.ok()) {
        codec::Encode(out, result.value);
      }
      break;
    }
    case 14: {  // PanoramaBuildManager.ghosts
      BuildId build{};
      build.value = in.GetId();
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      auto result = runtime.panoramaBuild().Ghosts(build);
      PutStatus(out, result.status);
      if (result.ok()) {
        codec::Encode(out, result.value);
      }
      break;
    }
    case 15: {  // PanoramaBuildManager.invalidate
      BuildId build{};
      build.value = in.GetId();
      std::vector<NodeId> dirty;
      { const size_t count = in.GetCount(8);
    dirty.clear();
    dirty.resize(count);
    for (auto& item : dirty) { item.value = in.GetId(); } }
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      const Status status = runtime.panoramaBuild().Invalidate(build, std::span<const NodeId>(dirty));
      PutStatus(out, status);
      break;
    }
    case 16: {  // PanoramaBuildManager.cancel
      BuildId build{};
      build.value = in.GetId();
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      const Status status = runtime.panoramaBuild().Cancel(build);
      PutStatus(out, status);
      break;
    }
    case 17: {  // ProjectManager.list
      auto result = runtime.project().List();
      PutStatus(out, result.status);
      if (result.ok()) {
        out.PutCount(result.value.size());
  for (const auto& item : result.value) { codec::Encode(out, item); }
      }
      break;
    }
    case 18: {  // ProjectManager.create
      std::string title{};
      title = in.GetString();
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      auto result = runtime.project().Create(title);
      PutStatus(out, result.status);
      if (result.ok()) {
        out.PutF64(static_cast<double>(result.value.value));
      }
      break;
    }
    case 19: {  // ProjectManager.delete
      ProjectId project{};
      project.value = in.GetId();
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      const Status status = runtime.project().Delete(project);
      PutStatus(out, status);
      break;
    }
    case 20: {  // ProjectManager.setSelection
      ProjectId project{};
      project.value = in.GetId();
      NodeId node{};
      node.value = in.GetId();
      CandidateId candidate{};
      candidate.value = in.GetId();
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      const Status status = runtime.project().SetSelection(project, node, candidate);
      PutStatus(out, status);
      break;
    }
    case 21: {  // ProjectManager.export
      ProjectId project{};
      project.value = in.GetId();
      BuildId build{};
      build.value = in.GetId();
      ExportSpec spec{};
      (void)codec::Decode(in, spec);
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      const Status status = runtime.project().Export(project, build, spec);
      PutStatus(out, status);
      break;
    }
    default:
      // A client bundle can be older than the core it loaded; an unknown id is a
      // version mismatch to report, not a crash.
      PutStatus(out, Fail(StatusCode::NotFound, "facade", "unknown method id"));
      break;
  }

  g_result = out.bytes();
  return static_cast<int32_t>(g_result.size());
}

}  // extern "C"
