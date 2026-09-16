#!/usr/bin/env python3
"""Tests for the markdown table checker.

The checker's own failure mode is silence, so the cases that matter most are the ones it must
*not* report: a document that mentions a shell pipeline, a table followed by ordinary prose, a
fenced example. A checker nobody can leave green is one somebody turns off.
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import markdown_table_check  # noqa: E402
import reading  # noqa: E402
from markdown_table_check import breaks_in  # noqa: E402


WHOLE = """| # | Volatility | Owner |
| - | --------- | ----- |
| V1 | how a capture is sequenced | CaptureSessionManager |
| V2 | how a panorama is built | PanoramaBuildManager |

Prose about the table, below all of it.
"""

SPLIT = """| # | Volatility | Owner |
| - | --------- | ----- |
| V1 | how a capture is sequenced | CaptureSessionManager |

A note about V1, in the wrong place.

| V2 | how a panorama is built | PanoramaBuildManager |
"""


class TableBreaks(unittest.TestCase):
    def test_a_whole_table_is_clean(self):
        self.assertEqual(breaks_in(WHOLE), [])

    def test_a_paragraph_between_rows_is_reported(self):
        found = breaks_in(SPLIT)
        self.assertEqual(len(found), 1)
        line, text = found[0]
        self.assertEqual(line, 7)
        self.assertIn("V2", text)

    def test_the_real_regression(self):
        """The shape that got through: the note went between V5 and V6 of the volatility map."""
        body = (
            "| # | Volatility | Owner |\n| - | - | - |\n"
            + "".join(f"| V{n} | axis | owner |\n" for n in range(1, 6))
            + "\nSensor absence moved out of V5 and into V1.\n\n"
            + "".join(f"| V{n} | axis | owner |\n" for n in range(6, 17))
        )
        found = breaks_in(body)
        self.assertEqual(len(found), 1)
        self.assertIn("V6", found[0][1])

    def test_prose_containing_a_pipe_is_not_a_row(self):
        # The roadmap is full of shell pipelines. A leading pipe is what makes a row.
        body = "| a | b |\n| - | - |\n| 1 | 2 |\n\nRun `git ls-files | wc -l` to count them.\n"
        self.assertEqual(breaks_in(body), [])

    def test_two_tables_separated_by_prose_are_both_whole(self):
        body = WHOLE + "\n| x | y |\n| - | - |\n| 1 | 2 |\n"
        self.assertEqual(breaks_in(body), [])

    def test_a_fenced_example_is_skipped(self):
        # A document about markdown contains a broken table deliberately; the checker's own
        # docstring is the first example in this repository.
        body = "```\n| a | b |\n| - | - |\n| 1 | 2 |\n\n| 3 | 4 |\n```\n"
        self.assertEqual(breaks_in(body), [])

    def test_a_table_with_no_delimiter_is_not_a_table(self):
        # Without the dashes row it is prose that happens to use pipes, and a gap in it is not a
        # break — reporting one would flag any document drawing a diagram with pipes.
        body = "| a | b |\n| c | d |\n\n| e | f |\n"
        self.assertEqual(breaks_in(body), [])

    def test_an_aligned_delimiter_still_counts(self):
        body = "| a | b |\n|:--|--:|\n| 1 | 2 |\n\n| 3 | 4 |\n"
        self.assertEqual(len(breaks_in(body)), 1)


class WalkingARepository(unittest.TestCase):
    """`check` and `main`, which had no case at all.

    The suite above covers `breaks_in` thoroughly and nothing else, so `check` could be replaced by
    `return []` — and `markdown_files`, the bounded read and `main`'s refusal arm could each be
    removed — with every Python suite green and the build at exit 0. A checker whose walk is
    untested is a checker that can quietly stop walking.
    """

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        subprocess.run(["git", "init", "-q"], cwd=self.root, check=True)

    def write(self, name, text, track=True):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        if track:
            subprocess.run(["git", "add", "--", name], cwd=self.root, check=True)
        return path

    def test_a_broken_table_in_a_tracked_file_is_found_and_located(self):
        self.write("docs/a.md", SPLIT)
        broken = markdown_table_check.check(self.root)
        self.assertEqual([entry.path for entry in broken], ["docs/a.md"])
        self.assertEqual(markdown_table_check.main(["checker", str(self.root)]), 1)

    def test_a_whole_table_is_clean_and_exits_zero(self):
        self.write("docs/a.md", WHOLE)
        self.assertEqual(markdown_table_check.check(self.root), [])
        self.assertEqual(markdown_table_check.main(["checker", str(self.root)]), 0)

    def test_a_file_that_is_not_markdown_is_not_walked(self):
        # `markdown_files` filters by suffix, and without it this content — which is a broken table
        # — would be reported from a `.txt` nobody writes tables in.
        self.write("docs/a.txt", SPLIT)
        self.assertEqual(markdown_table_check.check(self.root), [])

    def test_an_untracked_markdown_file_is_still_walked(self):
        # `--others` is the half this branch added, and it is the reason the ceiling below matters:
        # a file nobody has committed is exactly the one most likely to be enormous.
        self.write("docs/a.md", SPLIT, track=False)
        self.assertEqual([entry.path for entry in markdown_table_check.check(self.root)],
                         ["docs/a.md"])

    def test_a_file_past_the_ceiling_is_skipped_rather_than_read(self):
        # Skipped, not reported: a markdown table does not live past four megabytes, and a file
        # that big has nothing this checker can say about it. Asserted by lowering the ceiling
        # rather than by writing four megabytes, because the property is "past the ceiling".
        self.addCleanup(setattr, markdown_table_check, "MAX_BYTES",
                        markdown_table_check.MAX_BYTES)
        markdown_table_check.MAX_BYTES = 64
        self.write("docs/a.md", SPLIT)
        self.assertGreater(len(SPLIT), markdown_table_check.MAX_BYTES, "the fixture is under it")
        self.assertEqual(markdown_table_check.check(self.root), [])
        # And the ceiling is not simply "refuse everything".
        markdown_table_check.MAX_BYTES = len(SPLIT)
        self.assertEqual([entry.path for entry in markdown_table_check.check(self.root)],
                         ["docs/a.md"])

    def test_a_file_it_cannot_read_is_an_error_not_a_pass(self):
        # The same refusal the git failure gets, and the reason it is a raise: a tracked file this
        # cannot read is a file it cannot clear, so a silent skip would be a false pass.
        self.write("docs/a.md", WHOLE)
        with mock.patch.object(reading, "text", side_effect=OSError("permission denied")):
            with self.assertRaises(RuntimeError):
                markdown_table_check.check(self.root)

    def test_and_main_turns_that_refusal_into_a_sentence(self):
        self.write("docs/a.md", WHOLE)
        with mock.patch.object(reading, "text", side_effect=OSError("permission denied")):
            self.assertEqual(markdown_table_check.main(["checker", str(self.root)]), 1)

    def test_a_directory_that_is_not_a_repository_is_an_error_and_not_an_empty_answer(self):
        with tempfile.TemporaryDirectory() as bare:
            with self.assertRaises(RuntimeError):
                markdown_table_check.check(Path(bare))
if __name__ == "__main__":
    unittest.main()
