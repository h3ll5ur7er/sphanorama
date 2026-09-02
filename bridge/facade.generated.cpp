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
    "CaptureSessionManager.getPlan",
    "CaptureSessionManager.onMotion",
    "CaptureSessionManager.captureCell",
    "CaptureSessionManager.offerFrame",
    "CaptureSessionManager.coverage",
    "CaptureSessionManager.candidates",
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
    "ProjectManager.resume",
    "ProjectManager.delete",
    "ProjectManager.setSelection",
    "ProjectManager.export",
};

constexpr int32_t kMethodCount = 21;

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
      project.value = static_cast<uint64_t>(in.GetF64());
      CapturePlanSpec spec{};
      if (!codec::Decode(in, spec)) break;
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
    case 1: {  // CaptureSessionManager.getPlan
      auto result = runtime.captureSession().GetPlan();
      PutStatus(out, result.status);
      if (result.ok()) {
        codec::Encode(out, result.value);
      }
      break;
    }
    case 2: {  // CaptureSessionManager.onMotion
      std::vector<ImuSample> samples;
      { const size_t count = in.GetCount(1);
    if (!in.ok()) break;
    samples.clear();
    samples.resize(count);
    for (auto& item : samples) { if (!codec::Decode(in, item)) break; } }
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
    case 3: {  // CaptureSessionManager.captureCell
      NodeId node{};
      node.value = static_cast<uint64_t>(in.GetF64());
      BurstSpec burst{};
      if (!codec::Decode(in, burst)) break;
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      auto result = runtime.captureSession().CaptureCell(node, burst);
      PutStatus(out, result.status);
      if (result.ok()) {
        out.PutCount(result.value.size());
  for (const auto& item : result.value) { codec::Encode(out, item); }
      }
      break;
    }
    case 4: {  // CaptureSessionManager.offerFrame
      NodeId node{};
      node.value = static_cast<uint64_t>(in.GetF64());
      FrameRef frame{};
      if (!codec::Decode(in, frame)) break;
      PoseSample pose{};
      if (!codec::Decode(in, pose)) break;
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
    case 5: {  // CaptureSessionManager.coverage
      auto result = runtime.captureSession().Coverage();
      PutStatus(out, result.status);
      if (result.ok()) {
        codec::Encode(out, result.value);
      }
      break;
    }
    case 6: {  // CaptureSessionManager.candidates
      NodeId node{};
      node.value = static_cast<uint64_t>(in.GetF64());
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
    case 7: {  // CaptureSessionManager.requestRetake
      NodeId node{};
      node.value = static_cast<uint64_t>(in.GetF64());
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
    case 8: {  // CaptureSessionManager.end
      const Status status = runtime.captureSession().End();
      PutStatus(out, status);
      break;
    }
    case 9: {  // PanoramaBuildManager.start
      SessionId session{};
      session.value = static_cast<uint64_t>(in.GetF64());
      BuildSpec spec{};
      if (!codec::Decode(in, spec)) break;
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
    case 10: {  // PanoramaBuildManager.poll
      BuildId build{};
      build.value = static_cast<uint64_t>(in.GetF64());
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
    case 11: {  // PanoramaBuildManager.panorama
      BuildId build{};
      build.value = static_cast<uint64_t>(in.GetF64());
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
    case 12: {  // PanoramaBuildManager.ghosts
      BuildId build{};
      build.value = static_cast<uint64_t>(in.GetF64());
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
    case 13: {  // PanoramaBuildManager.invalidate
      BuildId build{};
      build.value = static_cast<uint64_t>(in.GetF64());
      std::vector<NodeId> dirty;
      { const size_t count = in.GetCount(1);
    if (!in.ok()) break;
    dirty.clear();
    dirty.resize(count);
    for (auto& item : dirty) { item.value = static_cast<uint64_t>(in.GetF64()); } }
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      const Status status = runtime.panoramaBuild().Invalidate(build, std::span<const NodeId>(dirty));
      PutStatus(out, status);
      break;
    }
    case 14: {  // PanoramaBuildManager.cancel
      BuildId build{};
      build.value = static_cast<uint64_t>(in.GetF64());
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      const Status status = runtime.panoramaBuild().Cancel(build);
      PutStatus(out, status);
      break;
    }
    case 15: {  // ProjectManager.list
      auto result = runtime.project().List();
      PutStatus(out, result.status);
      if (result.ok()) {
        out.PutCount(result.value.size());
  for (const auto& item : result.value) { codec::Encode(out, item); }
      }
      break;
    }
    case 16: {  // ProjectManager.create
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
    case 17: {  // ProjectManager.resume
      ProjectId project{};
      project.value = static_cast<uint64_t>(in.GetF64());
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      auto result = runtime.project().Resume(project);
      PutStatus(out, result.status);
      if (result.ok()) {
        out.PutF64(static_cast<double>(result.value.value));
      }
      break;
    }
    case 18: {  // ProjectManager.delete
      ProjectId project{};
      project.value = static_cast<uint64_t>(in.GetF64());
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      const Status status = runtime.project().Delete(project);
      PutStatus(out, status);
      break;
    }
    case 19: {  // ProjectManager.setSelection
      ProjectId project{};
      project.value = static_cast<uint64_t>(in.GetF64());
      NodeId node{};
      node.value = static_cast<uint64_t>(in.GetF64());
      CandidateId candidate{};
      candidate.value = static_cast<uint64_t>(in.GetF64());
      if (!in.ok()) {
        PutStatus(out, Fail(StatusCode::InvalidArgument, "facade",
                            "malformed arguments"));
        break;
      }
      const Status status = runtime.project().SetSelection(project, node, candidate);
      PutStatus(out, status);
      break;
    }
    case 20: {  // ProjectManager.export
      ProjectId project{};
      project.value = static_cast<uint64_t>(in.GetF64());
      BuildId build{};
      build.value = static_cast<uint64_t>(in.GetF64());
      ExportSpec spec{};
      if (!codec::Decode(in, spec)) break;
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
