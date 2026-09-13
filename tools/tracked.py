#!/usr/bin/env python3
"""Which files are in this repository, asked once.

Three checkers need the same answer — the conflict-marker check, the markdown-table check and the
asset-provenance check — and until this module existed each asked git itself, with the same tuple,
the same call and the same rule about failure. Nothing was wrong with any of them. What is wrong
with three copies is that the next change to the question lands in one file: a `--recurse-submodules`,
a decision to stop scanning `--others`, a dedup rule for merge stages. The other two keep answering
the old question, and two checkers disagreeing about which files are in the repository shows up only
as one of them quietly missing something.

The same argument the provenance checker's own test suite already acts on, one level up: it calls
`asset_provenance.records` rather than re-deriving the listing, because a second listing is a second
answer.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

# Tracked files plus untracked ones git is not ignoring — exactly the set that can become a commit.
# It also keeps every scan out of node_modules and build/, which is what makes a checker's answer
# about this repository rather than about its dependencies.
LS_FILES = ("git", "ls-files", "-z", "--cached", "--others", "--exclude-standard")


def tracked_files(root: Path) -> list[str]:
    """Every path git would let you commit, relative to `root`.

    A failure to ask is raised rather than swallowed. Returning "no files" from a git that would not
    answer makes an unrunnable check indistinguishable from a clean tree, which is the shape of bug
    every caller of this exists to catch.
    """
    result = subprocess.run(LS_FILES, cwd=root, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"git could not list this tree: {result.stderr.strip()}")
    # Deduplicated, because `--cached` lists a path once per stage while a merge is unresolved —
    # base, ours, theirs. That is exactly when the marker check runs, so without it every marker in
    # a conflicted file is reported three times, in the output somebody is reading to find them.
    return sorted({name for name in result.stdout.split("\0") if name})
