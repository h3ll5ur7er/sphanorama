#!/usr/bin/env python3
"""Fail the build if a merge left its conflict markers behind.

Nothing else here would notice. The compilers only see C++ and TypeScript, and a marker in either
of those is a syntax error the build already catches — so the files actually at risk are the ones
no tool reads: the ADRs, the roadmap, the architecture notes. Documentation is a deliverable in
this repository (ADR 0007), and a document carrying a half-finished merge is a document that has
stopped being one — worse than an out-of-date one, because it reads as damage rather than drift.

This exists because it happened: a resolution during a three-way merge left a tail marker in
docs/06-roadmap.md, the full gate went green over it, and it was caught by an ad-hoc grep rather
than by anything that would have run in CI.

The markers are described rather than spelled anywhere in this file. The check is anchored to the
start of a line, so quoting one mid-sentence would in fact be safe — but only until somebody
reflows the paragraph and it lands in column one, and a checker that can be broken by rewrapping
its own documentation is not one to rely on.

Usage:  uv run tools/conflict_marker_check.py [repo_root]
"""
from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path

import reading
from tracked import tracked_files

# Built from repeated characters, so that no line of this file or its tests can be one. Spelling
# them out would eventually make the checker flag itself, and the usual answer to that — excluding
# tools/ from the scan — would leave the checkers as the one tree where a bad merge goes unnoticed.
OURS = "<" * 7
BASE = "|" * 7
THEIRS = ">" * 7
SPLIT = "=" * 7

# What counts as "in the repository" is asked of git rather than described here.
#
# The first version of this carried its own skip list — node_modules, build, dist, .claude — and
# it was wrong in both directions within an hour. It missed half of what .gitignore already knew
# about (.venv, datasets, captures, playwright-report, __pycache__), so it walked them for
# nothing; and it pruned the whole of `.claude`, of which `.claude/skills/` is *tracked* content,
# so a marker in the engineering skill would have gone unnoticed. A hand-written list beside an
# existing one drifts, and this one drifted before it was ever merged.
#
# `tools/tracked.py` answers that question now, for this checker and two others, and carries the
# rest of this paragraph with it.
#
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

    The head and tail markers are followed by a branch name, so the space is part of the shape and
    requiring it costs nothing — while a bare prefix would trip on the arrows these documents use
    to draw the layer diagram.

    The base marker is the one `merge.conflictStyle = diff3` or `zdiff3` adds, naming the common
    ancestor. Checked against a real three-way merge in each of the three styles rather than
    assumed: git writes seven pipes, a space, then a description. The bare form is accepted too,
    since unlike the arrows there is no prose here that seven pipes could be — a markdown table
    rule carries dashes between its pipes.

    The split marker stands alone, which makes it the one that could collide with prose: a setext
    heading underlined with exactly seven `=` would match. No such line exists in this repository,
    and a heading that needs exactly seven can have eight, so the ambiguity is worth taking to
    catch a resolution that left only the middle.
    """
    if text.startswith(OURS + " ") or text.startswith(THEIRS + " "):
        return True
    if text.startswith(BASE + " ") or text.rstrip() == BASE:
        return True
    return text.rstrip() == SPLIT


def check(root: Path) -> list[Marker]:
    root = Path(root)
    found: list[Marker] = []

    for rel in tracked_files(root):
        path = root / rel
        # Not every listed path is a file to read: a submodule is a gitlink, and a tracked file
        # deleted from the working tree is still in the index. Neither has content to scan.
        # **Inside the `try`, like every other `is_file()` this project has been caught by.**
        # `Path.stat` swallows `ENOENT`, `ENOTDIR`, `EBADF` and `ELOOP` and nothing else, so a
        # tracked symlink whose target is a 300-character name raises `ENAMETOOLONG` out of here —
        # a traceback from the one call this loop makes before it is ready to report anything. The
        # provenance checker guards the identical call in three places; these two did not, and the
        # `/proc/kmsg` case that tests the promise walks straight past it because `is_file()`
        # succeeds for that file.
        try:
            if not path.is_file():
                continue
            # **The ceiling is measured, not read from `stat`.** `st_size > MAX_BYTES` looks
            # equivalent and is a second answer to the same question — and it is wrong for exactly
            # the files that need bounding: a `/proc` file reports zero, so the ceiling let through
            # the one input it existed for and `read_text` then blocked in the kernel forever.
            body = reading.text(path, MAX_BYTES)
            if body is None:
                continue
        except OSError as failure:
            # Reported, not skipped. A tracked file this cannot read is a file it cannot clear,
            # and swallowing that would make an unreadable tree look like a clean one — which is
            # the same false pass the git failure above refuses to give.
            raise RuntimeError(f"{rel} could not be read: {failure}") from failure
        for number, text in enumerate(body.splitlines(), start=1):
            if is_marker(text):
                found.append(Marker(rel, number, text))
    return found


def main(argv: list[str]) -> int:
    root = Path(argv[1]) if len(argv) > 1 else Path(__file__).resolve().parent.parent
    # **A refusal to run is a sentence, not a traceback.** `tracked.py` raises when git will not
    # answer, and these checkers raise when a tracked file cannot be read — both deliberately, since
    # returning "nothing found" from a check that could not run is the one false pass they exist to
    # prevent. What was not deliberate is that the message then arrived as a `RuntimeError` under
    # four frames of checker source, with git's own remedy buried at the bottom. The commonest
    # trigger needs no hostile input at all: git refuses a repository whose checkout and whose
    # caller are different users, which is an ordinary container shape.
    try:
        markers = check(root)
    except RuntimeError as refused:
        print(f"this check could not run: {refused}", file=sys.stderr)
        return 1
    if not markers:
        return 0
    print(f"{len(markers)} conflict marker(s) left behind:\n", file=sys.stderr)
    for marker in markers:
        print(f"  {marker}", file=sys.stderr)
    print("\nA merge was resolved halfway. Finish it before committing.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
