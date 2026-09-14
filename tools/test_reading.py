#!/usr/bin/env python3
"""Tests for the shared file reader.

Three checkers open the paths `tracked.py` lists, and the failures this module exists to prevent are
all of the same kind: a checker that does not come back, or comes back with a traceback, about a
file somebody committed. None of those is visible in a suite whose fixtures are ordinary files, so
every case here is about a file that is not ordinary.

The alarm is the assertion in the hang cases. A hang has no other symptom, and a test that waits for
one is the only kind that can fail on it.
"""
from __future__ import annotations

import contextlib
import errno
import hashlib
import io
import os
import signal
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import reading  # noqa: E402

# A regular file by `S_ISREG` whose read blocks until the kernel has something to say. This is the
# input the whole module is shaped around, and it is named rather than read: reading `/proc/kmsg`
# *drains the kernel ring buffer*, which a test has no business doing to the machine it runs on.
BLOCKING_REGULAR_FILE = Path("/proc/kmsg")

# And a regular file that answers forever: `S_ISREG`, `st_size` 0, no end. Unlike
# `/proc/kmsg` this one is safe to read — it reports this process's own page table and
# reading it consumes nothing.
ENDLESS_REGULAR_FILE = Path("/proc/self/pagemap")


class Impatient(unittest.TestCase):
    """A base that fails rather than waits."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        # Put back, because `signal.signal` is process-wide: `alarm(0)` cancels the timer and leaves
        # the handler installed, so a later test anywhere in the process would fail with this
        # file's message about a problem it does not have.
        self.addCleanup(signal.signal, signal.SIGALRM, signal.getsignal(signal.SIGALRM))

    def impatiently(self, seconds, call, *arguments):
        def complain(number, frame):
            raise AssertionError(f"did not return within {seconds}s: a read is blocking")

        signal.signal(signal.SIGALRM, complain)
        signal.alarm(seconds)
        try:
            return call(*arguments)
        finally:
            signal.alarm(0)


class OpeningSomethingThatIsNotAnOrdinaryFile(Impatient):
    def test_a_fifo_is_refused_rather_than_waited_on(self):
        # The `open` and not the read: a FIFO blocks in the kernel until a writer appears, so a
        # ceiling on the read is defeated by the call before the one it bounds.
        fifo = self.root / "pipe"
        os.mkfifo(fifo)
        with self.assertRaises(OSError):
            self.impatiently(10, reading.open_regular, fifo)

    def test_a_directory_is_refused(self):
        with self.assertRaises(OSError):
            reading.open_regular(self.root)

    def test_a_refused_open_leaves_no_descriptor_behind(self):
        # `open_regular` holds a bare descriptor between `os.open` and `os.fdopen`, and every way
        # out of that window but the happy one is an exception. A bare descriptor has nothing owning
        # it to have a finaliser, so no `ResourceWarning` is ever raised — there is no signal to
        # catch and the count is the only evidence.
        fifo = self.root / "pipe"
        os.mkfifo(fifo)
        before = len(os.listdir("/proc/self/fd"))
        # Under the alarm, because without `O_NONBLOCK` this does not fail — it *hangs*, on the
        # first of the 64 opens, and CI reports a job timeout with no red test. A reviewer measured
        # exactly that on the version of this case that lived in the provenance suite: `timeout 40`,
        # exit 124, nothing named. A leak test whose sabotage is a hang is a leak test that cannot
        # report the thing next to what it is looking at.
        for _ in range(64):
            with self.assertRaises(OSError):
                self.impatiently(10, reading.open_regular, fifo)
        self.assertLessEqual(len(os.listdir("/proc/self/fd")), before,
                             "a refused open left its descriptor behind")

    def test_an_ordinary_file_is_opened_and_read(self):
        # The case that proves the three above are refusing something specific rather than
        # everything, which is the failure mode a guard like this actually has.
        path = self.root / "ordinary"
        path.write_bytes(b"hello")
        with reading.open_regular(path) as handle:
            self.assertEqual(handle.read(64), b"hello")


class AFileWithNothingReadyToRead(Impatient):
    """The third outcome `O_NONBLOCK` buys, which is neither bytes nor end of file."""

    def test_a_none_read_is_a_refusal(self):
        with self.assertRaises(OSError) as refused:
            reading.ready(None)
        self.assertEqual(refused.exception.errno, errno.EAGAIN)

    def test_bytes_pass_through_unchanged(self):
        self.assertEqual(reading.ready(b""), b"")
        self.assertEqual(reading.ready(b"abc"), b"abc")

    @unittest.skipUnless(BLOCKING_REGULAR_FILE.exists(),
                         f"{BLOCKING_REGULAR_FILE} is not on this machine")
    def test_the_flag_that_makes_none_possible_is_really_on_the_descriptor(self):
        # The premise, measured rather than asserted from the source: `S_ISREG` is true of a file
        # whose read blocks, so the type check cannot be what saves this — and it is the `open` flag
        # that turns that block into a `None`. Without this, every case above is a test of a
        # situation that might not arise.
        import fcntl
        path = self.root / "ordinary"
        path.write_bytes(b"hello")
        with reading.open_regular(path) as handle:
            self.assertTrue(fcntl.fcntl(handle.fileno(), fcntl.F_GETFL) & os.O_NONBLOCK)
        self.assertTrue(BLOCKING_REGULAR_FILE.is_file(),
                        "the input this module is shaped around is a regular file by is_file()")


class ReadingInBlocks(Impatient):
    """The one block loop left in this repository, driven past its first block.

    `BLOCK` is a megabyte and nothing any suite writes is a megabyte, so before the loop was one
    loop it ran exactly once in every test everywhere — and a digest that hashes the first block and
    stops, a decoder that restarts per block, and a decoder never flushed were each invisible.
    """

    def test_every_block_is_yielded_and_the_bytes_are_the_file(self):
        held = reading.BLOCK
        reading.BLOCK = 8
        self.addCleanup(setattr, reading, "BLOCK", held)
        for size in (0, 7, 8, 9, 25):
            with self.subTest(size=size):
                path = self.root / f"f{size}"
                content = os.urandom(size)
                path.write_bytes(content)
                self.assertEqual(b"".join(reading.blocks(path)), content)

    def test_a_block_is_at_most_block_bytes(self):
        held = reading.BLOCK
        reading.BLOCK = 8
        self.addCleanup(setattr, reading, "BLOCK", held)
        path = self.root / "f"
        path.write_bytes(os.urandom(30))
        self.assertEqual([len(chunk) for chunk in reading.blocks(path)], [8, 8, 8, 6])

    def test_a_digest_over_the_blocks_equals_a_digest_over_the_file(self):
        # The consumer-shaped assertion, because "the bytes come back" and "an incremental consumer
        # sees them all" are different claims and it is the second one that has been wrong here.
        held = reading.BLOCK
        reading.BLOCK = 8
        self.addCleanup(setattr, reading, "BLOCK", held)
        path = self.root / "f"
        content = os.urandom(100)
        path.write_bytes(content)
        running = hashlib.sha256()
        for chunk in reading.blocks(path):
            running.update(chunk)
        self.assertEqual(running.hexdigest(), hashlib.sha256(content).hexdigest())

    def test_a_fifo_is_refused_rather_than_waited_on(self):
        fifo = self.root / "pipe"
        os.mkfifo(fifo)
        with self.assertRaises(OSError):
            self.impatiently(10, lambda: list(reading.blocks(fifo)))


class AStreamThatStopsWithoutEnding(Impatient):
    """A read that returns *some* bytes and then is not ready — the outcome `ready` cannot see.

    `ready` catches the case where a non-blocking read answers `None` having produced nothing. It
    structurally cannot catch the other one: `BufferedReader.read(n)` on a non-blocking raw returns
    what it has when the raw stalls part-way, and a short return is indistinguishable from end of
    file to a caller that only counts bytes.

    That is a false pass rather than an error, which is why it matters more than the `None`. A
    conflict marker truncated from `<<<<<<< HEAD` to `<<<<<` stops matching `is_marker`, so the
    checker reports a clean tree about a file it only partly read — the one answer it exists to
    prevent. `blocks` was already safe, because its next read raises; `head` was not.
    """

    class Stalls(io.RawIOBase):
        """Five bytes, then EAGAIN. Not end of file — just nothing more ready."""

        def __init__(self, given=b"<<<<<"):
            self.given = given

        def readable(self):
            return True

        def readinto(self, buffer):
            if not self.given:
                return None
            count = len(self.given)
            buffer[:count] = self.given
            self.given = b""
            return count

    def stalling(self, *arguments):
        held = reading.open_regular
        self.addCleanup(setattr, reading, "open_regular", held)
        reading.open_regular = lambda path: io.BufferedReader(self.Stalls(*arguments))

    def test_a_short_read_that_is_not_end_of_file_is_a_refusal(self):
        self.stalling()
        with self.assertRaises(OSError) as refused:
            reading.head(self.root / "anything", 64)
        self.assertEqual(refused.exception.errno, errno.EAGAIN)

    def test_text_does_not_hand_back_a_truncated_file_as_a_whole_one(self):
        # The consequence, in the units of the checker that suffers it. Before, this returned
        # `'<<<<<'` — five bytes of a file the reader had not finished — and both line checkers
        # would have scanned it and found nothing.
        self.stalling()
        with self.assertRaises(OSError):
            reading.text(self.root / "anything", 64)

    def test_a_genuinely_short_file_is_still_short(self):
        # The other side, and the one that says this is not simply "refuse anything under the
        # limit". A 10-byte file asked for 64 bytes is complete at 10, and `read` returning fewer
        # than asked is the *normal* case at end of file.
        path = self.root / "small"
        path.write_bytes(b"0123456789")
        self.assertEqual(reading.head(path, 64), b"0123456789")
        self.assertEqual(reading.text(path, 64), "0123456789")


class AFileThatNeverEnds(Impatient):
    """A regular file with no end, which "it never hangs" did not cover.

    `open_regular` guards the `open` and `ready` guards a stalled read. Neither bounds a file that
    keeps answering: `/proc/self/pagemap` is `S_ISREG`, reports `st_size` 0, opens instantly and
    yielded **7.7 GB in three seconds with no EOF**. The module promised never to hang and did not
    keep it — what kept it in practice were two guards in a different file, one refusing symlinks
    inside a record's directory and one happening to hit invalid UTF-8 in the first block. A
    promise kept by somebody else's coincidence is the shape this project keeps being caught by.

    So the stream has a ceiling. Generous rather than tight: the largest committed asset here is
    179 KB, an asset is allowed to be a panorama, and the number only has to be below "forever".
    """

    def test_a_stream_with_no_end_is_refused_rather_than_followed(self):
        held = reading.STREAM_CEILING
        reading.STREAM_CEILING = 4096
        self.addCleanup(setattr, reading, "STREAM_CEILING", held)
        path = self.root / "big"
        path.write_bytes(b"x" * (reading.STREAM_CEILING + 1))
        with self.assertRaises(OSError) as refused:
            list(reading.blocks(path))
        self.assertIn("larger than", refused.exception.strerror)

    def test_a_file_exactly_at_the_ceiling_is_read(self):
        # The boundary, because a ceiling asserted only from above is satisfied by a reader that
        # refuses everything — and `digest` is the caller, so refusing a legitimate asset would
        # fail the build on the file it is meant to account for.
        held = reading.STREAM_CEILING
        reading.STREAM_CEILING = 4096
        self.addCleanup(setattr, reading, "STREAM_CEILING", held)
        path = self.root / "exact"
        path.write_bytes(b"x" * reading.STREAM_CEILING)
        self.assertEqual(len(b"".join(reading.blocks(path))), reading.STREAM_CEILING)

    @unittest.skipUnless(ENDLESS_REGULAR_FILE.exists(),
                         f"{ENDLESS_REGULAR_FILE} is not on this machine")
    def test_the_real_endless_file_terminates(self):
        # The fixture above is a large file, which is not the same thing. This is a file that does
        # not end, and the only way to tell the two apart is to read one.
        held = reading.STREAM_CEILING
        reading.STREAM_CEILING = 1024 * 1024
        self.addCleanup(setattr, reading, "STREAM_CEILING", held)
        with self.assertRaises(OSError):
            self.impatiently(20, lambda: list(reading.blocks(ENDLESS_REGULAR_FILE)))


class ReadingAHead(Impatient):
    def test_at_most_the_limit_is_returned(self):
        path = self.root / "f"
        path.write_bytes(b"0123456789")
        self.assertEqual(reading.head(path, 4), b"0123")
        self.assertEqual(reading.head(path, 10), b"0123456789")
        self.assertEqual(reading.head(path, 99), b"0123456789")

    def test_a_fifo_is_refused_rather_than_waited_on(self):
        fifo = self.root / "pipe"
        os.mkfifo(fifo)
        with self.assertRaises(OSError):
            self.impatiently(10, reading.head, fifo, 64)


class ReadingText(Impatient):
    def test_a_file_within_the_limit_is_its_text(self):
        path = self.root / "f"
        path.write_text("# A title\n\nbody\n")
        self.assertEqual(reading.text(path, 1024), "# A title\n\nbody\n")

    def test_a_file_exactly_at_the_limit_is_still_read(self):
        # The off-by-one that decides whether a ceiling is a ceiling. `head` is asked for one byte
        # past it precisely so this boundary is answered by measurement rather than by `stat`.
        path = self.root / "f"
        path.write_bytes(b"x" * 64)
        self.assertEqual(reading.text(path, 64), "x" * 64)
        self.assertIsNone(reading.text(path, 63))

    def test_bytes_that_are_not_utf8_are_replaced_rather_than_raised(self):
        # These two checkers look for ASCII punctuation in whatever the file happens to be. A
        # `UnicodeDecodeError` here would fail the build naming a file whose encoding is nobody's
        # business.
        path = self.root / "f"
        path.write_bytes(b"caf\xe9 <<<<<<< HEAD\n")
        self.assertIn("<<<<<<< HEAD", reading.text(path, 1024))

    def test_a_file_past_the_limit_is_none_rather_than_held(self):
        path = self.root / "f"
        path.write_bytes(b"x" * 200)
        self.assertIsNone(reading.text(path, 64))

    def test_the_limit_is_not_read_from_stat(self):
        # The sibling checker bounded its read by `st_size`, which looks equivalent and is a second
        # answer to the same question — and it is wrong for exactly the files that need bounding.
        # A `/proc` file reports zero. Rather than depend on `/proc`, this drives the equivalence
        # directly: a file whose `st_size` says nothing useful is still bounded by what was read.
        path = self.root / "f"
        path.write_bytes(b"x" * 200)
        held = os.stat

        def lies(*arguments, **named):
            answer = held(*arguments, **named)
            return os.stat_result((answer.st_mode, answer.st_ino, answer.st_dev, answer.st_nlink,
                                   answer.st_uid, answer.st_gid, 0,
                                   answer.st_atime, answer.st_mtime, answer.st_ctime))

        os.stat = lies
        self.addCleanup(setattr, os, "stat", held)
        self.assertIsNone(reading.text(path, 64), "the ceiling believed stat over the bytes")


class EveryCheckerThatWalksThisRepository(Impatient):
    """The property all three share, asserted where a reader will look for it.

    A tracked symlink to a blocking regular file is the input that got past `asset_provenance`'s own
    hardening, because that hardening reached two of its five open sites. These cases are here
    rather than in each checker's own suite because the promise is the same promise, and three
    copies of it is the shape this module exists to end.
    """

    CHECKERS = ("asset_provenance", "conflict_marker_check", "markdown_table_check")

    def tree(self, build):
        subprocess.run(["git", "init", "-q"], cwd=self.root, check=True)
        (self.root / "LICENSE").write_text("MIT, for the purposes of this fixture.\n")
        tracked = ["LICENSE"] + build(self.root)
        subprocess.run(["git", "add", "-f", "--", *tracked], cwd=self.root, check=True)

    def each_checker_answers(self, seconds=20):
        for name in self.CHECKERS:
            with self.subTest(checker=name):
                checker = __import__(name)
                # Through `main`, because the promise is about what a person running the build
                # sees. Two of these three report an unreadable file by raising out of `check`,
                # which is deliberate — a silent skip would be a false pass — so `check` alone
                # cannot say whether the build got a sentence or a traceback. `main` can.
                #
                # **And the answer is checked, not merely counted.** `assertIsInstance(answer, int)`
                # was the whole of this, and a checker that walked nothing would satisfy it by
                # returning 0 — so the case passed for a file none of the three had opened, which
                # is precisely the failure the surrounding class is about. A non-zero exit and the
                # file's own name in what the build prints is what "it looked at this and had
                # something to say" looks like from outside.
                said = io.StringIO()
                with contextlib.redirect_stderr(said):
                    answer = self.impatiently(seconds, checker.main, ["checker", str(self.root)])
                self.assertEqual(answer, 1, said.getvalue())
                self.assertIn("notes.md", said.getvalue())

    def test_none_of_them_raises_on_a_name_this_filesystem_will_not_answer_about(self):
        # `is_file()` is the first thing all three do with a listed path, and `Path.stat` swallows
        # `ENOENT`, `ENOTDIR`, `EBADF` and `ELOOP` and nothing else. A tracked symlink to a
        # 300-character name gives `ENAMETOOLONG`, which is in no ignore set.
        #
        # This is a *different* input from the blocking file below and it has to be, which is the
        # whole finding: for `/proc/kmsg`, `is_file()` succeeds, so that case walks past the
        # unguarded call without touching it. One promise, two ways to break it, and a suite that
        # tested the promise with only the first went on being green.
        def build(root):
            (root / "notes.md").symlink_to("n" * 300)
            return ["notes.md"]

        self.tree(build)
        self.each_checker_answers()

    @unittest.skipUnless(BLOCKING_REGULAR_FILE.exists(),
                         f"{BLOCKING_REGULAR_FILE} is not on this machine")
    def test_none_of_them_hangs_on_a_tracked_symlink_to_a_blocking_file(self):
        def build(root):
            # `.md`, so the table checker has a reason to open it, and a name the marker checker
            # scans too. The symlink target is what matters, not the extension.
            (root / "notes.md").symlink_to(BLOCKING_REGULAR_FILE)
            return ["notes.md"]

        self.tree(build)
        self.each_checker_answers()


if __name__ == "__main__":
    unittest.main(verbosity=1)
