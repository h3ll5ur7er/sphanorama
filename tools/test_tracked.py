#!/usr/bin/env python3
"""Tests for the shared "which files are in this repository" listing.

Three checkers ask this question and none of them had a test for the answer, which is how a
decoding bug in twelve characters of `subprocess.run` went unnoticed in all three at once. The
cases here are about the two things a path is that a string is not: it is bytes, and those bytes
may contain anything except NUL and `/`.

A real git repository rather than a fake, for the reason the other fixtures give: what counts as a
file here is git's answer, and a fixture that reimplemented that rule would assert against a copy
of it.
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from tracked import indexed_files, tracked_files  # noqa: E402


class Listing(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        subprocess.run(["git", "init", "-q"], cwd=self.root, check=True)

    def write(self, name: bytes, content: bytes = b"hello\n") -> None:
        """Write a file whose *name* is the given bytes, which need not be UTF-8."""
        with open(Path(self.directory.name).joinpath(Path(name.decode("utf-8", "surrogateescape"))),
                  "wb") as handle:
            handle.write(content)

    def commit(self) -> None:
        subprocess.run(["git", "add", "-A"], cwd=self.root, check=True)
        subprocess.run(["git", "-c", "user.email=t@t", "-c", "user.name=t", "commit", "-qm", "x"],
                       cwd=self.root, check=True)

    def test_a_name_that_is_not_utf8_is_listed_rather_than_raised(self):
        # `text=True` decoded git's output as UTF-8, so a file named in any other encoding — an
        # editor writing `café.md` as Latin-1 — raised `UnicodeDecodeError` out of
        # `subprocess._translate_newlines`. All three checkers died with a traceback naming a line
        # of the standard library, about a repository that is merely unusual.
        self.write(b"caf\xe9.md")
        self.commit()
        listed = tracked_files(self.root)
        self.assertEqual(len(listed), 1, listed)
        # And the name round-trips to the file it names, which is the whole point of
        # `surrogateescape` over a decode that substitutes: a caller opens what git listed.
        self.assertEqual((self.root / listed[0]).read_bytes(), b"hello\n")

    def test_a_carriage_return_in_a_name_is_not_turned_into_a_newline(self):
        # The silent half, and the worse one. `-z` asks git for NUL-separated paths precisely so a
        # newline inside one cannot be read as a separator — and then `text=True`'s universal-newline
        # translation put a newline there anyway. The listed path did not exist, so the provenance
        # walk could not find the file and reported nothing: exit 0 on a committed, unrecorded
        # binary, which is the one failure that checker exists to prevent.
        self.write(b"stol\renn.jpg", bytes(range(256)) * 700)
        self.commit()
        listed = tracked_files(self.root)
        self.assertEqual(listed, ["stol\renn.jpg"], listed)
        self.assertTrue((self.root / listed[0]).is_file(),
                        "the listed path does not name a file that exists")

    def test_a_newline_in_a_name_is_one_path_and_not_two(self):
        # The separator question asked directly, since the case above only shows the damage a
        # translated `\r` does. `-z` is what makes this hold; `git ls-files` without it quotes such
        # a path instead, which is a different wrong answer.
        self.write(b"two\nlines.jpg", bytes(range(256)) * 700)
        self.commit()
        self.assertEqual(tracked_files(self.root), ["two\nlines.jpg"])

    def test_the_two_listings_differ_by_exactly_the_files_nobody_has_added(self):
        # The distinction the provenance checker's licence rule turns on: `tracked_files` answers
        # "what may this repository commit" and `indexed_files` "what would a checkout contain".
        # Asserted as a difference rather than separately, because a change that collapsed them
        # would leave two tests passing independently.
        self.write(b"committed.md")
        self.commit()
        self.write(b"written-but-not-added.md")
        self.assertEqual(tracked_files(self.root), ["committed.md", "written-but-not-added.md"])
        self.assertEqual(indexed_files(self.root), ["committed.md"])

    def test_a_path_at_three_merge_stages_is_listed_once(self):
        # The dedup this module's comment is about, which nothing here reached: `--cached` lists a
        # conflicted path once per stage — base, ours, theirs — and that is exactly when the marker
        # check runs, so without the `set()` every marker in a conflicted file is reported three
        # times in the output somebody is reading to find them. Staged by hand rather than by
        # provoking a real merge, because the three stages are the whole of what matters.
        self.write(b"conflicted.md")
        self.commit()
        blob = subprocess.run(["git", "hash-object", "-w", "--stdin"], cwd=self.root,
                              input=b"x", capture_output=True, check=True).stdout.decode().strip()
        subprocess.run(["git", "rm", "--cached", "-q", "conflicted.md"], cwd=self.root, check=True)
        for stage in (1, 2, 3):
            subprocess.run(["git", "update-index", "--add", "--cacheinfo",
                            f"100644,{blob},conflicted.md"] if stage == 1 else
                           ["git", "update-index", "--index-info"],
                           cwd=self.root, check=True,
                           input=f"100644 {blob} {stage}\tconflicted.md\n".encode()
                           if stage != 1 else None,
                           capture_output=True)
        listed = [name for name in tracked_files(self.root) if name == "conflicted.md"]
        self.assertEqual(listed, ["conflicted.md"], tracked_files(self.root))

    def test_a_git_that_will_not_answer_is_raised_rather_than_read_as_an_empty_tree(self):
        # Returning "no files" from a git that refused makes an unrunnable check indistinguishable
        # from a clean one, which is the shape of bug every caller of this exists to catch.
        with tempfile.TemporaryDirectory() as outside:
            # A directory with no `.git` anywhere above it, so `git ls-files` refuses rather than
            # answering about some enclosing repository. `TMPDIR` is not inside a checkout.
            with self.assertRaises(RuntimeError) as refused:
                tracked_files(Path(outside))
        self.assertIn("could not list this tree", str(refused.exception))


if __name__ == "__main__":
    unittest.main(verbosity=2)
