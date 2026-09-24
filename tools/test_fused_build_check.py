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


def entry(path: str, flags: str, form: str = "command") -> dict:
    line = f"/usr/bin/clang++ -O3 {flags} -c {path}"
    if form == "arguments":
        return {"directory": "/build", "file": path, "arguments": line.split()}
    return {"directory": "/build", "file": path, "command": line}


class FusedBuildCheck(unittest.TestCase):
    def check(self, entries):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "compile_commands.json").write_text(json.dumps(entries))
            return fused_build_check.problems(Path(tmp))

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
            found = fused_build_check.problems(Path(tmp))
        self.assertEqual(len(found), 1)
        self.assertIn("configure", found[0])

    def test_an_unreadable_database_says_so(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "compile_commands.json").write_text("{not json")
            found = fused_build_check.problems(Path(tmp))
        self.assertEqual(len(found), 1)
        self.assertIn("could not be read", found[0])

    def test_the_command_line_exits_nonzero_on_a_problem(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "compile_commands.json").write_text(json.dumps([entry(QUATERNION, "")]))
            self.assertEqual(fused_build_check.main([str(tmp)]), 1)
            (Path(tmp) / "compile_commands.json").write_text(
                json.dumps([entry(QUATERNION, "-ffp-contract=fast")]))
            self.assertEqual(fused_build_check.main([str(tmp)]), 0)


if __name__ == "__main__":
    unittest.main()
