#!/usr/bin/env python3
"""Tests for the markdown table checker.

The checker's own failure mode is silence, so the cases that matter most are the ones it must
*not* report: a document that mentions a shell pipeline, a table followed by ordinary prose, a
fenced example. A checker nobody can leave green is one somebody turns off.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
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


if __name__ == "__main__":
    unittest.main()
