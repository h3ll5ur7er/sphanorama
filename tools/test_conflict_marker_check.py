"""Tests for the conflict-marker check.

The case that matters is a marker in a file no compiler reads. A marker in C++ or TypeScript is a
syntax error the build already catches; a marker in an ADR is a document that reads as damage, and
until this checker existed nothing in the gate would have said so.
"""
import subprocess
import sys
from unittest import mock
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import conflict_marker_check  # noqa: E402

# Built rather than written out, for the same reason the checker builds them: a test file spelling
# them literally would be flagged by the checker it is testing.
OURS = "<" * 7
BASE = "|" * 7
THEIRS = ">" * 7
SPLIT = "=" * 7


class Repo:
    """A real git repository, because the checker asks git what is in one.

    That is the point of the design rather than an inconvenience of testing it: the skipping rules
    under test are `.gitignore`'s, so a fixture that faked them would be asserting against a copy
    of the thing rather than the thing.
    """

    def __init__(self, root: Path):
        self.root = root
        subprocess.run(["git", "init", "-q"], cwd=root, check=True)
        # Pinned rather than inherited. A fixture that produces a real merge conflict produces
        # whatever shape the *developer's* global git config asks for, so a machine with
        # `merge.conflictStyle = diff3` set would see a fourth marker in every conflict here and
        # fail tests that counted three. Set explicitly so what these fixtures build is a property
        # of the test rather than of whoever is running it; the diff3 test overrides it, which is
        # the only place the style is the subject.
        for key, value in (("user.email", "t@example.invalid"), ("user.name", "t"),
                           ("merge.conflictStyle", "merge")):
            subprocess.run(["git", "config", key, value], cwd=root, check=True)

    def write(self, rel: str, body: str = ""):
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body)
        return path

    def ignore(self, *patterns: str):
        self.write(".gitignore", "".join(f"{p}\n" for p in patterns))


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

    def test_catches_the_base_marker_a_diff3_merge_leaves(self):
        # `merge.conflictStyle = diff3` and `zdiff3` add a fourth marker naming the common
        # ancestor. A resolution that left it behind is exactly as broken as one that left the
        # others, and until now it would have passed.
        self.repo.write("docs/a.md", f"{BASE} c13ba62\nbase text\n")
        self.repo.write("docs/b.md", f"{BASE}\n")   # git can write it without a description
        self.assertEqual(sorted(m.path for m in self.markers()), ["docs/a.md", "docs/b.md"])

    def test_catches_a_conflict_git_itself_produced_in_diff3_style(self):
        # The shape asserted above, taken from git rather than from memory. A unit test that
        # hard-codes a format is only as good as whoever typed it; this one breaks if git ever
        # writes something else.
        root = self.repo.root
        run = lambda *a: subprocess.run(a, cwd=root, check=True,
                                        capture_output=True, text=True)
        # The one place the style is the subject rather than the background.
        run("git", "config", "merge.conflictStyle", "diff3")
        self.repo.write("docs/a.md", "base\n")
        run("git", "add", "docs/a.md")
        run("git", "commit", "-qm", "base")
        run("git", "checkout", "-qb", "other")
        self.repo.write("docs/a.md", "theirs\n")
        run("git", "commit", "-qam", "theirs")
        run("git", "checkout", "-q", "-")
        self.repo.write("docs/a.md", "ours\n")
        run("git", "commit", "-qam", "ours")
        subprocess.run(["git", "merge", "other"], cwd=root, capture_output=True, text=True)

        shapes = {m.text.split(" ")[0].rstrip() for m in self.markers()}
        self.assertEqual(shapes, {OURS, BASE, SPLIT, THEIRS})

    def test_a_file_it_cannot_read_is_an_error_not_a_pass(self):
        # The same refusal the git failure gets. A tracked file this cannot read is a file it
        # cannot clear, and swallowing the error would make an unreadable tree look like a clean
        # one — the false pass this checker exists to prevent, in its own machinery.
        self.repo.write("docs/a.md", "fine\n")
        with mock.patch.object(Path, "read_text", side_effect=OSError("permission denied")):
            with self.assertRaises(RuntimeError):
                self.markers()

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
                        "| col | col |\n|-----|-----|\n"
                        + "<" * 8 + " not this either\n"
                        + THEIRS + "no space, so not a marker\n")
        self.assertEqual(self.markers(), [])

    def test_a_bare_split_counts(self):
        # The middle marker on its own is what a half-finished resolution leaves. It is also the
        # one shape that could collide with a setext heading, which is why the check is anchored
        # to exactly seven and documented.
        self.repo.write("docs/notes.md", f"text\n{SPLIT}\nmore\n")
        self.assertEqual([m.line for m in self.markers()], [2])

    def test_what_git_ignores_is_not_ours_to_police(self):
        # The skip rules are `.gitignore`'s, whatever it happens to say. Listed here as the real
        # one lists them, so a scratch tree added there later is skipped without touching this.
        self.repo.ignore("node_modules/", "build/", "dist/", ".claude/worktrees/",
                         ".venv/", "captures/", "playwright-report/")
        self.repo.write("node_modules/pkg/index.js", f"{OURS} HEAD\n")
        self.repo.write("build/native-debug/x.h", f"{THEIRS} origin/main\n")
        self.repo.write("dist/app.js", f"{SPLIT}\n")
        self.repo.write(".claude/worktrees/agent-x/docs/a.md", f"{OURS} HEAD\n")
        self.repo.write(".venv/lib/thing.py", f"{OURS} HEAD\n")
        self.repo.write("captures/run/frame.md", f"{THEIRS} origin/main\n")
        self.repo.write("playwright-report/index.html", f"{SPLIT}\n")
        self.assertEqual(self.markers(), [])

    def test_tracked_content_under_a_partly_ignored_directory_is_still_scanned(self):
        # The hole this replaced: the old skip list pruned all of `.claude`, but only
        # `.claude/worktrees/` is scratch — `.claude/skills/` is tracked, and it is the file that
        # tells everyone how to work in this repository. A marker there went unnoticed.
        self.repo.ignore(".claude/worktrees/")
        self.repo.write(".claude/worktrees/agent-x/notes.md", f"{OURS} HEAD\n")
        self.repo.write(".claude/skills/sphanorama-engineering/SKILL.md",
                        f"# Skill\n{THEIRS} origin/main\n")
        self.assertEqual([m.path for m in self.markers()],
                         [".claude/skills/sphanorama-engineering/SKILL.md"])

    def test_a_file_in_conflict_is_counted_once(self):
        # During an unresolved merge `git ls-files` emits the conflicted path once per stage —
        # base, ours, theirs — so the file arrives three times and every marker in it was reported
        # three times. Which is precisely when this check runs and precisely when its output most
        # needs to be readable: the merge that left the markers has not been finished yet.
        root = self.repo.root
        run = lambda *a: subprocess.run(a, cwd=root, check=True, capture_output=True, text=True)
        self.repo.write("docs/a.md", "base\n")
        run("git", "add", "docs/a.md")
        run("git", "commit", "-qm", "base")
        run("git", "checkout", "-qb", "other")
        self.repo.write("docs/a.md", "theirs\n")
        run("git", "commit", "-qam", "theirs")
        run("git", "checkout", "-q", "-")
        self.repo.write("docs/a.md", "ours\n")
        run("git", "commit", "-qam", "ours")
        subprocess.run(["git", "merge", "other"], cwd=root, capture_output=True, text=True)

        # Three markers in the file, each once — not nine.
        found = self.markers()
        self.assertEqual(len(found), 3)
        self.assertEqual(sorted(m.line for m in found), [1, 3, 5])

    def test_a_file_staged_but_not_yet_committed_counts(self):
        # `--cached --others` is the set that can become a commit, which is the set that matters.
        # A marker is caught before it is committed or not at all.
        self.repo.write("docs/new.md", f"{OURS} HEAD\n")
        subprocess.run(["git", "add", "docs/new.md"], cwd=self.repo.root, check=True)
        self.assertEqual([m.path for m in self.markers()], ["docs/new.md"])

    def test_a_tree_git_will_not_answer_for_is_an_error_not_a_pass(self):
        # The failure this checker exists to prevent, in its own machinery: a check that cannot
        # run must not be indistinguishable from a check that found nothing.
        with tempfile.TemporaryDirectory() as outside:
            with self.assertRaises(RuntimeError):
                conflict_marker_check.check(Path(outside))

    def test_a_name_that_merely_starts_like_build_output_is_still_scanned(self):
        # The skip list was a prefix match, so `build` also silenced anything whose path began
        # with those letters. Build output lives at exactly two known paths; a directory that
        # happens to share a prefix with one is ordinary content and its documents count.
        self.repo.write("buildings/notes.md", f"{THEIRS} origin/main\n")
        self.repo.write("shell/public/manifest.json", f"{OURS} HEAD\n")
        self.assertEqual(sorted(m.path for m in self.markers()),
                         ["buildings/notes.md", "shell/public/manifest.json"])

    def test_reports_a_nonzero_exit_and_names_the_file(self):
        self.repo.write("docs/06-roadmap.md", f"{THEIRS} origin/main\n")
        result = subprocess.run(
            [sys.executable, str(Path(__file__).parent / "conflict_marker_check.py"),
             str(self.repo.root)],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("docs/06-roadmap.md", result.stderr)

    def test_a_clean_tree_exits_zero(self):
        self.repo.write("docs/a.md", "nothing wrong here\n")
        result = subprocess.run(
            [sys.executable, str(Path(__file__).parent / "conflict_marker_check.py"),
             str(self.repo.root)],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
