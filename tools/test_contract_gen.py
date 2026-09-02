"""Tests for the C++ -> TypeScript contract generator.

The generator's job is to make drift impossible, so its most important property is not what it
emits but what it *refuses*: anything outside the declared subset must be a hard error, or the
header quietly grows constructs the mirror silently drops.
"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import contract_gen  # noqa: E402

PRELUDE = "namespace sphanorama {\n"
POSTLUDE = "}  // namespace sphanorama\n"


def parse(body: str):
    return contract_gen.parse(PRELUDE + body + POSTLUDE)


def emit(body: str) -> str:
    return contract_gen.emit_typescript(parse(body))


class IdTest(unittest.TestCase):
    def test_id_aliases_become_branded_numbers(self):
        ts = emit("using NodeId = Id<NodeTag>;\n")
        self.assertIn("export type NodeId = number & { readonly __brand: 'NodeId' };", ts)


class EnumTest(unittest.TestCase):
    def test_enum_becomes_a_string_literal_union(self):
        ts = emit("enum class Residency : uint8_t { HeapPinned, Spilled };\n")
        self.assertIn("export type Residency = 'HeapPinned' | 'Spilled';", ts)

    def test_enum_with_explicit_values_keeps_its_member_names(self):
        ts = emit("enum class StatusCode : uint16_t { Ok = 0, NotFound, Internal };\n")
        self.assertIn("'Ok' | 'NotFound' | 'Internal'", ts)

    def test_multiline_enum_bodies_parse(self):
        ts = emit("enum class BuildStage : uint8_t {\n  Queued, Features,\n  Complete\n};\n")
        self.assertIn("'Queued' | 'Features' | 'Complete'", ts)


class StructTest(unittest.TestCase):
    def test_struct_becomes_an_interface_with_mapped_field_types(self):
        ts = emit("struct Vec3 { double x = 0; double y = 0; };\n")
        self.assertIn("export interface Vec3 {", ts)
        self.assertIn("x: number;", ts)

    def test_comma_separated_fields_expand(self):
        ts = emit("struct Vec3 { double x = 0, y = 0, z = 0; };\n")
        for axis in "xyz":
            self.assertIn(f"{axis}: number;", ts)

    def test_scalar_kinds_map_to_typescript(self):
        ts = emit("struct S {\n"
                  "  int32_t count = 0;\n"
                  "  bool enabled = false;\n"
                  "  double ratio = 0;\n"
                  "  std::string title;\n"
                  "  uint64_t contentHash = 0;\n"
                  "};\n")
        self.assertIn("count: number;", ts)
        self.assertIn("enabled: boolean;", ts)
        self.assertIn("ratio: number;", ts)
        self.assertIn("title: string;", ts)
        # A 64-bit hash does not survive a JS number; emitting one would corrupt build
        # fingerprints in a way nothing would notice until a rebuild silently reused a stale tile.
        self.assertIn("contentHash: bigint;", ts)

    def test_vectors_become_arrays(self):
        ts = emit("struct GhostReport { std::vector<GhostRegion> regions; };\n")
        self.assertIn("regions: GhostRegion[];", ts)

    def test_byte_spans_become_uint8array(self):
        # Encoded image bytes as number[] would marshal one JS number per byte — megabytes of
        # boxed values per export, for a payload that is already a byte buffer on both sides.
        ts = emit("struct S { std::vector<uint8_t> bytes; };\n")
        self.assertIn("bytes: Uint8Array;", ts)

    def test_named_types_pass_through(self):
        ts = emit("struct Candidate { NodeId node; QualityScore quality; };\n")
        self.assertIn("node: NodeId;", ts)
        self.assertIn("quality: QualityScore;", ts)

    def test_doc_comments_are_carried_across(self):
        ts = emit("// A handle, not a buffer.\nstruct FrameRef { FrameId id; };\n")
        self.assertIn("A handle, not a buffer.", ts)


class InterfaceTest(unittest.TestCase):
    """Only interfaces marked @boundary are mirrored: engines never cross into JavaScript."""

    def test_unmarked_interface_is_not_emitted(self):
        ts = emit("class IPoseEngine {\n public:\n"
                  "  virtual Status Reset(PoseMode mode) = 0;\n};\n")
        self.assertNotIn("PoseEngine", ts)

    def test_marked_interface_is_emitted_without_the_leading_i(self):
        ts = emit("// @boundary\nclass IProjectManager {\n public:\n"
                  "  virtual Status Delete(ProjectId project) = 0;\n};\n")
        self.assertIn("export interface ProjectManager {", ts)

    def test_methods_become_lower_camel_case_and_async(self):
        ts = emit("// @boundary\nclass IProjectManager {\n public:\n"
                  "  virtual Result<ProjectId> Create(std::string_view title) = 0;\n};\n")
        self.assertIn("create(title: string): Promise<Result<ProjectId>>;", ts)

    def test_status_returning_methods_become_result_void(self):
        # Status and Result<T> are the same thing on the wire; collapsing Status to a bare void
        # would lose the failure channel entirely.
        ts = emit("// @boundary\nclass IProjectManager {\n public:\n"
                  "  virtual Status Delete(ProjectId project) = 0;\n};\n")
        self.assertIn("delete(project: ProjectId): Promise<Result<void>>;", ts)

    def test_span_and_vector_parameters_become_arrays(self):
        ts = emit("// @boundary\nclass ICaptureSessionManager {\n public:\n"
                  "  virtual Status OnMotion(std::span<const ImuSample> samples) = 0;\n};\n")
        self.assertIn("onMotion(samples: ImuSample[]): Promise<Result<void>>;", ts)

    def test_const_reference_parameters_lose_their_decoration(self):
        ts = emit("// @boundary\nclass ICaptureSessionManager {\n public:\n"
                  "  virtual Status Begin(const CapturePlanSpec& spec) = 0;\n};\n")
        self.assertIn("begin(spec: CapturePlanSpec): Promise<Result<void>>;", ts)

    def test_trailing_const_on_a_method_is_accepted(self):
        ts = emit("// @boundary\nclass ICaptureSessionManager {\n public:\n"
                  "  virtual Result<CapturePlan> GetPlan() const = 0;\n};\n")
        self.assertIn("getPlan(): Promise<Result<CapturePlan>>;", ts)


class StrictnessTest(unittest.TestCase):
    """What the generator refuses is the whole point: silence here means silent drift."""

    def test_unknown_scalar_type_is_rejected(self):
        with self.assertRaises(contract_gen.ContractSyntaxError):
            parse("struct S { long double weird = 0; };\n")

    def test_unnamed_parameter_is_rejected(self):
        # An unnamed parameter cannot be mirrored, and guessing a name would produce a TS
        # signature that compiles but means nothing to a caller.
        with self.assertRaises(contract_gen.ContractSyntaxError):
            parse("// @boundary\nclass IX {\n public:\n"
                  "  virtual Status Go(const BurstSpec&) = 0;\n};\n")

    def test_pointer_field_is_rejected(self):
        with self.assertRaises(contract_gen.ContractSyntaxError):
            parse("struct S { int32_t* ptr = nullptr; };\n")

    def test_error_names_the_construct_it_could_not_parse(self):
        with self.assertRaises(contract_gen.ContractSyntaxError) as ctx:
            parse("struct S { long double weird = 0; };\n")
        self.assertIn("weird", str(ctx.exception))


class GeneratedFileTest(unittest.TestCase):
    def test_output_is_marked_as_generated(self):
        ts = emit("using NodeId = Id<NodeTag>;\n")
        self.assertIn("DO NOT EDIT", ts)

    def test_generation_is_deterministic(self):
        # CI diffs the regenerated file against the committed one, so any run-to-run instability
        # would show up as a permanently red build.
        body = "enum class Residency : uint8_t { HeapPinned, Spilled };\nstruct S { bool a = false; };\n"
        self.assertEqual(emit(body), emit(body))


class RealHeaderTest(unittest.TestCase):
    """The generator has to cope with the actual contract header, not just fixtures."""

    def setUp(self):
        self.types = Path(__file__).parent.parent / "contracts/cpp/sphanorama/types.h"

    def test_the_real_types_header_parses(self):
        module = contract_gen.parse(self.types.read_text())
        names = {d.name for d in module.declarations}
        self.assertIn("FrameRef", names)
        self.assertIn("StatusCode", names)
        self.assertIn("NodeId", names)

    def test_the_committed_mirror_is_up_to_date(self):
        # This is the drift check itself, run as a unit test so it fails locally before CI.
        root = Path(__file__).parent.parent
        expected = contract_gen.generate(root)
        actual = (root / "contracts/ts/contracts.d.ts").read_text()
        self.assertEqual(actual, expected,
                         "contracts/ts/contracts.d.ts is stale — run tools/contract_gen.py")


class CommandLineTest(unittest.TestCase):
    def script(self):
        return Path(__file__).parent / "contract_gen.py"

    def test_check_mode_exits_zero_when_the_mirror_matches(self):
        r = subprocess.run([sys.executable, str(self.script()), "--check"],
                           capture_output=True, text=True,
                           cwd=str(Path(__file__).parent.parent))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_check_mode_exits_nonzero_on_a_stale_mirror(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "contracts/cpp/sphanorama").mkdir(parents=True)
            (root / "contracts/ts").mkdir(parents=True)
            (root / "contracts/cpp/sphanorama/types.h").write_text(
                PRELUDE + "using NodeId = Id<NodeTag>;\n" + POSTLUDE)
            (root / "contracts/ts/contracts.d.ts").write_text("stale\n")
            r = subprocess.run([sys.executable, str(self.script()), "--check", "--root", str(root)],
                               capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("stale", (r.stdout + r.stderr).lower())


if __name__ == "__main__":
    unittest.main()
