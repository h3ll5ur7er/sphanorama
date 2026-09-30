#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sphanorama/types.h"

namespace sphanorama {

// What a tab leaves behind. It is deliberately a flat text document rather than the generated
// wire codec: that codec belongs to the boundary (ADR 0013) and a manager reaching into bridge/
// would be a client dependency pointing the wrong way. This is small enough to read in a
// debugger, which is worth something for the one artefact whose job is to outlive the process
// that wrote it.
//
// `CaptureSessionManager` writes it and reads it back on `Resume`; `PanoramaBuildManager` reads it
// to build from. A utility rather than either manager's private code because managers do not call
// managers, and a format read in two copies is two formats the day one of them changes (ADR 0070).
// It has one writer still.
//
// Versioned by its first line. A document from a build that wrote a different shape is refused
// rather than guessed at — half a restored session is a coverage map that lies about which cells
// hold frames.
//
// Version 2 added the tier line, and every version-1 document stopped loading with it. That is
// the right outcome rather than a cost of it: a document written before this build is precisely
// one that cannot say which capture its frames belong to, and reading it would mean assuming the
// answer this field exists to stop assuming (ADR 0035).
inline constexpr const char* kSessionDocumentKey = "session";

struct SessionDocument {
  uint64_t session = 0;
  uint64_t nextCandidate = 1;
  // Which spill tier the frames below are in, as the store reported it when this was written.
  // Frame identities restart at 1 in every process and the tier does not, so the identities alone
  // do not say whose pixels they are; this is what a resume compares before it adopts any of them
  // (ADR 0035). Zero is a real value — a host with no spill tier at all — rather than "unset".
  uint64_t generation = 0;
  CapturePlanSpec spec;
  Intrinsics lens;
  // Each cell's candidates best first, in the order the ranking left them — a build takes the first
  // whose pose was measured as the cell's frame where nobody picked (ADR 0070) — and rewritten after
  // every burst, discarding retake and offer, so a write that succeeded names no frame the capture has since let go of.
  //
  // Only the frames this session's own bursts produced, and the reason is where their bytes are.
  // Cooling spills a cell's own candidates and deliberately leaves offered ones alone (ADR 0023),
  // so an offered frame — a file import, a manual shutter — has nothing in the sink under its
  // name. Writing it down would restore a candidate whose first Pin fails, which is a cell
  // claiming evidence it cannot produce. The caller's handle went away with the tab that held it.
  std::vector<Candidate> candidates;
};

// Whether a capture may issue this identity: not zero, which is "none", and below 2^53, since an
// identity crosses to the page as a double and one past that reaches it as its neighbour's. A
// document or a pick naming any other names something no capture made, and a counter that has got
// there issues nothing more.
inline bool IssuableCandidate(CandidateId id) {
  return id.valid() && id.value < (uint64_t{1} << 53);
}

std::string EncodeSessionDocument(const SessionDocument& stored);

// All or nothing. A line this does not understand fails the whole read, because the alternative
// is a session that comes back missing the cells whose lines were malformed — and a coverage map
// that quietly lost a cell is worse than one that refuses to load, which at least says so.
bool DecodeSessionDocument(const std::string& text, SessionDocument& out);

// Where `ProjectManager` records a cell's manual pick. One document per cell, so setting a pick
// does not read and rewrite the others. The project is in the address rather than the key, which
// is what keeps two spheres that both chose something for cell 3 from sharing it.
std::string SelectionDocumentKey(NodeId node);

// The candidate a selection document names, or nothing when it does not name one.
//
// Parsed rather than trusted. `ProjectManager` wrote it, but it went through a store that outlives
// the process and can be edited by anything with the origin's storage — and `stoull` on a
// non-number throws, which is not available here. An identity no counter could issue is refused with
// the rest — zero is what `GetSelection` answers for "nobody has chosen here".
std::optional<CandidateId> ParseSelectionDocument(const std::string& text);

}  // namespace sphanorama
