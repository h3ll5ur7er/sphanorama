"""Tests for the conflict-marker check.

The case that matters is a marker in a file no compiler reads. A marker in C++ or TypeScript is a
syntax error the build already catches; a marker in an ADR is a document that reads as damage, and
until this checker existed nothing in the gate would have said so.
"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import conflict_marker_check  # noqa: E402

# Built rather than written out, for the same reason the checker builds them: a test file spelling
# them literally would be flagged by the checker it is testing.
OURS = "<" * 7
THEIRS = ">" * 7
SPLIT = "=" * 7


class Repo:
    def __init__(self, root: Path):
        self.root = root

    def write(self, rel: str, body: str = ""):
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body)
        return path


class ConflictMarkerCheckTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Repo(Path(self._tmp.name))

    def tearDown(self):
        self._tmp.cleanup()

    def markers(self):
        return conflict_marker_check.check(self.repo.root)

    def test_a_finished_merge_passes(self):
        self.repo.write("docs/06-roadmap.md", "# Roadmap\n\nA paragraph that survived.\n")
        self.repo.write("core/src/managers/thing.cpp", "int f() { return 1; }\n")
        self.assertEqual(self.markers(), [])

    def test_catches_the_line_that_actually_slipped_through(self):
        # The real one: a resolution that removed the head and split lines and left the tail. The
        # full gate went green over exactly this.
        self.repo.write("docs/06-roadmap.md", f"A resolved paragraph.\n{THEIRS} origin/main\n")
        found = self.markers()
        self.assertEqual([m.line for m in found], [2])
        self.assertEqual(found[0].path, "docs/06-roadmap.md")

    def test_catches_all_three_sides_of_a_conflict(self):
        self.repo.write("docs/adr/0001-a.md",
                        f"{OURS} HEAD\nours\n{SPLIT}\ntheirs\n{THEIRS} origin/main\n")
        self.assertEqual([m.line for m in self.markers()], [1, 3, 5])

    def test_a_marker_in_source_is_caught_too(self):
        # Redundant against the compiler and kept anyway: the checker should not carry an opinion
        # about which trees are allowed to be broken.
        self.repo.write("shell/src/main.ts", f"const a = 1;\n{OURS} HEAD\n")
        self.assertEqual([m.path for m in self.markers()], ["shell/src/main.ts"])

    def test_prose_that_merely_resembles_a_marker_is_left_alone(self):
        # Arrows and rules are ordinary in these documents, and a checker that cried wolf over
        # them would be turned off. The shape is seven of the character and then a space.
        self.repo.write("docs/03-architecture.md",
                        "Clients " + ">" * 3 + " Managers\n"
                        "A rule: " + "=" * 40 + "\n"
                        + "<" * 8 + " not this either\n"
                        + THEIRS + "no space, so not a marker\n")
        self.assertEqual(self.markers(), [])

    def test_a_bare_split_counts(self):
        # The middle marker on its own is what a half-finished resolution leaves. It is also the
        # one shape that could collide with a setext heading, which is why the check is anchored
        # to exactly seven and documented.
        self.repo.write("docs/notes.md", f"text\n{SPLIT}\nmore\n")
        self.assertEqual([m.line for m in self.markers()], [2])

    def test_dependencies_and_build_output_are_not_ours_to_police(self):
        self.repo.write("node_modules/pkg/index.js", f"{OURS} HEAD\n")
        self.repo.write("build/native-debug/x.h", f"{THEIRS} origin/main\n")
        self.repo.write("dist/app.js", f"{SPLIT}\n")
        self.repo.write(".claude/worktrees/agent-x/docs/a.md", f"{OURS} HEAD\n")
        self.assertEqual(self.markers(), [])

    def test_reports_a_nonzero_exit_and_names_the_file(self):
        self.repo.write("docs/06-raodmap.md", f"{THEIRS} origin/main\n")
        result = subprocess.run(
            [sys.executable, str(Path(__file__).parent / "conflict_marker_check.py"),
             str(self.repo.root)],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("docs/06-raodmap.md", result.stderr)

    def test_a_clean_tree_exits_zero(self):
        self.repo.write("docs/a.md", "nothing wrong here\n")
        result = subprocess.run(
            [sys.executable, str(Path(__file__).parent / "conflict_marker_check.py"),
             str(self.repo.root)],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
