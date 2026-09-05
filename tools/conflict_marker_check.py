#!/usr/bin/env python3
"""Fail the build if a merge left its conflict markers behind.

Nothing else here would notice. The compilers only see C++ and TypeScript, and a marker in either
of those is a syntax error the build already catches — so the files actually at risk are the ones
no tool reads: the ADRs, the roadmap, the architecture notes. Documentation is a deliverable in
this repository (ADR 0007), and a document carrying `<<<<<<< HEAD` is a document that has stopped
being one, which is worse than an out-of-date one because it reads as damage rather than drift.

This exists because it happened: a resolution during a three-way merge left a `>>>>>>> origin/main`
line in docs/06-roadmap.md, the full gate went green over it, and it was caught by an ad-hoc grep
rather than by anything that would have run in CI.

Usage:  python3 tools/conflict_marker_check.py [repo_root]
"""
from __future__ import annotations

import os
import sys
from dataclasses import dataclass
from pathlib import Path

# Built rather than written out, so that this file and its tests can be scanned like everything
# else. Spelling them literally would make the checker flag itself, and the usual answer to that —
# excluding tools/ from the scan — would leave the checkers as the one tree where a bad merge goes
# unnoticed.
OURS = "<" * 7
THEIRS = ">" * 7
SPLIT = "=" * 7

# Directories that are not ours to police: dependencies, build output, other checkouts.
# Pruned by name wherever they appear — a `node_modules` is a `node_modules` at any depth.
SKIPPED_DIRS = frozenset({
    ".git", "node_modules", "dist", "emsdk-cache", ".claude",
})
# And these by their path from the repository root, because the names are ordinary English and
# only these locations are build output. Matched exactly rather than as a prefix: `build` should
# not also silence a directory called `buildings`.
SKIPPED_ROOTS = frozenset({"build", "shell/public/core"})

# Read as text; anything that is not decodes to replacement characters and simply will not match.
# A size ceiling because a marker lives in a hand-edited file, and walking a large binary line by
# line to prove it has none is work for nothing.
MAX_BYTES = 4 * 1024 * 1024


@dataclass(frozen=True)
class Marker:
    path: str
    line: int
    text: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.text.strip()}"


def is_marker(text: str) -> bool:
    """Whether a line is a conflict marker rather than prose that resembles one.

    `<<<<<<<` and `>>>>>>>` are followed by a branch name, so the space is part of the shape and
    requiring it costs nothing. `=======` stands alone, which makes it the one that could collide
    with prose — a setext heading underlined with exactly seven `=` would match. No such line
    exists in this repository, and a heading that needs exactly seven is a heading that can have
    eight, so the ambiguity is worth taking to catch a resolution that left only the middle.
    """
    if text.startswith(OURS + " ") or text.startswith(THEIRS + " "):
        return True
    return text.rstrip() == SPLIT


def check(root: Path) -> list[Marker]:
    """Every marker under `root`, in path order.

    Walked rather than globbed, and the skipped directories are pruned *before* descending. That
    is not premature: this repository's `node_modules`, `build/` and agent worktrees hold tens of
    thousands of files between them, and a glob that enumerates them all and then filters pays for
    every one. The cost also grows with the number of worktrees, which is the case where somebody
    is most likely to be running the gate by hand.
    """
    root = Path(root)
    found: list[Marker] = []

    for dirpath, dirnames, filenames in os.walk(root):
        here = Path(dirpath)
        # In place, because os.walk reads this list to decide where to go next.
        dirnames[:] = sorted(
            name for name in dirnames
            if name not in SKIPPED_DIRS
            and (here / name).relative_to(root).as_posix() not in SKIPPED_ROOTS
        )
        for name in sorted(filenames):
            path = here / name
            if not path.is_file():
                continue
            rel = path.relative_to(root).as_posix()
            try:
                if path.stat().st_size > MAX_BYTES:
                    continue
                body = path.read_text(errors="replace")
            except OSError:
                continue
            for number, text in enumerate(body.splitlines(), start=1):
                if is_marker(text):
                    found.append(Marker(rel, number, text))
    return found


def main(argv: list[str]) -> int:
    root = Path(argv[1]) if len(argv) > 1 else Path(__file__).resolve().parent.parent
    markers = check(root)
    if not markers:
        return 0
    print(f"{len(markers)} conflict marker(s) left behind:\n", file=sys.stderr)
    for marker in markers:
        print(f"  {marker}", file=sys.stderr)
    print("\nA merge was resolved halfway. Finish it before committing.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
