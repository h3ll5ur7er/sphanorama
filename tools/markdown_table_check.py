#!/usr/bin/env python3
"""Fail the build if a markdown table has been broken open.

A GFM table ends at the first blank line. Put a paragraph between two of its rows — to explain one
of them, which is the natural thing to want — and every row below the gap stops being a table and
renders as literal pipe text. Nothing about the source looks wrong: the rows are still there, still
aligned, still readable in an editor. Only the rendered page is broken, and only from the gap down.

This exists because it happened. A note about one axis was inserted between the V5 and V6 rows of
`docs/02-volatility-map.md`; eleven rows — every engine, resource access and owner from V6 to V16 —
rendered as pipes. Four review rounds read the paragraph's prose and none rendered the page, and
the gate had no step that would.

Documentation is a deliverable here (ADR 0007) and the volatility map is where a reader goes to
find out which component owns a decision. A table that stops halfway is worse than an out-of-date
one, because it does not read as damage — it reads as a shorter table.

What it checks, and deliberately no more: that no run of table-shaped lines is separated from its
header by a blank line. It does not validate column counts, alignment or escaping — a renderer is
the authority on those, and a checker that half-implements one drifts from it.

Usage:  uv run tools/markdown_table_check.py [repo_root]
"""
from __future__ import annotations

import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

# The same question `conflict_marker_check.py` asks, for the same reason: what git would let you
# commit, rather than a hand-written skip list that drifts from `.gitignore`.
LS_FILES = ("git", "ls-files", "-z", "--cached", "--others", "--exclude-standard")

# A delimiter row: pipes separating runs of dashes, with optional alignment colons. This is what
# turns the line above it into a header, and it is the only unambiguous marker a GFM table has.
DELIMITER = re.compile(r"^\s*\|?\s*:?-{1,}:?\s*(\|\s*:?-{1,}:?\s*)*\|?\s*$")


def looks_like_row(text: str) -> bool:
    """Whether a line would be a table row if it were in a table.

    Leading pipe only. A sentence containing a pipe mid-way is prose — the shell pipelines in the
    roadmap are exactly that — and treating those as rows would report a finding on every document
    that mentions a command.
    """
    return text.lstrip().startswith("|")


@dataclass(frozen=True)
class Break:
    path: str
    line: int
    text: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.text.strip()[:72]}"


def cells(text: str) -> int:
    """How many columns a row-shaped line has.

    The shape is what tells an orphaned row from a pipe-drawn diagram that happens to sit under a
    table: rows separated from their header still have the header's column count, because they
    were written as part of it. Comparing shapes rather than merely finding pipes is what keeps
    this from reporting every document that draws a box.
    """
    return len([cell for cell in text.strip().strip("|").split("|")])


def breaks_in(body: str) -> list[tuple[int, str]]:
    """Rows that render as text because a blank line closed the table above them.

    Walks the file rather than parsing it. Find a header and its delimiter, follow the rows to the
    blank line that ends the table, then keep reading: any later run of row-shaped lines that does
    *not* begin a table of its own, and has the same column count as the header, is a run somebody
    meant to be part of the table above.

    Bounded at the next heading, because past a section break it is a different subject and a
    coincidence of shape is more likely than a mistake.

    A first version of this walked only the blank run after the table and asked whether the next
    line was row-shaped. It was not — the whole failure is that a *paragraph* sits in the gap — so
    the checker reported nothing on the very document that prompted it. Its tests are what said so.
    """
    found: list[tuple[int, str]] = []
    lines = body.splitlines()
    fenced = False
    index = 0
    while index < len(lines):
        text = lines[index]
        if text.lstrip().startswith("```"):
            fenced = not fenced
            index += 1
            continue
        if fenced or not DELIMITER.match(text) or index == 0 or not looks_like_row(lines[index - 1]):
            index += 1
            continue

        width = cells(lines[index - 1])
        cursor = index + 1
        while cursor < len(lines) and looks_like_row(lines[cursor]):
            cursor += 1

        # Past the table, looking for rows that were separated from it.
        scan = cursor
        while scan < len(lines):
            here = lines[scan]
            if here.lstrip().startswith("```") or here.lstrip().startswith("#"):
                break
            if looks_like_row(here):
                starts_a_table = scan + 1 < len(lines) and DELIMITER.match(lines[scan + 1])
                if not starts_a_table and cells(here) == width:
                    found.append((scan + 1, here))
                # Either way, skip the whole run: one report per break is what a reader needs.
                while scan < len(lines) and looks_like_row(lines[scan]):
                    scan += 1
                if starts_a_table:
                    break
                continue
            scan += 1
        index = max(cursor, index + 1)
    return found


def tracked_files(root: Path) -> list[str]:
    """Every markdown path git would let you commit, relative to `root`."""
    result = subprocess.run(LS_FILES, cwd=root, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"git could not list this tree: {result.stderr.strip()}")
    return sorted({n for n in result.stdout.split("\0") if n and n.endswith(".md")})


def check(root: Path) -> list[Break]:
    root = Path(root)
    found: list[Break] = []
    for rel in tracked_files(root):
        path = root / rel
        if not path.is_file():
            continue
        try:
            body = path.read_text(errors="replace")
        except OSError as failure:
            # Reported rather than skipped, for the reason the sibling checker gives: a file this
            # cannot read is one it cannot clear, and a silent skip is a false pass.
            raise RuntimeError(f"{rel} could not be read: {failure}") from failure
        for number, text in breaks_in(body):
            found.append(Break(rel, number, text))
    return found


def main(argv: list[str]) -> int:
    root = Path(argv[1]) if len(argv) > 1 else Path(__file__).resolve().parent.parent
    broken = check(root)
    if not broken:
        return 0
    print(f"{len(broken)} table row(s) render as text, not as a table:\n", file=sys.stderr)
    for entry in broken:
        print(f"  {entry}", file=sys.stderr)
    print(
        "\nA blank line closes a markdown table. Move the paragraph below the last row.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
