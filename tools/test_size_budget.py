"""Tests for the artifact size budget check."""
import subprocess
import sys
import gzip
import random
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import size_budget  # noqa: E402


def make_artifact(directory: Path, name: str, compressed_target: int) -> Path:
    """Write a file whose gzipped size is close to `compressed_target` bytes.

    The bytes have to be genuinely incompressible or the fixture lies: an arithmetic pattern
    looks random and gzips to almost nothing, which would make an "oversized" artifact pass.
    A seeded PRNG keeps it reproducible.
    """
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / name
    path.write_bytes(random.Random(name).randbytes(compressed_target))
    return path


class MeasureTest(unittest.TestCase):
    def test_measures_the_compressed_size_because_that_is_what_crosses_the_network(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "a.wasm"
            path.write_bytes(b"x" * 100_000)
            measured = size_budget.compressed_size(path)
            self.assertEqual(measured, len(gzip.compress(path.read_bytes(), mtime=0)))
            self.assertLess(measured, 100_000)


class CheckTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.build = Path(self._tmp.name) / "build"
        self.budgets = {"sphanorama-core.wasm": 50_000, "sphanorama-core.js": 20_000}

    def tearDown(self):
        self._tmp.cleanup()

    def test_a_build_within_budget_reports_no_failures(self):
        make_artifact(self.build, "sphanorama-core.wasm", 1_000)
        make_artifact(self.build, "sphanorama-core.js", 1_000)
        results = size_budget.check(self.build, self.budgets)
        self.assertTrue(all(r.within_budget for r in results))

    def test_an_oversized_artifact_is_reported(self):
        make_artifact(self.build, "sphanorama-core.wasm", 60_000)
        make_artifact(self.build, "sphanorama-core.js", 1_000)
        results = size_budget.check(self.build, self.budgets)
        over = [r for r in results if not r.within_budget]
        self.assertEqual(len(over), 1)
        self.assertEqual(over[0].name, "sphanorama-core.wasm")

    def test_a_missing_artifact_is_a_failure_not_a_pass(self):
        # Silently passing when the build produced nothing is the worst possible outcome: the
        # budget looks green precisely when the build is broken.
        make_artifact(self.build, "sphanorama-core.js", 1_000)
        results = size_budget.check(self.build, self.budgets)
        missing = [r for r in results if r.missing]
        self.assertEqual(len(missing), 1)
        self.assertFalse(missing[0].within_budget)

    def test_results_carry_the_headroom_so_a_creeping_budget_is_visible(self):
        make_artifact(self.build, "sphanorama-core.wasm", 1_000)
        make_artifact(self.build, "sphanorama-core.js", 1_000)
        results = size_budget.check(self.build, self.budgets)
        for r in results:
            self.assertGreater(r.budget, 0)
            self.assertGreater(r.budget - r.measured, 0)


class BudgetFileTest(unittest.TestCase):
    def test_the_committed_budget_file_declares_every_profile_ci_builds(self):
        budgets = size_budget.load_budgets(Path(__file__).parent.parent)
        self.assertIn("wasm-release", budgets)
        self.assertIn("wasm-release-threaded", budgets)

    def test_the_core_budget_matches_the_documented_eight_megabytes(self):
        budgets = size_budget.load_budgets(Path(__file__).parent.parent)
        self.assertEqual(budgets["wasm-release"]["sphanorama-core.wasm"], 8 * 1024 * 1024)


class CommandLineTest(unittest.TestCase):
    def script(self):
        return Path(__file__).parent / "size_budget.py"

    def test_exits_nonzero_when_the_build_directory_does_not_exist(self):
        r = subprocess.run([sys.executable, str(self.script()),
                            "--profile", "wasm-release", "--build-dir", "/nonexistent"],
                           capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)

    def test_reports_an_unknown_profile_rather_than_assuming_one(self):
        r = subprocess.run([sys.executable, str(self.script()),
                            "--profile", "not-a-profile", "--build-dir", "."],
                           capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not-a-profile", r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
