"""Tests for the no-browser check.

The core has to compile and run without a browser, because that is what makes managers testable
natively and what keeps platform assumptions out of business logic. bridge/ is the one tree
allowed to know Emscripten exists.
"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import no_browser_check  # noqa: E402


class Repo:
    def __init__(self, root: Path):
        self.root = root

    def write(self, rel: str, body: str = ""):
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body)
        return path


class NoBrowserCheckTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Repo(Path(self._tmp.name))

    def tearDown(self):
        self._tmp.cleanup()

    def violations(self):
        return no_browser_check.check(self.repo.root)

    def test_a_clean_core_passes(self):
        self.repo.write("core/src/engines/pose_engine.cpp", "#include <cmath>\nint f() { return 1; }\n")
        self.assertEqual(self.violations(), [])

    def test_emscripten_include_in_the_core_is_a_violation(self):
        self.repo.write("core/src/engines/pose_engine.cpp", "#include <emscripten/bind.h>\n")
        self.assertTrue(self.violations())

    def test_emscripten_macro_in_the_core_is_a_violation(self):
        # The ifdef form is the one that sneaks in: it compiles natively, so nothing fails until
        # the two builds have quietly diverged in behaviour.
        self.repo.write("core/src/managers/project_manager.cpp",
                        "#ifdef __EMSCRIPTEN__\nint g() { return 2; }\n#endif\n")
        self.assertTrue(self.violations())

    def test_inline_javascript_in_the_core_is_a_violation(self):
        self.repo.write("core/src/engines/pose_engine.cpp", 'EM_ASM({ console.log(1); });\n')
        self.assertTrue(self.violations())

    def test_contracts_may_not_reference_the_browser_either(self):
        self.repo.write("contracts/cpp/sphanorama/types.h", "#include <emscripten.h>\n")
        self.assertTrue(self.violations())

    def test_the_bridge_is_allowed_to_know_about_emscripten(self):
        self.repo.write("bridge/module.cpp", "#include <emscripten/bind.h>\nEM_ASM({});\n")
        self.assertEqual(self.violations(), [])

    def test_a_violation_names_the_file_and_the_line(self):
        self.repo.write("core/src/engines/pose_engine.cpp", "#include <cmath>\n#include <emscripten.h>\n")
        violations = self.violations()
        self.assertEqual(len(violations), 1)
        self.assertIn("pose_engine.cpp", str(violations[0]))
        self.assertIn("2", str(violations[0]))

    def test_the_word_emscripten_in_a_comment_is_still_reported(self):
        # Deliberately blunt. A comment saying "on Emscripten we do X" is a sign the core is
        # reasoning about the platform, which is the thing being prevented.
        self.repo.write("core/src/engines/pose_engine.cpp", "// on emscripten this differs\n")
        self.assertTrue(self.violations())


class RealRepoTest(unittest.TestCase):
    def test_this_repository_passes(self):
        root = Path(__file__).parent.parent
        self.assertEqual(no_browser_check.check(root), [])


class CommandLineTest(unittest.TestCase):
    def test_exit_zero_on_this_repository(self):
        root = Path(__file__).parent.parent
        r = subprocess.run([sys.executable, str(root / "tools/no_browser_check.py")],
                           capture_output=True, text=True, cwd=str(root))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
