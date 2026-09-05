"""Tests for the generated wire codec.

The generator's contract is that both sides are emitted from one parse, so they cannot disagree
about field order or widths. What is worth testing is the shape of what it emits and — as with
the contract mirror — what it refuses, since a type it silently skipped is one that crosses the
boundary as garbage.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import contract_gen  # noqa: E402

PRELUDE = "namespace sphanorama {\n"
POSTLUDE = "}  // namespace sphanorama\n"


def parse(body: str):
    return contract_gen.parse(PRELUDE + body + POSTLUDE)


class WireTypeTest(unittest.TestCase):
    def test_scalars_map_onto_fixed_wire_kinds(self):
        self.assertEqual(contract_gen.wire_kind('bool', 'x'), 'bool')
        self.assertEqual(contract_gen.wire_kind('int32_t', 'x'), 'i32')
        self.assertEqual(contract_gen.wire_kind('double', 'x'), 'f64')
        self.assertEqual(contract_gen.wire_kind('std::string', 'x'), 'string')

    def test_a_64_bit_hash_gets_its_own_kind(self):
        # Widening a content hash into a double loses the low bits, which are exactly the ones
        # that decide whether a build stage is reused.
        self.assertEqual(contract_gen.wire_kind('uint64_t', 'x'), 'u64')

    def test_byte_sequences_are_a_kind_of_their_own(self):
        self.assertEqual(contract_gen.wire_kind('std::vector<uint8_t>', 'x'), 'bytes')

    def test_vectors_and_spans_agree(self):
        self.assertEqual(contract_gen.wire_kind('std::vector<Candidate>', 'x'),
                         contract_gen.wire_kind('std::span<const Candidate>', 'x'))

    def test_an_unencodable_type_is_refused(self):
        with self.assertRaises(contract_gen.ContractSyntaxError):
            contract_gen.wire_kind('std::map<int,int>', 'x')


class CodecTest(unittest.TestCase):
    def setUp(self):
        self.module = parse(
            "enum class NodeState : uint8_t { Pending, Satisfied };\n"
            "using NodeId = Id<NodeTag>;\n"
            "struct Inner { int32_t count = 0; };\n"
            "struct Outer {\n"
            "  NodeId id;\n"
            "  NodeState state = NodeState::Pending;\n"
            "  std::string label;\n"
            "  std::vector<Inner> items;\n"
            "};\n")

    def test_emits_a_cpp_encoder_and_decoder_per_struct(self):
        cpp = contract_gen.emit_cpp_codec(self.module)
        self.assertIn('void Encode(Writer& out, const Outer& value)', cpp)
        self.assertIn('bool Decode(Reader& in, Outer& value)', cpp)

    def test_emits_a_typescript_encoder_and_decoder_per_struct(self):
        ts = contract_gen.emit_ts_codec(self.module)
        self.assertIn('export function encodeOuter', ts)
        self.assertIn('export function decodeOuter', ts)

    def test_fields_are_written_in_declaration_order_on_both_sides(self):
        # Field order is the wire format. If the two emitters ever disagree the payload decodes
        # into plausible nonsense rather than failing, which is the whole reason both come from
        # one parse.
        cpp = contract_gen.emit_cpp_codec(self.module)
        ts = contract_gen.emit_ts_codec(self.module)
        for source, marker in ((cpp, 'value.'), (ts, 'value.')):
            block = source[source.index('Outer'):]
            order = [block.index(f'{marker}{name}') for name in ('id', 'state', 'label', 'items')]
            self.assertEqual(order, sorted(order))

    def test_enums_cross_as_their_index_not_their_name(self):
        cpp = contract_gen.emit_cpp_codec(self.module)
        self.assertIn('static_cast<int32_t>(value.state)', cpp)


class MinimumWireSizeTest(unittest.TestCase):
    """What bounds a decoded element count against the bytes actually present."""

    def setUp(self):
        self.module = parse(
            "using NodeId = Id<NodeTag>;\n"
            "enum class NodeState : uint8_t { Pending, Satisfied };\n"
            "struct Vec3 { double x = 0; double y = 0; double z = 0; };\n"
            "struct Sample {\n"
            "  int64_t timestampNs = 0;\n"
            "  Vec3 rate;\n"
            "  bool flagged = false;\n"
            "  NodeId node;\n"
            "  NodeState state = NodeState::Pending;\n"
            "  std::string label;\n"
            "};\n")
        self.structs, enums, self.ids = contract_gen._index(self.module)
        self.enums = set(enums)

    def size(self, kind):
        return contract_gen.min_wire_size(kind, self.structs, self.enums, self.ids)

    def test_scalars_are_their_encoded_width(self):
        self.assertEqual(self.size("bool"), 1)
        self.assertEqual(self.size("i32"), 4)
        self.assertEqual(self.size("f64"), 8)

    def test_variable_length_kinds_are_their_length_prefix(self):
        # An empty string, byte run or list is four bytes of count and nothing else.
        self.assertEqual(self.size("string"), 4)
        self.assertEqual(self.size("bytes"), 4)
        self.assertEqual(self.size("list:Vec3"), 4)

    def test_a_struct_is_the_sum_of_its_fields(self):
        self.assertEqual(self.size("named:Vec3"), 24)
        # 8 timestamp + 24 rate + 1 flag + 8 id + 4 enum + 4 empty string
        self.assertEqual(self.size("named:Sample"), 49)

    def test_a_self_referential_struct_is_refused_rather_than_recursed(self):
        module = parse("struct Loop { Loop inner; };\n")
        structs, enums, ids = contract_gen._index(module)
        with self.assertRaises(contract_gen.ContractSyntaxError):
            contract_gen.min_wire_size("named:Loop", structs, set(enums), ids)


class MethodTableTest(unittest.TestCase):
    def setUp(self):
        # Both markers, because they mean different things: @boundary mirrors the interface into
        # TypeScript, @facade also generates dispatch for it. Only managers carry both.
        self.module = parse(
            "using ProjectId = Id<ProjectTag>;\n"
            "// @boundary @facade\n"
            "class IProjectManager {\n"
            " public:\n"
            "  virtual Result<ProjectId> Create(std::string_view title) = 0;\n"
            "  virtual Status Delete(ProjectId project) = 0;\n"
            "};\n"
            "// @boundary @facade\n"
            "class ICaptureSessionManager {\n"
            " public:\n"
            "  virtual Result<CoverageState> Coverage() const = 0;\n"
            "};\n")

    def test_every_boundary_method_gets_an_id(self):
        names = [m.wire_name for m in contract_gen.method_table(self.module)]
        self.assertIn('ProjectManager.create', names)
        self.assertIn('ProjectManager.delete', names)
        self.assertIn('CaptureSessionManager.coverage', names)

    def test_ids_are_dense_and_stable_under_reparse(self):
        first = [m.wire_name for m in contract_gen.method_table(self.module)]
        second = [m.wire_name for m in contract_gen.method_table(self.module)]
        # Asserted before the comparisons, because every one of them holds for an empty table —
        # which is exactly how this suite went green against a generator that emitted nothing.
        self.assertEqual(len(first), 3)
        self.assertEqual(first, second)
        self.assertEqual([m.id for m in contract_gen.method_table(self.module)],
                         list(range(len(first))))

    def test_an_interface_marked_boundary_alone_gets_no_dispatch(self):
        # The distinction the two markers exist for. An engine crosses into TypeScript as a type
        # but is not callable from a client: dispatch for one would be a hole straight past the
        # managers, which is the call rule the whole layering rests on.
        module = parse(
            "// @boundary\n"
            "class IPoseEngine {\n"
            " public:\n"
            "  virtual Result<PoseSample> Integrate(std::span<const ImuSample> samples) = 0;\n"
            "};\n")
        self.assertEqual(contract_gen.method_table(module), [])
        self.assertNotIn('PoseEngine.integrate', contract_gen.emit_cpp_facade(module))
        self.assertNotIn('createPoseEngineProxy', contract_gen.emit_ts_facade(module))

    def test_the_method_table_is_published_for_lookup_by_name(self):
        # Same reason the probe publishes its field names: an id the client hard-codes is an id
        # that silently shifts the day a method is inserted above it.
        cpp = contract_gen.emit_cpp_facade(self.module)
        self.assertIn('sph_facade_method_name', cpp)
        self.assertIn('"ProjectManager.create"', cpp)

    def test_the_dispatcher_covers_every_method(self):
        cpp = contract_gen.emit_cpp_facade(self.module)
        for wire in ('ProjectManager.create', 'ProjectManager.delete',
                     'CaptureSessionManager.coverage'):
            self.assertIn(wire, cpp)

    def test_the_dispatcher_rejects_an_unknown_method_id(self):
        # The id space is shared with a client that may be a stale cached bundle.
        cpp = contract_gen.emit_cpp_facade(self.module)
        self.assertIn('default:', cpp)

    def test_the_typescript_proxy_exposes_each_interface(self):
        ts = contract_gen.emit_ts_facade(self.module)
        self.assertIn('createProjectManagerProxy', ts)
        self.assertIn('createCaptureSessionManagerProxy', ts)
        self.assertIn('async create(', ts)

    def test_a_proxy_checks_that_the_value_actually_decoded(self):
        # A failed Reader returns zeros rather than throwing, so a response carrying a valid Ok
        # status and nothing else decodes into a successful empty result. Every proxy that
        # returns a value has to look at the reader before handing that value over.
        ts = contract_gen.emit_ts_facade(self.module)
        create = ts[ts.index('async create('):ts.index('async delete(')]
        self.assertIn('if (!input.ok)', create)
        self.assertIn('malformedResponse', create)
        # And the check comes after the decode, not before it: checking first would pass on
        # exactly the truncated payload it is meant to catch.
        self.assertLess(create.index('const value ='), create.index('if (!input.ok)'))

    def test_a_void_proxy_needs_no_value_check(self):
        # Nothing was decoded past the status, so there is nothing to have failed.
        ts = contract_gen.emit_ts_facade(self.module)
        delete = ts[ts.index('async delete('):]
        self.assertNotIn('malformedResponse', delete.split('},')[0])

    def test_a_decode_failure_reaches_the_status_path_rather_than_leaving_the_case(self):
        # `break` inside a switch case exits the case, so a decode failure written that way skips
        # the block that encodes InvalidArgument and hands the client an empty buffer instead of
        # the status the facade ABI promises. Nothing may break out of a case before that block.
        cpp = contract_gen.emit_cpp_facade(self.module)
        for case in cpp.split("case ")[1:]:
            # Only the parameter decoding: everything before the status check, or before the
            # manager call for a method that takes no arguments. The `break` inside the status
            # check is the correct one; any break earlier than it is the bug.
            decoding = case
            for boundary in ("if (!in.ok())", "auto result =", "const Status status ="):
                decoding = decoding.split(boundary)[0]
            self.assertNotIn("break;", decoding,
                             f"a parameter decode leaves the case early:\n{decoding}")

    def test_ids_are_decoded_through_a_checked_conversion(self):
        # An id crosses as a double because JavaScript has no other number. NaN, a negative, or
        # 1e300 are all values a client can send, and casting any of them to uint64_t is
        # undefined behaviour — before any manager gets the chance to reject the id.
        cpp = contract_gen.emit_cpp_facade(self.module)
        self.assertNotIn("static_cast<uint64_t>(in.GetF64())", cpp)
        self.assertIn("GetId(", cpp)

    def test_a_parameter_is_decoded_into_the_type_the_contract_declared(self):
        # A number crosses as a double because JavaScript has no other kind, and the mirror maps
        # every width of integer onto it. What comes *back* has to be the type the C++ method
        # actually takes: decoding into a `double` and passing it to an `int32_t` parameter is a
        # narrowing the core refuses to compile under -Wconversion, so an entirely ordinary
        # contract — a pixel count, a tile index — could not cross at all. The subset is meant to
        # fail the build on what it cannot represent, and this it can.
        module = parse(
            "// @boundary @facade\n"
            "class ICaptureSessionManager {\n"
            " public:\n"
            "  virtual Result<CoverageState> Preview(int32_t maxEdge) = 0;\n"
            "};\n")
        cpp = contract_gen.emit_cpp_facade(module)
        self.assertIn("int32_t maxEdge{};", cpp)
        self.assertNotIn("double maxEdge{};", cpp)

    def test_a_double_parameter_is_still_a_double(self):
        # The other half, so the fix above cannot be "always int32_t": most numbers in these
        # contracts are angles and fractions, and rounding one at the boundary would be a
        # reticle in the wrong place rather than a compile error.
        module = parse(
            "// @boundary @facade\n"
            "class ICaptureSessionManager {\n"
            " public:\n"
            "  virtual Result<CoverageState> Aim(double azimuthDeg) = 0;\n"
            "};\n")
        self.assertIn("double azimuthDeg{};", contract_gen.emit_cpp_facade(module))

    def test_a_list_count_is_bounded_by_its_element_size_not_by_one_byte(self):
        # GetCount(1) lets a payload claim one element per byte present. Sized from that claim,
        # a small malformed message becomes a very large allocation: a hundred kilobytes of
        # nothing can ask for a hundred thousand structs. The bound has to be what an element
        # actually costs on the wire.
        module = parse(
            "struct Vec3 { double x = 0; double y = 0; double z = 0; };\n"
            "// @boundary @facade\n"
            "class ICaptureSessionManager {\n"
            " public:\n"
            "  virtual Status Feed(std::span<const Vec3> points) = 0;\n"
            "};\n")
        cpp = contract_gen.emit_cpp_facade(module)
        self.assertIn("in.GetCount(24)", cpp)
        self.assertNotIn("in.GetCount(1)", cpp)

    def test_the_proxy_calls_methods_by_name_not_by_id(self):
        ts = contract_gen.emit_ts_facade(self.module)
        self.assertIn("'ProjectManager.create'", ts)


class GeneratedFilesTest(unittest.TestCase):
    def test_the_committed_generated_files_are_up_to_date(self):
        # The drift check, as a unit test so it fails locally before CI does.
        root = Path(__file__).parent.parent
        for relative, expected in contract_gen.generate_all(root).items():
            actual = (root / relative).read_text()
            self.assertEqual(actual, expected, f"{relative} is stale — run tools/contract_gen.py")


if __name__ == "__main__":
    unittest.main()
