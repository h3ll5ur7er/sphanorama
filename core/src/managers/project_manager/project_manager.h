#pragma once

#include <map>
#include <string>

#include "sphanorama/managers/project_manager.h"
#include "sphanorama/resource_access/export_access.h"
#include "sphanorama/resource_access/frame_store_access.h"
#include "sphanorama/resource_access/image_codec_access.h"
#include "sphanorama/resource_access/project_store_access.h"

namespace sphanorama {

// Project lifecycle and export.
//
// Export is the one place bytes leave the device, so this is the only manager holding
// IExportAccess, and the codec beside it that writes what leaves (ADR 0087).
//
// It holds the frame store for one question, which tier it holds, so a listing can say which
// sessions that tier can give back (ADR 0083).
class ProjectManager final : public IProjectManager {
 public:
  ProjectManager(IProjectStoreAccess& store, IFrameStoreAccess& frames, IImageCodecAccess& codec,
                 IExportAccess& exporter);

  Result<std::vector<ProjectSummary>> List() override;
  Result<ProjectId> Create(std::string_view title) override;
  Status Delete(ProjectId project) override;
  Status SetSelection(ProjectId project, NodeId node, CandidateId candidate) override;
  Result<CandidateId> GetSelection(ProjectId project, NodeId node) override;
  Status Export(ProjectId project, const PanoramaRef& panorama, const ExportSpec& spec) override;

 private:
  bool Exists(ProjectId project);

  IProjectStoreAccess& store_;
  IFrameStoreAccess& frames_;
  IImageCodecAccess& codec_;
  IExportAccess& exporter_;
};

}  // namespace sphanorama
