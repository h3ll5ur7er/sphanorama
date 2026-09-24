"""Tests for the fused-build check.

What it guards is quiet: a build that stops contracting still compiles, still passes every test, and
reads as the fused job having looked. So each way the flag can go missing gets a case, including the
one a round-12 reviewer found by sabotage — the flag present on the command line and overridden by
a later one.
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import fused_build_check  # noqa: E402

QUATERNION = "/repo/core/src/utilities/quaternion.cpp"
AVERAGE = "/repo/core/src/utilities/quaternion_average.cpp"
AVERAGE_OBJECT_SOURCE = AVERAGE


def entry(path: str, flags: str, form: str = "command") -> dict:
    output = "core/" + Path(path).name + ".o"
    line = f"/usr/bin/clang++ -O3 {flags} -o {output} -c {path}"
    if form == "arguments":
        return {"directory": "/build", "file": path, "output": output, "arguments": line.split()}
    return {"directory": "/build", "file": path, "output": output, "command": line}


FUSED = "  401000:\tc4 e2 f1 a9 c2\tvfmadd213sd %xmm2,%xmm1,%xmm0\n"
UNFUSED = "  401000:\tc5 fb 59 c1\tvmulsd %xmm1,%xmm0,%xmm0\n  401004:\tc5 fb 58 c2\tvaddsd %xmm2,%xmm0,%xmm0\n"


def fused(_object: Path) -> str:
    return FUSED


class FusedBuildCheck(unittest.TestCase):
    def check(self, entries, disassemble=fused):
        entries = entries + [entry(AVERAGE_OBJECT_SOURCE, "-ffp-contract=fast")] if not any(
            e["file"] == AVERAGE_OBJECT_SOURCE for e in entries) else entries
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "compile_commands.json").write_text(json.dumps(entries))
            return fused_build_check.problems(Path(tmp), disassemble)

    def test_a_build_that_contracts_everywhere_passes(self):
        self.assertEqual(self.check([entry(QUATERNION, "-ffp-contract=fast"),
                                     entry(AVERAGE, "-ffp-contract=fast")]), [])

    def test_a_compile_without_the_flag_is_named(self):
        found = self.check([entry(QUATERNION, "-ffp-contract=fast"), entry(AVERAGE, "")])
        self.assertEqual(len(found), 1)
        self.assertIn("quaternion_average.cpp", found[0])

    def test_a_later_flag_overrides_an_earlier_one(self):
        # The compiler obeys the last `-ffp-contract=`, so a check that only asks whether `fast`
        # appears passes a build that contracts nothing. Found by sabotage in round 12.
        found = self.check([entry(QUATERNION, "-ffp-contract=fast -ffp-contract=off")])
        self.assertEqual(len(found), 1)
        self.assertIn("off", found[0])

    def test_an_earlier_off_is_overridden_by_a_later_fast(self):
        self.assertEqual(self.check([entry(QUATERNION, "-ffp-contract=off -ffp-contract=fast")]),
                         [])

    def test_the_arguments_form_is_read_too(self):
        # CMake's Ninja generator writes `command`; other generators and tools write `arguments`.
        self.assertEqual(self.check([entry(QUATERNION, "-ffp-contract=fast", "arguments")]), [])
        self.assertEqual(len(self.check([entry(QUATERNION, "", "arguments")])), 1)

    def test_a_database_without_the_norm_is_refused(self):
        # An empty or unrelated database has nothing wrong in it, and must not read as a pass.
        found = self.check([entry(AVERAGE, "-ffp-contract=fast")])
        self.assertEqual(len(found), 1)
        self.assertIn("quaternion.cpp", found[0])

    def test_a_missing_database_says_so(self):
        with tempfile.TemporaryDirectory() as tmp:
            found = fused_build_check.problems(Path(tmp), fused)
        self.assertEqual(len(found), 1)
        self.assertIn("configure", found[0])

    def test_an_unreadable_database_says_so(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "compile_commands.json").write_text("{not json")
            found = fused_build_check.problems(Path(tmp), fused)
        self.assertEqual(len(found), 1)
        self.assertIn("could not be read", found[0])

    def test_a_later_floating_point_model_decides_the_mode(self):
        # `-ffp-model=` sets contraction too, and clang obeys whichever of the two comes last:
        # `precise` means contraction within a statement only, which is not the build that showed
        # the defect, and `strict` means none. Found in round 13.
        self.assertEqual(len(self.check([entry(QUATERNION, "-ffp-contract=fast -ffp-model=precise")])), 1)
        self.assertEqual(len(self.check([entry(QUATERNION, "-ffp-contract=fast -ffp-model=strict")])), 1)
        self.assertEqual(self.check([entry(QUATERNION, "-ffp-model=strict -ffp-contract=fast")]), [])
        self.assertEqual(self.check([entry(QUATERNION, "-ffp-model=fast")]), [])

    def test_a_build_whose_averager_fused_nothing_is_refused(self):
        # What the flags cannot show: a build without the FMA instruction set, or with
        # `-frounding-math`, keeps `-ffp-contract=fast` on every line and fuses nothing. The
        # averager is measured rather than `quaternion.cpp`, because it has no `std::fma` of its
        # own, so any fused instruction in it is the compiler contracting. Found in round 13.
        found = self.check([entry(QUATERNION, "-ffp-contract=fast")], lambda _o: UNFUSED)
        self.assertEqual(len(found), 1)
        self.assertIn("no fused", found[0])

    def test_a_database_without_the_averager_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "compile_commands.json").write_text(
                json.dumps([entry(QUATERNION, "-ffp-contract=fast")]))
            found = fused_build_check.problems(Path(tmp), fused)
        self.assertEqual(len(found), 1)
        self.assertIn("quaternion_average.cpp", found[0])

    def test_the_disassembly_count_reads_every_fused_spelling(self):
        for mnemonic in ("vfmadd231sd", "vfnmadd213pd", "vfmsub132ss", "fmadd", "fmla"):
            self.assertEqual(fused_build_check.fused_instructions(f"\t{mnemonic} x\n"), 1, mnemonic)
        self.assertEqual(fused_build_check.fused_instructions(UNFUSED), 0)

    def test_the_command_line_exits_nonzero_on_a_problem(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "compile_commands.json").write_text(json.dumps([entry(QUATERNION, "")]))
            self.assertEqual(fused_build_check.main([str(tmp)], fused), 1)
            (Path(tmp) / "compile_commands.json").write_text(json.dumps(
                [entry(QUATERNION, "-ffp-contract=fast"), entry(AVERAGE, "-ffp-contract=fast")]))
            self.assertEqual(fused_build_check.main([str(tmp)], fused), 0)


if __name__ == "__main__":
    unittest.main()
