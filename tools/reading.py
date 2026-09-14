#!/usr/bin/env python3
"""How to read a file this repository did not write, asked once.

`tracked.py` answers which files are in the repository. This answers the question immediately after
it, and it is the same argument one step along: three checkers open the paths that listing returns,
and until this module existed each opened them its own way. Two used `path.open("rb")`, one used
`read_text()`, one bounded the read by `stat().st_size` and two did not bound it at all — four
answers to "how do I read a file somebody else put here", drifting independently.

The cost of that was not hypothetical. `asset_provenance` hardened its opener against a FIFO and a
`/proc` file and applied it to **two of its five** open sites, so a single committed
`ln -s /proc/kmsg LICENSE` was correctly refused by the licence reader and then hung the very same
run in `why_asset` — the fix and the defect, in one process, on one file. The two sibling checkers
hung on it too.

Three promises, and they are the checkers' promises rather than this module's:

**It never hangs.** `open()` on a FIFO blocks in the kernel until a writer appears, so the guard
has to be on the `open` and not on the read.

**It never answers about something that is not a regular file**, and it asks the descriptor rather
than the path, so there is no window between the question and the answer.

**It never returns something that is not bytes.** The `O_NONBLOCK` that stops the hang gives a read
a third outcome, `None`, which is neither bytes nor end of file.

What it deliberately does not do is decide policy. Whether an unreadable file is a problem, and what
to say about it, is each checker's business; this raises `OSError` and lets them answer.
"""
from __future__ import annotations

import errno
import os
import stat
from pathlib import Path
from typing import Iterator

# A megabyte. Large enough that a real file is one or two reads, small enough that the peak cost of
# walking an arbitrary tree is a property of this constant rather than of the largest file in it.
BLOCK = 1024 * 1024


def open_regular(path: Path):
    """Open `path` for reading, refusing anything that is not a regular file.

    **`O_NONBLOCK`, because `open()` itself can hang.** On a FIFO it blocks in the kernel until a
    writer appears, and no ceiling on the *read* helps: a single `mkfifo LICENSE` made a checker
    never return. Every path these checkers open comes from the git index, which says what was
    committed and nothing whatever about what is on disk now.

    `fstat` on the descriptor rather than `is_file()` on the path, so there is no window between the
    question and the answer: what is opened is what is checked.
    """
    handle = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    try:
        if not stat.S_ISREG(os.fstat(handle).st_mode):
            raise OSError(errno.EINVAL, "is not a regular file")
        return os.fdopen(handle, "rb")
    except BaseException:
        # Every way out of this window but the happy one is an exception, and the descriptor is a
        # bare integer with nothing owning it — so no finaliser runs and no `ResourceWarning` is
        # raised. A leak here is invisible until a big enough tree runs the process out of them.
        os.close(handle)
        raise


def ready(head: bytes | None) -> bytes:
    """The bytes a read returned, refusing the one answer that is not bytes.

    `open_regular` opens with `O_NONBLOCK` and the flag stays on the descriptor, so a read here has
    a third outcome besides bytes and end of file: `None`, meaning "a regular file with nothing
    ready". The flag cannot be cleared to make that go away — the files it guards against are
    exactly the ones whose *read* would then block forever. `/proc/kmsg` is a regular file by
    `S_ISREG` and reading it waits for the next kernel message, so `os.set_blocking` reinstates the
    hang the flag was added to prevent, one call later and in a place nothing tests.

    Unhandled it was `AttributeError: 'NoneType' object has no attribute 'decode'` and
    `TypeError: object of type 'NoneType' has no len()` — neither an `OSError`, so neither caught by
    the arms that exist for a file that cannot be read, and both arriving as a traceback.

    `EAGAIN`, which is the errno the flag's own contract names for it, so a caller reporting
    `strerror` says something true about what happened.
    """
    if head is None:
        raise OSError(errno.EAGAIN, "had nothing ready to read")
    return head


def blocks(path: Path) -> Iterator[bytes]:
    """The file's bytes, a `BLOCK` at a time, for a consumer that can work incrementally.

    The only loop of its kind left in this repository, which is the point: a digest that hashes the
    first block and stops, and a decoder that restarts per block, are both invisible in a suite
    whose fixtures are smaller than one block. There is one place to drive past a boundary now
    rather than one per consumer.

    A generator rather than a callback, so the consumer keeps its own state — `hashlib` and an
    incremental UTF-8 decoder want opposite things from a chunk and neither wants to be inverted.
    """
    with open_regular(path) as handle:
        while chunk := ready(handle.read(BLOCK)):
            yield chunk


def head(path: Path, limit: int) -> bytes:
    """At most `limit` bytes from the start of the file.

    For the questions a stream cannot answer: `json.loads` needs a whole document, and "is there
    anything legible in this licence" is settled by the first block of any licence somebody wrote.

    **Asked of the file, not of `stat`.** Bounding a read by `st_size` looks equivalent and is a
    second answer to the same question, wrong for exactly the files this module exists for: a
    `/proc` file reports zero and a character device reports zero, so a ceiling written that way
    lets through precisely the input that needs it. A caller that wants to know whether the file is
    longer than its ceiling passes `ceiling + 1` and measures what it got.
    """
    with open_regular(path) as handle:
        return ready(handle.read(limit))


def text(path: Path, limit: int) -> str | None:
    """The file's text, or None if it is longer than `limit` bytes.

    The shape both line-oriented checkers want. None rather than a refusal because for those two a
    file too large to scan is a file they have nothing to say about — neither a conflict marker nor
    a markdown table lives in a 400 MB blob — and that is a different answer from "this could not be
    read", which is still an `OSError` and still theirs to report.

    `errors="replace"`, because these checkers are looking for ASCII punctuation in whatever the
    file happens to be, and a `UnicodeDecodeError` on an unrelated file is a build failure that
    names the wrong problem.
    """
    found = head(path, limit + 1)
    if len(found) > limit:
        return None
    return found.decode("utf-8", errors="replace")
