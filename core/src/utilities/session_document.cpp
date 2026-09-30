#include "utilities/session_document.h"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <system_error>

#include "utilities/quaternion.h"

namespace sphanorama {
namespace {
constexpr int kSessionVersion = 2;

// Every double round-trips: 17 significant digits is what IEEE-754 needs to come back bit for
// bit, and a pose that drifts in the last place on every reload would be a slow corruption of the
// only thing anchoring a cell to a direction.
std::string Digits(double value) {
  std::ostringstream out;
  out << std::setprecision(17) << value;
  return out.str();
}

template <typename Enum>
bool ReadEnum(std::istringstream& in, int limit, Enum& out) {
  int raw = -1;
  if (!(in >> raw) || raw < 0 || raw >= limit) return false;
  out = static_cast<Enum>(raw);
  return true;
}

}  // namespace

std::string EncodeSessionDocument(const SessionDocument& stored) {
  std::ostringstream out;
  out << "sphanorama-session " << kSessionVersion << '\n';
  out << "session " << stored.session << ' ' << stored.nextCandidate << '\n';
  // A line of its own rather than a field on the session line: it is a statement about the tier
  // the frames are in, not about the session's counters, and the two are written from different
  // places.
  out << "tier " << stored.generation << '\n';
  out << "lens " << stored.lens.width << ' ' << stored.lens.height << '\n';
  out << "spec " << static_cast<int>(stored.spec.strategy)
      << ' ' << Digits(stored.spec.horizontalFovDeg)
      << ' ' << Digits(stored.spec.verticalFovDeg)
      << ' ' << Digits(stored.spec.overlapTarget)
      << ' ' << Digits(stored.spec.acceptanceConeDeg)
      << ' ' << (stored.spec.coverPoles ? 1 : 0)
      << ' ' << static_cast<int>(stored.spec.motion) << '\n';

  for (const Candidate& candidate : stored.candidates) {
    const FrameRef& frame = candidate.frame;
    const PoseSample& pose = candidate.pose;
    const QualityScore& quality = candidate.quality;
    out << "candidate " << candidate.id.value << ' ' << candidate.node.value
        << ' ' << frame.id.value << ' ' << frame.buffer.value
        << ' ' << static_cast<int>(frame.format)
        << ' ' << frame.width << ' ' << frame.height << ' ' << frame.stride
        << ' ' << frame.timestampNs << ' ' << frame.contentHash
        << ' ' << pose.timestampNs
        << ' ' << Digits(pose.orientation.w) << ' ' << Digits(pose.orientation.x)
        << ' ' << Digits(pose.orientation.y) << ' ' << Digits(pose.orientation.z)
        << ' ' << Digits(pose.angularVelocity.x) << ' ' << Digits(pose.angularVelocity.y)
        << ' ' << Digits(pose.angularVelocity.z)
        << ' ' << Digits(pose.confidence) << ' ' << (pose.visuallyCorrected ? 1 : 0)
        << ' ' << Digits(quality.sharpness) << ' ' << Digits(quality.motionBlur)
        << ' ' << Digits(quality.exposureAgreement) << ' ' << Digits(quality.alignmentResidual)
        << ' ' << Digits(quality.moverPenalty) << ' ' << Digits(quality.aggregate) << '\n';
  }
  return out.str();
}

bool DecodeSessionDocument(const std::string& text, SessionDocument& out) {
  std::istringstream lines(text);
  std::string line;

  if (!std::getline(lines, line)) return false;
  {
    std::istringstream header(line);
    std::string tag;
    int version = 0;
    if (!(header >> tag >> version) || tag != "sphanorama-session" || version != kSessionVersion) {
      return false;
    }
  }

  // Nothing may be left over on a line once this build has read what it knows about. A trailing
  // field is a document from a shape this one does not have, and the version gate is the only
  // sanctioned way to read one of those — taking the prefix that fits is how a session comes back
  // missing whatever the extra field was there to say.
  const auto exhausted = [](std::istringstream& in) {
    std::string extra;
    return !(in >> extra);
  };

  bool sawSession = false, sawLens = false, sawSpec = false, sawTier = false;
  while (std::getline(lines, line)) {
    if (line.empty()) continue;
    std::istringstream in(line);
    std::string tag;
    in >> tag;
    if (tag == "session") {
      if (!(in >> out.session >> out.nextCandidate)) return false;
      // Zero is not an identity: `Id::valid()` is `value != 0` and every counter in this codebase
      // starts at 1. A document carrying one is one this build cannot honour, and restoring it
      // would seat the session under a name nothing can legitimately hold.
      // The session's own identity is bounded like a candidate's: `Resume` steps the manager's
      // counter past it, and one at the top would wrap it through zero.
      if (!IssuableIdentity(out.session) || out.nextCandidate == 0) return false;
      if (!exhausted(in)) return false;
      sawSession = true;
    } else if (tag == "tier") {
      // Zero is legal here, unlike the identities above: it is what a host with no spill tier
      // reports, and a document written against one has to be able to say so.
      if (!(in >> out.generation)) return false;
      if (!exhausted(in)) return false;
      sawTier = true;
    } else if (tag == "lens") {
      if (!(in >> out.lens.width >> out.lens.height)) return false;
      if (!exhausted(in)) return false;
      sawLens = true;
    } else if (tag == "spec") {
      int coverPoles = 0;
      if (!ReadEnum(in, 3, out.spec.strategy)) return false;
      if (!(in >> out.spec.horizontalFovDeg >> out.spec.verticalFovDeg >> out.spec.overlapTarget
               >> out.spec.acceptanceConeDeg >> coverPoles)) {
        return false;
      }
      if (!ReadEnum(in, 4, out.spec.motion)) return false;
      out.spec.coverPoles = coverPoles != 0;
      if (!exhausted(in)) return false;
      sawSpec = true;
    } else if (tag == "candidate") {
      Candidate candidate;
      int corrected = 0;
      if (!(in >> candidate.id.value >> candidate.node.value
               >> candidate.frame.id.value >> candidate.frame.buffer.value)) {
        return false;
      }
      if (!ReadEnum(in, 7, candidate.frame.format)) return false;
      if (!(in >> candidate.frame.width >> candidate.frame.height >> candidate.frame.stride
               >> candidate.frame.timestampNs >> candidate.frame.contentHash
               >> candidate.pose.timestampNs
               >> candidate.pose.orientation.w >> candidate.pose.orientation.x
               >> candidate.pose.orientation.y >> candidate.pose.orientation.z
               >> candidate.pose.angularVelocity.x >> candidate.pose.angularVelocity.y
               >> candidate.pose.angularVelocity.z
               >> candidate.pose.confidence >> corrected
               >> candidate.quality.sharpness >> candidate.quality.motionBlur
               >> candidate.quality.exposureAgreement >> candidate.quality.alignmentResidual
               >> candidate.quality.moverPenalty >> candidate.quality.aggregate)) {
        return false;
      }
      // Same rule as the session line above, and it matters more here: a candidate or frame under
      // an invalid identity is one the store would be asked to adopt, and the first thing to go
      // wrong with it would go wrong a long way from this document.
      // A frame's too: `Adopt` steps the store's counter past it.
      if (!IssuableCandidate(candidate.id) || !candidate.node.valid()
          || !IssuableIdentity(candidate.frame.id.value)
          || !candidate.frame.buffer.valid()) {
        return false;
      }
      // A pose `OfferFrame` would refuse at the door keeps its frame and loses its claim: it comes
      // back unanchored, which is what a pose nobody measured already is. Restored as it stands, it
      // would be written back by every checkpoint and refuse every `Refine` built from the capture;
      // refusing the document instead cost the whole sphere, since the page reads a refusal here
      // as one a later build can open and the one button it leaves clears the tier (ADR 0065).
      if (PoseSampleDefect(candidate.pose)) candidate.pose.confidence = 0.0;
      if (!exhausted(in)) return false;
      candidate.pose.visuallyCorrected = corrected != 0;
      out.candidates.push_back(candidate);
    } else {
      // An unknown tag is a document from a shape this build does not have, which the version
      // line should already have caught. Refusing rather than skipping keeps that the only way a
      // newer document can be read, instead of half-read.
      return false;
    }
  }
  // The tier line is required, not defaulted. A document that does not say which capture it
  // belongs to is exactly the document this build must not act on, and a missing line reading as
  // zero would make it look like one written on a host with no tier at all.
  if (!(sawSession && sawLens && sawSpec && sawTier)) return false;

  // The candidates win where the two disagree, for the same reason the spill index's slots beat
  // its high-water mark (ADR 0030): only the candidates are acted on. A counter that has fallen
  // behind them — a write torn between the candidate lines and the session line — would have the
  // next burst issue ids naming frames the cell is already holding, and a cell with two
  // candidates under one id has two frames as far as everything downstream can tell. Raised
  // rather than refused, because nothing is lost by raising it and a capture is lost by refusing.
  for (const Candidate& candidate : out.candidates) {
    out.nextCandidate = std::max(out.nextCandidate, candidate.id.value + 1);
  }
  return true;
}

std::string SelectionDocumentKey(NodeId node) {
  return "selection/" + std::to_string(node.value);
}

std::optional<CandidateId> ParseSelectionDocument(const std::string& text) {
  uint64_t chosen = 0;
  const auto* const end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, chosen);

  // Three clauses answering three questions — did it parse, was all of it a number, is the number
  // a legal identity — and no input makes the first of them the deciding one: `from_chars` leaves
  // `chosen` untouched when it fails, and `chosen` starts at zero, so the third catches whatever
  // the first would have. That overlap is deliberate rather than dead. The third is a rule about
  // *content* and would still be needed if the parse could not fail; the first is what makes the
  // zero initialiser not load-bearing, and dropping it would leave the refusal of an overflowing
  // document resting on a standard guarantee about a variable nobody assigned.
  if (parsed.ec != std::errc{} || parsed.ptr != end || !IssuableCandidate(CandidateId{chosen})) {
    return std::nullopt;
  }
  return CandidateId{chosen};
}

}  // namespace sphanorama
