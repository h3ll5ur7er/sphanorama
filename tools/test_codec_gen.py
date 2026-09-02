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


class GeneratedFilesTest(unittest.TestCase):
    def test_the_committed_generated_files_are_up_to_date(self):
        # The drift check, as a unit test so it fails locally before CI does.
        root = Path(__file__).parent.parent
        for relative, expected in contract_gen.generate_all(root).items():
            actual = (root / relative).read_text()
            self.assertEqual(actual, expected, f"{relative} is stale — run tools/contract_gen.py")


if __name__ == "__main__":
    unittest.main()
