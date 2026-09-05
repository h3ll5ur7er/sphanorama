// The generated dispatch, exercised natively.
//
// The boundary is where a mistake is most expensive to find, and finding it in a browser is the
// slowest way to find anything. Everything here runs against the same generated switch the WASM
// build compiles.
#include <gtest/gtest.h>

#include <limits>
#include <set>
#include <string>
#include <vector>

#include "facade.h"
#include "sphanorama/codec.h"

namespace sphanorama {
namespace {

int32_t MethodId(const std::string& wireName) {
  for (int32_t id = 0; id < sph_facade_method_count(); ++id) {
    const char* name = sph_facade_method_name(id);
    if (name != nullptr && wireName == name) return id;
  }
  return -1;
}

struct Response {
  Status status;
  std::vector<uint8_t> bytes;
  wire::Reader reader() const { return wire::Reader(bytes.data(), bytes.size()); }
};

Response Call(const std::string& wireName, const std::vector<uint8_t>& args = {}) {
  const int32_t id = MethodId(wireName);
  EXPECT_GE(id, 0) << "no such method: " << wireName;
  const int32_t length = sph_facade_call(id, args.data(), static_cast<int32_t>(args.size()));
  Response response;
  response.bytes.assign(sph_facade_result(), sph_facade_result() + length);
  return response;
}

Status ReadStatus(wire::Reader& in) {
  Status status;
  status.code = static_cast<StatusCode>(in.GetI32());
  in.GetString();                     // component, not asserted on here
  status.detail = in.GetString();
  return status;
}

TEST(Facade, PublishesEveryMethodByName) {
  // A client resolves names at startup rather than hard-coding ids, which shift the day a method
  // is inserted above them.
  ASSERT_GT(sph_facade_method_count(), 0);
  std::set<std::string> names;
  for (int32_t id = 0; id < sph_facade_method_count(); ++id) {
    const char* name = sph_facade_method_name(id);
    ASSERT_NE(name, nullptr) << "method " << id;
    names.insert(name);
  }
  EXPECT_EQ(static_cast<int32_t>(names.size()), sph_facade_method_count());
  EXPECT_TRUE(names.count("ProjectManager.create"));
  EXPECT_TRUE(names.count("CaptureSessionManager.onMotion"));
}

TEST(Facade, OnlyManagersAreDispatched) {
  // Resource accesses are mirrored into TypeScript because the browser implements them; the call
  // goes the other way, so dispatching them here would mean calling a runtime that has no such
  // thing.
  for (int32_t id = 0; id < sph_facade_method_count(); ++id) {
    const std::string name = sph_facade_method_name(id);
    EXPECT_NE(name.find("Manager."), std::string::npos) << name;
  }
}

TEST(Facade, AnOutOfRangeNameIsNull) {
  EXPECT_EQ(sph_facade_method_name(-1), nullptr);
  EXPECT_EQ(sph_facade_method_name(sph_facade_method_count()), nullptr);
}

TEST(Facade, AnUnknownMethodIdIsReportedRatherThanCrashing) {
  // A cached client bundle can be older than the core it loaded: a version mismatch is something
  // to report, not a trap.
  const int32_t length = sph_facade_call(9999, nullptr, 0);
  ASSERT_GT(length, 0);
  std::vector<uint8_t> bytes(sph_facade_result(), sph_facade_result() + length);
  wire::Reader in(bytes.data(), bytes.size());
  EXPECT_EQ(ReadStatus(in).code, StatusCode::NotFound);
}

TEST(Facade, CreatesAProjectAndReadsItBack) {
  wire::Writer args;
  args.PutString("kitchen");
  Response created = Call("ProjectManager.create", args.bytes());

  wire::Reader in = created.reader();
  ASSERT_EQ(ReadStatus(in).code, StatusCode::Ok);
  const auto id = static_cast<uint64_t>(in.GetF64());
  EXPECT_GT(id, 0u);

  Response listed = Call("ProjectManager.list");
  wire::Reader listing = listed.reader();
  ASSERT_EQ(ReadStatus(listing).code, StatusCode::Ok);
  const size_t count = listing.GetCount(1);
  ASSERT_GE(count, 1u);

  bool found = false;
  for (size_t i = 0; i < count; ++i) {
    ProjectSummary summary;
    ASSERT_TRUE(codec::Decode(listing, summary));
    if (summary.id.value == id) {
      EXPECT_EQ(summary.title, "kitchen");
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST(Facade, ASelectionCrossesTheBoundaryAndComesBackTheSameNumber) {
  // The new call in ADR 0040, over the generated dispatch rather than over a manager held
  // directly. Until now the only thing exercising it was one end-to-end test in a browser, which
  // is the slowest place to find a boundary mistake and the one that says least about which end
  // made it.
  //
  // Each `Response` is held in a named local because `reader()` points into its buffer, which a
  // temporary would free at the end of the statement.
  wire::Writer named;
  named.PutString("kitchen");
  Response opened = Call("ProjectManager.create", named.bytes());
  wire::Reader created = opened.reader();
  ASSERT_EQ(ReadStatus(created).code, StatusCode::Ok);
  const double project = created.GetF64();

  // A cell nobody has chosen for is a *success* answering zero, not a failure — the sentinel this
  // whole decision turns on, and this is the only test that watches it survive the wire.
  wire::Writer cell;
  cell.PutF64(project);
  cell.PutF64(3.0);
  Response never = Call("ProjectManager.getSelection", cell.bytes());
  wire::Reader absent = never.reader();
  ASSERT_EQ(ReadStatus(absent).code, StatusCode::Ok);
  EXPECT_EQ(absent.GetF64(), 0.0);
  // The reader is asked whether it read anything, which on this leg is the whole assertion.
  // `StatusCode::Ok` is zero and a failed `wire::Reader` answers zero to everything, so both lines
  // above are also satisfied by a response of no bytes at all — on the one leg where zero is the
  // value under test rather than an incidental one. `wire.h` states this protocol: check `ok()`
  // once at the end rather than after every read.
  EXPECT_TRUE(absent.ok()) << "a zero read out of a failed reader is not an answer of zero";

  wire::Writer pick;
  pick.PutF64(project);
  pick.PutF64(3.0);
  pick.PutF64(7.0);
  Response wrote = Call("ProjectManager.setSelection", pick.bytes());
  wire::Reader recorded = wrote.reader();
  ASSERT_EQ(ReadStatus(recorded).code, StatusCode::Ok);

  Response asked = Call("ProjectManager.getSelection", cell.bytes());
  wire::Reader read = asked.reader();
  ASSERT_EQ(ReadStatus(read).code, StatusCode::Ok);
  EXPECT_EQ(read.GetF64(), 7.0);
  EXPECT_TRUE(read.ok());

  // And a cell the writer refuses is refused rather than answered, so the two ends cannot be made
  // to agree on a value neither of them considers an identity.
  wire::Writer unset;
  unset.PutF64(project);
  unset.PutF64(0.0);
  Response denied = Call("ProjectManager.getSelection", unset.bytes());
  wire::Reader refused = denied.reader();
  EXPECT_EQ(ReadStatus(refused).code, StatusCode::InvalidArgument);
}

TEST(Facade, ADomainFailureCrossesAsAStatusNotAsATrap) {
  wire::Writer args;
  args.PutString("");                 // an untitled project is refused by the manager
  Response response = Call("ProjectManager.create", args.bytes());
  wire::Reader in = response.reader();
  EXPECT_EQ(ReadStatus(in).code, StatusCode::InvalidArgument);
}

TEST(Facade, MalformedArgumentsAreRejectedRatherThanDecodedAsGarbage) {
  // What a truncated postMessage looks like from the receiving side.
  const std::vector<uint8_t> truncated{0x40, 0x00};
  Response response = Call("ProjectManager.create", truncated);
  wire::Reader in = response.reader();
  EXPECT_EQ(ReadStatus(in).code, StatusCode::InvalidArgument);
}

TEST(Facade, TheDetailExplainsWhyRatherThanRepeatingTheCode) {
  wire::Writer args;
  args.PutString("");
  Response response = Call("ProjectManager.create", args.bytes());
  wire::Reader in = response.reader();
  EXPECT_FALSE(ReadStatus(in).detail.empty());
}

TEST(Facade, StartingACaptureSessionFailsHonestlyWithoutACamera) {
  // The camera lives in JavaScript and nothing on this side opened one. The session refuses with
  // a reason rather than producing empty frames, which is the whole argument for null over stub.
  wire::Writer titled;
  titled.PutString("a project to capture into");
  Response created = Call("ProjectManager.create", titled.bytes());
  wire::Reader out = created.reader();
  ASSERT_EQ(ReadStatus(out).code, StatusCode::Ok);
  const double project = out.GetF64();

  wire::Writer args;
  args.PutF64(project);
  CapturePlanSpec spec;
  spec.acceptanceConeDeg = 4.0;
  codec::Encode(args, spec);

  Response response = Call("CaptureSessionManager.begin", args.bytes());
  wire::Reader in = response.reader();
  const Status status = ReadStatus(in);
  EXPECT_EQ(status.code, StatusCode::CameraUnavailable);
}

TEST(Facade, ATruncatedStructArgumentIsReportedAsAStatusNotAnEmptyBuffer) {
  // The client may be a stale bundle, a corrupted transfer, or something hostile. Whatever the
  // cause, the ABI promises a status — and a decode that left the switch case early handed back
  // an empty buffer instead, which the client can only report as "malformed response".
  wire::Writer args;
  args.PutF64(1.0);          // node id
  args.PutF64(5.0);          // frameCount, then the BurstSpec simply stops
  Response response = Call("CaptureSessionManager.armBurst", args.bytes());
  ASSERT_FALSE(response.bytes.empty()) << "the facade returned nothing at all";
  wire::Reader in = response.reader();
  EXPECT_EQ(ReadStatus(in).code, StatusCode::InvalidArgument);
}

TEST(Facade, APreviewRequestReachesTheManagerRatherThanStoppingAtTheArguments) {
  // The one call that answers with pixels (ADR 0038), and the first with an `int32_t` parameter:
  // the generator used to decode every number into a `double`, which the core will not narrow.
  // Three arguments in the order the contract declares them, and a status back from the manager
  // — `FailedPrecondition`, because nothing here has begun a session — rather than the facade's
  // own `InvalidArgument`, which is what a parameter list read in the wrong order produces.
  wire::Writer args;
  args.PutF64(1.0);          // node
  args.PutF64(1.0);          // candidate
  args.PutF64(128.0);        // maxEdge
  Response response = Call("CaptureSessionManager.candidatePreview", args.bytes());
  ASSERT_FALSE(response.bytes.empty()) << "the facade returned nothing at all";
  wire::Reader in = response.reader();
  EXPECT_EQ(ReadStatus(in).code, StatusCode::FailedPrecondition);
}

TEST(Facade, ATruncatedPreviewRequestIsReportedAsAStatus) {
  wire::Writer args;
  args.PutF64(1.0);          // a node, and then the request simply stops
  Response response = Call("CaptureSessionManager.candidatePreview", args.bytes());
  ASSERT_FALSE(response.bytes.empty());
  wire::Reader in = response.reader();
  EXPECT_EQ(ReadStatus(in).code, StatusCode::InvalidArgument);
}

TEST(Facade, AnIntegerArgumentThatIsNotOneIsRefusedRatherThanCast) {
  // Every number crosses as a double, because JavaScript has no other kind, and an `int32_t`
  // parameter is therefore a double that has to become an integer. `static_cast` of a NaN or of
  // anything outside the target's range is undefined — the same hazard `GetId` exists for, one
  // door along. So the reader refuses the message rather than converting it, and the caller gets
  // the facade's own `InvalidArgument` instead of whatever the cast happened to produce.
  for (const double raw : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(),
                           1e300, -1e300, 2147483648.0, -2147483649.0, 12.5}) {
    wire::Writer args;
    args.PutF64(1.0);          // node
    args.PutF64(1.0);          // candidate
    args.PutF64(raw);          // maxEdge, which is an int32_t on the other side
    Response response = Call("CaptureSessionManager.candidatePreview", args.bytes());
    ASSERT_FALSE(response.bytes.empty()) << "the facade returned nothing at all for " << raw;
    wire::Reader in = response.reader();
    EXPECT_EQ(ReadStatus(in).code, StatusCode::InvalidArgument) << "for " << raw;
  }
}

TEST(Facade, AnIntegerArgumentAtTheEdgeOfItsRangeStillCrosses) {
  // The bounds are inclusive, and asserting that matters: a check written with the wrong
  // comparison would reject the largest valid value and nothing above would notice.
  for (const double raw : {2147483647.0, -2147483648.0}) {
    wire::Writer args;
    args.PutF64(1.0);
    args.PutF64(1.0);
    args.PutF64(raw);
    Response response = Call("CaptureSessionManager.candidatePreview", args.bytes());
    ASSERT_FALSE(response.bytes.empty());
    wire::Reader in = response.reader();
    // Refused by the manager for having no session, which is one layer past the reader — the
    // point is that it got there rather than being rejected as malformed.
    EXPECT_EQ(ReadStatus(in).code, StatusCode::FailedPrecondition) << "for " << raw;
  }
}

TEST(Facade, AListCountIsRefusedWhenThePayloadCouldNotPossiblyHoldThatMany) {
  // The count is what the vector is sized from, so bounding it at one byte per element let a
  // small message ask for a very large allocation: 100 kB of filler claiming 100000 ImuSamples
  // is over 11 MB of vector on a phone, from a payload that cannot contain a single valid one.
  constexpr int32_t kClaimed = 100000;
  wire::Writer args;
  args.PutCount(kClaimed);
  std::vector<uint8_t> payload = args.bytes();
  payload.resize(payload.size() + kClaimed, 0);   // one byte per element promised

  Response response = Call("CaptureSessionManager.onMotion", payload);
  ASSERT_FALSE(response.bytes.empty());
  wire::Reader in = response.reader();
  EXPECT_EQ(ReadStatus(in).code, StatusCode::InvalidArgument);
}

TEST(Facade, ATruncatedListArgumentIsReportedAsAStatus) {
  wire::Writer args;
  args.PutCount(4);          // four samples promised, none supplied
  Response response = Call("CaptureSessionManager.onMotion", args.bytes());
  ASSERT_FALSE(response.bytes.empty());
  wire::Reader in = response.reader();
  EXPECT_EQ(ReadStatus(in).code, StatusCode::InvalidArgument);
}

TEST(Facade, AnIdThatIsNotAWholeNumberIsRefused) {
  // Ids cross as doubles because JavaScript has no other number type, so NaN, a negative and
  // 1e300 are all things a client can send. Converting any of them to uint64_t is undefined
  // behaviour, and it happens before a manager ever sees the id.
  for (const double bad : {std::numeric_limits<double>::quiet_NaN(),
                           -1.0, 0.5, 1e300}) {
    wire::Writer args;
    args.PutF64(bad);
    Response response = Call("ProjectManager.delete", args.bytes());
    ASSERT_FALSE(response.bytes.empty());
    wire::Reader in = response.reader();
    EXPECT_EQ(ReadStatus(in).code, StatusCode::InvalidArgument) << "for " << bad;
  }
}

TEST(Facade, ACaptureSessionForAProjectThatDoesNotExistIsRefused) {
  // Checked before the camera, so this is the answer even though no camera is open either.
  wire::Writer args;
  args.PutF64(4040.0);
  CapturePlanSpec spec;
  spec.acceptanceConeDeg = 4.0;
  codec::Encode(args, spec);

  Response response = Call("CaptureSessionManager.begin", args.bytes());
  wire::Reader in = response.reader();
  EXPECT_EQ(ReadStatus(in).code, StatusCode::NotFound);
}

TEST(Facade, ABuildCannotBeStartedYetAndSaysSo) {
  wire::Writer args;
  args.PutF64(1.0);
  BuildSpec spec;
  codec::Encode(args, spec);
  Response response = Call("PanoramaBuildManager.start", args.bytes());
  wire::Reader in = response.reader();
  EXPECT_EQ(ReadStatus(in).code, StatusCode::Unsupported);
}

TEST(Facade, TheResultBufferSurvivesUntilTheNextCall) {
  wire::Writer args;
  args.PutString("first");
  Response first = Call("ProjectManager.create", args.bytes());
  ASSERT_FALSE(first.bytes.empty());

  Call("ProjectManager.list");
  // `first` copied its bytes, so it is still readable — which is the contract the TypeScript side
  // relies on when it copies out of HEAPU8 before making another call.
  wire::Reader in = first.reader();
  EXPECT_EQ(ReadStatus(in).code, StatusCode::Ok);
}

}  // namespace
}  // namespace sphanorama
