#!/usr/bin/env python3
"""Which files are in this repository, asked once.

Three checkers need the same answer — the conflict-marker check, the markdown-table check and the
asset-provenance check — and until this module existed each asked git itself, with the same tuple,
the same call and the same rule about failure. Nothing was wrong with any of them. What is wrong
with three copies is that the next change to the question lands in one file: a `--recurse-submodules`,
a decision to stop scanning `--others`, a dedup rule for merge stages. The other two keep answering
the old question, and two checkers disagreeing about which files are in the repository shows up only
as one of them quietly missing something.

There turned out to be two questions, not one, and collapsing them is its own bug. "What must I
check" wants the file you have written and not yet added; "what will somebody else's checkout have"
must not count it. The provenance checker asked the first while meaning the second and accepted an
untracked LICENSE as this repository's licence — green locally, red on a clean checkout. Both are
here, named for what they answer, so a call site has to choose.

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

# The narrower question, and a different one. `--cached` alone is what a fresh clone gets: the index.
# Without `--others` a file nobody has added is simply not there.
LS_FILES_INDEXED = ("git", "ls-files", "-z", "--cached")


def ask(root: Path, question: tuple[str, ...]) -> list[str]:
    """Run one `git ls-files` and return its paths, relative to `root`.

    A failure to ask is raised rather than swallowed. Returning "no files" from a git that would not
    answer makes an unrunnable check indistinguishable from a clean tree, which is the shape of bug
    every caller of this exists to catch.
    """
    result = subprocess.run(question, cwd=root, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"git could not list this tree: {result.stderr.strip()}")
    # Deduplicated, because `--cached` lists a path once per stage while a merge is unresolved —
    # base, ours, theirs. That is exactly when the marker check runs, so without it every marker in
    # a conflicted file is reported three times, in the output somebody is reading to find them.
    return sorted({name for name in result.stdout.split("\0") if name})


def tracked_files(root: Path) -> list[str]:
    """Every path git would let you commit, relative to `root`.

    The right question for "what must this checker look at": a file you have written and not yet
    added is a file you are about to commit, and a checker that waits for `git add` to notice it
    tells you about it after the push.
    """
    return ask(root, LS_FILES)


def indexed_files(root: Path) -> list[str]:
    """Every path in the index, relative to `root` — what somebody else's checkout would contain.

    The other question, for a check whose subject is *their* tree rather than yours: does this
    repository have a LICENSE, does that generated file exist for the next person. Answering those
    from `tracked_files` reads an untracked file as present, which is green on the machine that
    wrote it and red on the machine that checks it out — the worst place for a checker to disagree
    with itself, because the disagreement arrives as CI failing on records nobody touched.

    Both are honest answers to "is this file in the repository". They differ on exactly the files
    the asker cares about, which is why each call site says which it means.
    """
    return ask(root, LS_FILES_INDEXED)
