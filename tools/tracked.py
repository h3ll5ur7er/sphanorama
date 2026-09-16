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
#
# No `--exclude-standard`, and that is not an omission: it applies to the *untracked* half, so with
# no `--others` there is nothing for it to exclude. A gitignored file that somebody added anyway is
# in the index and will be in the clone, which is what this question asks. A test that fed a
# `.gitignore` to this listing was therefore asserting nothing, and it was written believing the
# opposite.
LS_FILES_INDEXED = ("git", "ls-files", "-z", "--cached")


def ask(root: Path, question: tuple[str, ...]) -> list[str]:
    """Run one `git ls-files` and return its paths, relative to `root`.

    A failure to ask is raised rather than swallowed. Returning "no files" from a git that would not
    answer makes an unrunnable check indistinguishable from a clean tree, which is the shape of bug
    every caller of this exists to catch.
    """
    # **Bytes, not `text=True`.** A path is bytes on this platform and only conventionally UTF-8,
    # and `text=True` did two separate kinds of damage to one that is not:
    #
    # - a name that is not valid UTF-8 — `café.md` written by an editor in Latin-1 — raised
    #   `UnicodeDecodeError` out of `subprocess._translate_newlines`, killing all three checkers
    #   with a traceback naming a line of the standard library;
    # - universal-newline translation rewrote a `\r` *inside* a name to `\n`, which is worse than
    #   a crash because it is silent: `-z` asks git for NUL-separated paths precisely so a newline
    #   in one cannot be mistaken for a separator, and then the decoder put one there. A committed,
    #   unrecorded 179 KB JPEG named `stol\renn.jpg` came back as `stol\nenn.jpg`, a path that does
    #   not exist, so the provenance walk could not find the file and said nothing. **Exit 0 on an
    #   unrecorded binary**, which is the one failure that checker exists to prevent.
    #
    # `surrogateescape` rather than a decode that can fail or substitute: it round-trips undecodable
    # bytes back through `os.fsencode`, so `Path(name).open()` reaches the file git named.
    result = subprocess.run(question, cwd=root, capture_output=True)
    if result.returncode != 0:
        stderr = result.stderr.decode("utf-8", "surrogateescape").strip()
        raise RuntimeError(f"git could not list this tree: {stderr}")
    # Deduplicated, because `--cached` lists a path once per stage while a merge is unresolved —
    # base, ours, theirs. That is exactly when the marker check runs, so without it every marker in
    # a conflicted file is reported three times, in the output somebody is reading to find them.
    return sorted({name.decode("utf-8", "surrogateescape")
                   for name in result.stdout.split(b"\0") if name})


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
