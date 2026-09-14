#!/usr/bin/env python3
"""Fail the build if a file in the repository cannot say where it came from.

Source is self-describing and binaries are not. A `.jpg` that appears in a commit carries no author,
no licence and no origin, and six months later nobody can answer any of the three — at which point
the only safe move is to delete it and whatever was measured against it. This repository commits a
panorama because Phase 2's accuracy number has to be measured in a photographed world rather than a
checkerboard, so the answer is a record beside the bytes and a check that keeps the two together.

Two rules, and the second is the one that makes the first mean anything:

**Every tracked asset must have an entry, in the nearest `sources.json` above it.** Nearest, so a
record inside another one owns its own subtree rather than deadlocking with the outer record over the
same files. Only assets: a record in a directory does not make its neighbouring source files the
checker's business, which is why `shell/public/`'s record answers for an icon and says nothing about
the service worker beside it.

Something fetched from elsewhere goes under `assets` and names the work, its author, its licence and
where the bytes came from. Something this repository made goes under `ours` and still names an author
and a licence — what it is spared is the upstream trail, which for our own work is this repository.
Both carry a digest, so a file swapped later cannot inherit the clearance of the one it replaced.

**`ours` is deliberately not an escape hatch.** An earlier shape of it asked only for a command, so a
third-party file could be cleared by claiming this repository produced it — the licence question
skipped entirely. It cannot be skipped now: no check can stop somebody writing a false licence, and
that is a lie rather than a hole, but omission is what a checker can and should refuse.

Where an entry carries `produced_by`, that command is *executed*:
`tools/test_synth_dataset.py` runs it and compares the bytes, so a command that does not reproduce
the file it names fails the build rather than ageing quietly into fiction.

**A tracked file that is an asset has to be in one of those directories**, or nothing would ever ask
where it came from — the build step above this one claims every committed asset says so. Two ways to
be an asset: bytes that are not valid UTF-8, or a name whose extension is in `MEDIA` below. The
second exists because an SVG, an ASCII STL and a base64 `.gltf` are somebody's work and all three
read as text. Source is exempt, because a repository is mostly source and a rule that asked every
`.ts` file for a licence would be switched off within a week.

**A raster has to say what shape it is, and this cannot check that it is telling the truth.** So the
work is split: an entry whose extension is in `SHAPED` must carry `width` and `height` or the build
fails here, and `tools/test_synth_dataset.py` opens the file and compares them where Pillow is
already present. Deciding the shape here would put an image library in front of every build, which
is what this refuses to be — but leaving the field *optional* meant the one fact needing a decoder
could be deleted from a record with nothing going red, which is what it did.

**A field a program reads is a token, not a sentence.** `projection` was the first: an entry
recording it must use a value from `PROJECTIONS`, because `tools/test_synth_dataset.py` asserts the
2:1 rule on an entry claiming to be equirectangular — and while one record spelled that field as a
description, the assertion keyed on it was dead for every record in the tree with nothing able to
notice. `READ_BY_A_PROGRAM` is the list of them, and each is held to a single string; a field that
joins that list without a case in the suite is the defect this has already produced twice.
Everything else a record holds is prose, may be written as a list of lines, and is judged only for
being an answer at all.

Usage:  uv run tools/asset_provenance.py [repo_root]
"""
from __future__ import annotations

import codecs
import hashlib
import json
import sys
import unicodedata
from dataclasses import dataclass
from pathlib import Path

from reading import BLOCK, blocks, head
from tracked import indexed_files, tracked_files

RECORD = "sources.json"


# Each answers a question that cannot be recovered from the bytes. `sha256` can be, and is here so
# that the record is checkable against the file rather than merely present — `bytes` is a second
# copy of a fact the digest already pins, kept because a reader can compare it without hashing
# anything, not because it adds reach. The one failure it catches alone is a typo in the record.
REQUIRED = ("file", "sha256", "bytes", "work", "author", "licence", "licence_url",
            "source_repository", "source_path", "retrieved")

# Our own work still has an author and a licence; what it does not have is somewhere else it came
# from. `produced_by` is optional because some of it is written by hand rather than rendered, and it
# is checked where it appears.
REQUIRED_OURS = ("file", "sha256", "bytes", "author", "licence")

# Extensions that make a file an asset whatever its bytes decode as. Not a guess at "binary": these
# are the formats whose content is somebody's work rather than somebody's source, and several of
# them are text.
#
# **What this does and does not reach**, since a reviewer asked the question and the answer is not
# obvious from the tuple. A file whose *bytes* are not valid UTF-8 is an asset whatever it is
# called — `why_asset` decodes it — so a PNG committed as `frame.dat` still has to be recorded.
# What it escapes is the *shape* rule: `SHAPED` keys on the extension, so nothing asks `frame.dat`
# for `width` and `height`, and `tools/test_synth_dataset.py` skips it by the same test, so both
# halves of the split go quiet together. That is a real gap and it is bounded: the file cannot go
# unrecorded, only under-described. Closing it means sniffing content to decide the rule, which is
# the decoder-dependence `SHAPED` is derived from `MEDIA` to avoid; if a raster is ever committed
# under a name like that, rename it rather than teaching this to guess.
MEDIA = (".jpg", ".jpeg", ".png", ".gif", ".webp", ".avif", ".bmp", ".ico", ".tiff", ".tif", ".svg",
         ".ppm", ".pgm", ".pnm", ".hdr", ".exr", ".mp3", ".wav", ".ogg", ".flac", ".mp4", ".webm",
         ".mov", ".ttf", ".otf", ".woff", ".woff2", ".pdf", ".stl", ".obj", ".glb", ".gltf",
         ".blend", ".psd", ".zip")

# Extensions whose files have a pixel shape a decoder could confirm. An entry for one of these has
# to record `width` and `height`: this checker cannot verify them — that needs an image library in
# front of every build, which it refuses to be — but `tools/test_synth_dataset.py` does, where
# Pillow is already present, and a fact that is optional here is a fact that can be deleted there
# with nothing going red. Split by extension rather than by content for the same reason: asking the
# bytes is asking a decoder.
#
# Not every asset: an SVG is somebody's work and has no shape a raster decoder can confirm, and
# neither has an `.mp3`.
#
# Derived from `MEDIA` rather than listed beside it, because a second list of extensions drifts from
# the first and did: `.avif`, `.ico`, `.hdr` and `.exr` were rasters in one tuple and not the other,
# so a 3x2 PNG named `.avif` and recorded as 4096x7 cleared the build. Naming the exceptions instead
# means a new extension is shaped unless somebody says why it is not, which is the way round that
# fails safe.
UNSHAPED = (".svg", ".mp3", ".wav", ".ogg", ".flac", ".mp4", ".webm", ".mov", ".ttf", ".otf",
            ".woff", ".woff2", ".pdf", ".stl", ".obj", ".glb", ".gltf", ".blend", ".psd", ".zip")
SHAPED = tuple(suffix for suffix in MEDIA if suffix not in UNSHAPED)

# `projection` is read by a test rather than by a person: `tools/test_synth_dataset.py` asserts the
# 2:1 rule on an entry that claims to be equirectangular. So it is a token from a closed set, and an
# unrecognised one is refused here — while this field said "equirectangular, 360 by 180 degrees" the
# assertion keyed on it was dead for every record in the tree, and nothing could see that. A
# description of the projection belongs in `notes`, which nothing branches on.
PROJECTIONS = ("equirectangular",)

# Fields this file *branches on* rather than merely stores — run as a command, compared against a
# constant, hashed against the bytes. `unusable` holds each to a single string, because the prose
# rule below accepts a list of lines and that is right for everything a person reads and wrong for
# everything a program reads.
#
# It began as one field. `produced_by` spelled as a list cleared the checker and died in its
# consumer with `TypeError: unhashable type: 'list'`, and the fix named that field alone — so
# `source_blob` as a list sailed past `isinstance(recorded_blob, str)` and was never compared, and
# `licence` as a list was never equal to the deferral sentinel, so a repository with no licence
# came back clean. The same defect, twice, inside the commit that fixed it. It is a tuple of every
# such field now, and the subtest table in the suite is driven from this tuple rather than from a
# hand-written list, so a field added here cannot be added without a case.
#
# `licence_url` joined when `check` began branching on it — our own work must name a licence *with
# somewhere to look it up* — and it is the fourth demonstration of the same thing: a field becomes
# read-by-a-program the moment a rule consults it, and the rule and the tuple are edited by
# different hands. Spelled as a list it satisfied the prose rule, so
# `{"licence": "Proprietary, all rights reserved", "licence_url": ["nonsense"]}` cleared the very
# rule that field was added to serve.
READ_BY_A_PROGRAM = ("file", "sha256", "licence", "licence_url", "projection", "produced_by",
                     "source_blob")

def git_blob(path: Path) -> str:
    """Git's object name for a file's bytes: `sha1("blob <length>\0" + bytes)`.

    A record may carry the upstream blob hash so a reader can find the exact object in the source
    repository's history. It was the one recorded fact nothing derived, which matters because it is
    the one that goes stale silently after the single thing ADR 0059 forbids: a transcode changes
    `sha256` and `bytes` and the build says so, and it changes this too and nothing said anything.
    """
    # Streamed, like `digest` above and for its reason: a checker that reads an asset whole into
    # memory is one `--panorama` away from being the thing it refuses to be.
    #
    # Git's header needs the length up front, and `stat` is the only way to have it before the
    # bytes — so this is the one place here that reads a size from the filesystem, and it is safe
    # only because `reading.blocks` has already established what the file is. A `stat` size used to
    # *bound* a read would be the mistake the sibling checker made; used to *describe* the bytes
    # that follow it is what git's own object format asks for.
    running = hashlib.sha1(b"blob %d\0" % path.stat().st_size)
    for block in blocks(path):
        running.update(block)
    return running.hexdigest()


# A licence that defers to this repository's own, rather than naming one. It is the right way to
# write our own work down — a derivation cannot drift the way a copy of "MIT" in nine records can —
# but it has to point at something, and for the whole life of these records it pointed at nothing.
# Deleting the licence file used to leave the checker and all of its tests green; it produces six
# refusals now, one per deferring record.
DEFERS_TO_THIS_REPOSITORY = "same as this repository"
# Both spellings, because this module calls the field `licence` and would otherwise have told a
# repository that spells its file the same way that it has none. The suite pins what is in this
# tuple and cannot pin what is missing from it, which is how the British spelling stayed out of a
# checker written in British English.
LICENCE_FILES = ("LICENSE", "LICENSE.md", "LICENSE.txt", "LICENCE", "LICENCE.md", "LICENCE.txt",
                 "COPYING")

# Characters that put nothing on the page and are neither whitespace nor category `Cf`: the four
# Hangul fillers (`Lo`) and the braille blank (`So`). Named one by one because there is no closed
# set of "renders blank" — that is as much a font question as a Unicode one, and a rule that chases
# it forever is worse than one that stops somewhere a reader can see. Unicode's
# `Default_Ignorable_Code_Point` covers the fillers and every `Cf` character; U+2800 is outside even
# that, which is why the stopping place is written down rather than derived.
# The categories whose members put something on the page: letters, numbers, punctuation, symbols.
# Asked as a whitelist because every blacklist tried here was defeated in a round — see `visible`.
VISIBLE_CATEGORIES = ("L", "N", "P", "S")

# The exceptions inside those categories: four Hangul fillers (`Lo`) and the braille blank (`So`),
# which are letters and symbols to Unicode and blank to a reader.
BLANK = ("\u115f", "\u1160", "\u3164", "\uffa0", "\u2800")

# A licence that gestures at this repository without being the token. This was the *guard* for one
# round, and a reviewer showed that a guard made of a substring is a guard made of one noun:
# `"same as the repository"`, `"same as this project"`, `"see LICENSE"`, `"same as the top-level
# LICENSE"` and `"this project's licence"` all mean the deferral to any reader, contain no
# `"this repo"`, and each cleared a tree with **no licence file at all**. Four widenings before it
# had every one been about characters — case, surrounding space, inner space, invisibles — so
# nothing had ever asked the rule about a different word, and there is no end to the ways English
# points at something.
#
# So it is a *diagnostic* now and the rule in `check` is the guard. This only decides whether a
# refusal can name the likely intent, which is worth having and is allowed to be incomplete: a
# phrase it misses is refused anyway, just less helpfully.
NEARLY_DEFERS = "this repo"


def visible(value: str) -> str:
    """`value` with the characters that put nothing on the page removed, whitespace excepted.

    One definition, for the two rules that need it. Each had been deriving its own a character
    class at a time: `legible` learnt about `Cf` and the deferral sentinel did not, so a field
    answered with a zero-width space was refused while a *sentinel* holding the same character
    quietly stopped being the sentinel — and a repository with no licence file came back clean.
    A second answer to "does this render as nothing" is a second answer.

    **A list of what is visible, because every list of what is invisible has been incomplete.**
    Three rules were tried here and each was defeated within a round: naming `Cf` left out `Cc`,
    the surrogates and the private-use area; `isprintable()` covers those and leaves out `Mn`, so
    one U+034F combining grapheme joiner answered every prose field of a real record. The set of
    characters that render as nothing is open, keeps growing with Unicode, and is not even a
    property of the text alone — it depends on the font.

    So the question is asked the other way round. A character counts as visible if it is a letter,
    a number, a punctuation mark or a symbol (`L*`, `N*`, `P*`, `S*`), which is a closed set that
    does not grow in kind, minus the handful in `BLANK` that belong to those categories and still
    render as nothing. Everything else — marks, separators, format characters, controls,
    surrogates, private use, unassigned — is not an answer on its own.

    The direction of failure is what makes this the right way round. An unknown codepoint is now
    *not* visible, so a field holding only unknowns is refused; under the old rule it was accepted.
    A checker written against one Unicode version therefore fails safe on text from a later one.

    Whitespace is left in, because the callers want it differently: `legible` strips it and the
    sentinel collapses runs of it to single spaces.
    """
    return "".join(character for character in value
                   if (character.isspace()
                       or (unicodedata.category(character)[0] in VISIBLE_CATEGORIES
                           and character not in BLANK)))


# What a `sources.json` is allowed to be. Generous — the largest in this tree is under 5 KB and the
# whole committed fixture is 25,593 bytes — and finite, which is the point.
RECORD_CEILING = 4 * 1024 * 1024


def read_record(path: Path) -> str:
    """A record's text, refusing one too large to be a record.

    The last unbounded read in this file. `why_asset` streams and says why, `digest` streams and
    says why, and the record — the one file here read for its *contents* rather than its bytes —
    was `read_text()` with nothing between it and the disk. A 400 MB `sources.json` took peak memory
    to 779 MiB against a 34 MiB control, and a tracked `sources.json` symlinked to `/dev/zero` never
    returned at all, which is a build that hangs rather than fails.

    Refused by length rather than streamed, because unlike an asset a record has no honest large
    form: `json.loads` needs the whole document anyway, and one this size is a mistake or an attack
    rather than a big record. `stat` first would be a second answer — and a wrong one for a
    character device, whose length is zero — so this reads one byte past the ceiling and asks.
    """
    found = head(path, RECORD_CEILING + 1)
    if len(found) > RECORD_CEILING:
        raise ValueError(f"is larger than {RECORD_CEILING} bytes, so it is not a record")
    return found.decode("utf-8")


def says_something(path: Path) -> bool:
    """Whether this file has anything legible in it, read without trusting its length.

    `st_size > 0` was the whole test, so a `LICENSE` of one newline — or three spaces, or a single
    zero-width space — answered a deferral. A licence that says nothing is the state a half-finished
    `touch` leaves behind, and it resolves a pointer to nothing just as a missing file does.

    Bounded, because this is asked about a path from the index and a checker must not be the thing
    that reads an arbitrary file whole. Anything legible is in the first block of a real licence.

    An `OSError` is the caller's to report: a licence that cannot be read is not the same as one
    that is not there, and saying "this repository has no licence file" about a file that is right
    there sends a reader to write one. `why_asset` made the opposite choice deliberately and it is
    the right one — the reason is what a refusal is for.
    """
    return legible(head(path, BLOCK).decode("utf-8", "surrogateescape"))


def licence_trouble(root: Path, indexed: list[str]) -> str | None:
    """Why this repository's own licence cannot answer a deferral, or None if it can.

    **Asked once per run, not once per entry.** It sat inside `any()` inside a loop over spellings
    inside the entry loop, so a tree with 200 deferring entries and a 1 MiB licence spent 32 seconds
    re-reading the same file, and 600 entries spent 98 — and `RECORD_CEILING` permits some 23,000
    entries in one record, so the bound added to stop unbounded work was also the multiplier. The
    answer cannot change while `check` runs.

    Every spelling is tried before any of them is reported, because the two questions this can
    answer — "there is no licence" and "the licence cannot be read" — are both about the set rather
    than about one file.
    """
    unreadable = None
    for spelling in LICENCE_FILES:
        if spelling not in indexed:
            continue
        try:
            if says_something(root / spelling):
                return None
        except OSError as refused:
            # **Remembered, not returned.** Hoisting this out of the entry loop quietly changed the
            # answer: the `any(...)` it replaced swallowed an `OSError` and went on to the next
            # spelling, and returning here stopped the search at the first unreadable one — so a
            # repository with a `LICENSE` that is a FIFO and a perfectly good tracked `COPYING` was
            # refused. A licence that cannot be read is a reason to report *if nothing else
            # answers*, never a reason to stop looking.
            unreadable = unreadable or f"{spelling} cannot be read: {refused.strerror}"
    if unreadable is not None:
        return unreadable
    return (f"this repository has no licence file for it to mean — looked for "
            f"{', '.join(LICENCE_FILES)}")


def defers_to_this_repository(licence: object) -> bool:
    """Whether this `licence` answer points at the repository's own rather than naming one.

    Compared without case, invisible characters or surrounding space, because the rule was an exact
    string match and `"Same as this repository"` therefore cleared a repository with no licence file
    at all — a sentinel one capital letter wide. `projection` has the same shape of problem and
    solves it with a closed set; a licence cannot have one, since any licence in the world is a
    legitimate answer, so the deferral is recognised loosely instead and everything else is prose.

    Not monotone, which an earlier sentence here claimed it was. Collapsing runs of space makes
    more strings the sentinel; *deleting* an invisible character can go the other way, since the
    space two words needed may be the thing deleted — `same as this<ZWSP>repository` is not the
    deferral, and a test pins that it must not be, because a reader sees the joined word too. What
    holds in both directions is the consequence: a record that misses the sentinel is refused by
    the rule in `check` for having nothing behind it, so an escape costs a confusing refusal rather
    than a clean record with no licence.
    """
    if not isinstance(licence, str):
        return False
    # `split()` rather than `strip()`, because it splits on *every* unicode space and rejoins with
    # ordinary ones — reaching the inside of the string, which round 12's `strip()` did not.
    return normalised(licence) == DEFERS_TO_THIS_REPOSITORY


def normalised(licence: str) -> str:
    """This `licence` answer with everything the sentinel does not care about taken out."""
    return " ".join(visible(licence).split()).casefold()


def nearly_defers_to_this_repository(licence: object) -> bool:
    """Whether this `licence` means the deferral to a reader without being the token.

    Asked separately from `defers_to_this_repository` because the two want different answers: one
    decides whether a licence file must exist, and this one decides whether to refuse a record that
    nobody can act on either way.
    """
    if not isinstance(licence, str) or defers_to_this_repository(licence):
        return False
    return NEARLY_DEFERS in normalised(licence)



@dataclass(frozen=True)
class Problem:
    where: str
    text: str

    def __str__(self) -> str:
        return f"{self.where}: {self.text}"


def digest(path: Path) -> str:
    """The file's sha256, read a block at a time.

    A panorama is the small case; this walks whatever is committed, and a record can name a file of
    any size. Holding it whole to hash it costs its length in memory for no reason — `hashlib` is
    incremental and the loop is two lines.
    """
    running = hashlib.sha256()
    for chunk in blocks(path):
        running.update(chunk)
    return running.hexdigest()


def records(root: Path) -> list[Path]:
    """Every asset directory's record, as absolute paths."""
    return [root / name for name in tracked_files(root) if Path(name).name == RECORD]


# Fields whose answer is a number rather than a sentence, and the numeric half of the same rule
# `READ_BY_A_PROGRAM` states: a reader adding `frames` or `channels` belongs here rather than there.
# Everything else a record holds is prose — spelled as one string or as a list of lines, which is
# how the long answers (`licence_evidence`, `notes`) are written, so a rule that refused lists would
# refuse the tree this ships with. The exception is `projection`, a token from `PROJECTIONS`,
# because a test branches on it rather than a person reading it.
COUNTS = ("bytes", "width", "height")

# Of those, the ones a zero is not an answer to. A frame nought pixels wide is not a raster, and
# `>= 0` accepted it — which matters because `width` and `height` are the two facts only a decoder
# can check, so the arm that does not need one must not be the looser of the two. `bytes` keeps
# zero: an empty file is a real file, and the size check compares it against `stat`.
POSITIVE = ("width", "height")


def unusable(field: str, value: object) -> str | None:
    """Why this field does not answer its question, or None if it does.

    A type rather than a truthiness test, because JSON has more empty shapes than `None` and the
    blank string: a record whose `licence` is `false`, whose `source_path` is `[]` and whose
    `retrieved` is `{}` passed every check this file makes, and `"bytes": true` satisfied the size
    comparison for a one-byte file because `True == 1` in Python. This repository has already paid
    for that once, in a `truth.json` whose quaternion was four `false`s.
    """
    if value is None:
        return "is missing"
    if field in COUNTS:
        # `bool` first: it is a subclass of `int`, and `True` is the value that made the size
        # comparison agree with itself.
        least = 1 if field in POSITIVE else 0
        if isinstance(value, bool) or not isinstance(value, int) or value < least:
            what = ("a size is a whole number of bytes" if field == "bytes"
                    else f"{field} is a whole number of pixels, and a raster has at least one")
            return f"is {value!r}, and {what}"
        return None
    if field in READ_BY_A_PROGRAM and not isinstance(value, str):
        # Before the prose rule, and by type rather than by shape: a program reads this, so a list
        # of lines is as useless to it as a `False`, and each of them slipped past a check that
        # only asked lists to be lists of prose.
        return f"is {value!r}, and this one is read by a program, so it is a single string"
    if isinstance(value, list):
        # A blank line inside the list is a paragraph break — that is how the long answers in this
        # tree are written — so the rule is about the list as a whole rather than each line: every
        # element is prose, and at least one of them says something.
        for line in value:
            if not isinstance(line, str):
                return f"holds {line!r}, and this answers a question a reader asks in words"
        if not any(legible(line) for line in value):
            # `legible` rather than `strip()`, which is the same rule the single-string arm below
            # applies — and applying it to only one of the pair is how the zero-width space reached
            # this file in the first place. `["\u200b"]` is a list of nothing.
            return "is a list of nothing, which answers nothing"
        return None
    if not isinstance(value, str):
        return f"is {value!r}, and this answers a question a reader asks in words"
    if not legible(value):
        return "is blank"
    return None


def legible(value: str) -> bool:
    """Whether this string has a character in it that puts something on the page.

    `strip()` is not the test. It removes whitespace, and the characters that render as nothing
    without being whitespace — the format characters, the Hangul fillers, the braille blank — are
    not, so a field answered with a single U+200B survived every check here and read as blank to
    every human who would ever open the file.

    What it claims is what `visible` implements and no more: this is not a promise that the answer
    renders as something in a reader's font, which nothing here can know.
    """
    return bool(visible(value).strip())


def why_asset(path: Path) -> str | None:
    """Why this file is somebody's work rather than somebody's source, or None if it is not.

    Two independent reasons, because neither alone is right. An extension in `MEDIA` is an asset
    even when it decodes — an SVG is text and is still a picture. And bytes that are not valid
    UTF-8 cannot be source in this repository, whatever they are called.

    The reason is carried rather than discarded so the refusal can name it. A Latin-1 `README.md`
    trips the second rule, and being told that a markdown file "cannot say where it came from" sends
    somebody looking for a licence when the answer is to save it as UTF-8.
    """
    if path.suffix.lower() in MEDIA:
        return f"a {path.suffix.lower()} file is somebody's work"
    try:
        # **Decoded a block at a time, never held whole.** This read the entire file into memory to
        # ask one yes-or-no question about it, over every tracked file that is not a media name —
        # and `--others` means untracked ones too, so a 544 MiB scratch file nobody committed took
        # peak memory from 32 MiB to 1105 MiB, to be told nothing about it. An incremental decoder
        # answers the same question exactly: it holds a partial multi-byte sequence across a block
        # boundary, so a character split between two reads is not mistaken for invalid UTF-8, which
        # is the reason this cannot simply be `chunk.decode()` in a loop.
        decoder = codecs.getincrementaldecoder("utf-8")()
        for chunk in blocks(path):
            decoder.decode(chunk)
        decoder.decode(b"", final=True)
    except UnicodeDecodeError:
        return "its bytes are not valid UTF-8, so it is not source in this repository"
    except OSError as failure:
        # A file that cannot be read is not thereby source. Answering "not an asset" here would let
        # an unreadable file out of the check entirely, which is the one answer that cannot be
        # right about a file nobody can read.
        return f"it could not be read at all ({failure}), so nothing can say what it is"
    return None


def owner_of(name: str, directories: list[str]) -> str | None:
    """The nearest record directory containing `name`, or None if no record does.

    Nearest rather than any, because a record inside another one owns its own subtree: without this
    the outer record reports every file under the inner one as unrecorded and the inner record's
    own files can never satisfy both.
    """
    best: str | None = None
    for prefix in directories:
        if not name.startswith(prefix):
            continue
        if best is None or len(prefix) > len(best):
            best = prefix
    return best


def check(root: Path) -> list[Problem]:
    root = Path(root)
    listed = tracked_files(root)
    # The other question, asked once for the same reason the first is: what a reader's checkout
    # would contain, which is not what this working tree contains. Only the deferral needs it.
    indexed = indexed_files(root)
    # Resolved once. Every deferring entry asks the same question about the same file, and the
    # answer cannot change while this runs.
    trouble = licence_trouble(root, indexed)
    problems: list[Problem] = []

    found = records(root)
    prefixes = []
    for record in found:
        relative = record.parent.relative_to(root).as_posix()
        prefixes.append("" if relative == "." else f"{relative}/")

    for record, prefix in zip(found, prefixes):
        directory = record.parent
        rel = record.relative_to(root).as_posix()

        # **A record that is not in the index accounts for nothing in anybody else's checkout.**
        # `records` asks `tracked_files`, which is right — a record you have written and not yet
        # added is one this run must read, and waiting for `git add` to notice it would mean
        # reporting after the push. But *ownership* is a claim about the committed tree: an
        # untracked `sources.json` beside a tracked asset cleared the build here and failed in a
        # fresh clone of the same commit, which is exactly the split `indexed_files` was introduced
        # to close for the licence, reappearing at the record itself.
        #
        # Reported rather than skipped, for the same reason: skipping it would report the assets it
        # covers as unrecorded, which sends a reader to write a record that already exists.
        if rel not in indexed:
            problems.append(Problem(rel, "is not in the index, so a checkout of this commit would "
                                         "not have it and the files it accounts for would be "
                                         "unrecorded there — `git add` it"))

        try:
            document = json.loads(read_record(record))
        # Every way a record can fail to be a record, not only the one that has a named exception.
        # `read_text` decodes before `json.loads` sees anything, so bytes that are not UTF-8 raise
        # `UnicodeDecodeError`; and valid JSON that is a list or a string has no `.get`, so the
        # shape is checked rather than assumed. Each of those used to come out of this checker as a
        # traceback, which is a build failure that says the checker is broken rather than the record.
        # `RecursionError` because `json.loads` raises it on deeply nested input — 2000 unclosed
        # brackets is enough — and it is a `RuntimeError`, so it walked straight past an arm that
        # names only `OSError` and `ValueError`. A record is somebody's hand-edited file, and the
        # promise here is a sentence naming it whatever it holds.
        except (OSError, ValueError, RecursionError) as failure:
            problems.append(Problem(rel, f"could not be read as JSON: {failure}"))
            continue
        if not isinstance(document, dict):
            problems.append(Problem(rel, f"could not be read as JSON: it is a "
                                         f"{type(document).__name__} and a record is an object"))
            continue

        # Both lists are shape-checked before anything is read out of them. A record spelling
        # `"ours"` as an object keyed by filename gets a sentence from here and, downstream, a bare
        # `TypeError: can only concatenate list (not "dict") to list` out of
        # `tools/test_synth_dataset.py` — the checker saying nothing useful and its consumer dying
        # in a line of test code where a sentence should name the record. That is round 10's defect
        # one level up from the fields it was about.
        malformed = False
        for half in ("assets", "ours"):
            held = document.get(half)
            if held is not None and not isinstance(held, list):
                problems.append(Problem(rel, f"`{half}` is {type(held).__name__}, and it is a list "
                                             f"of entries"))
                malformed = True
        if malformed:
            continue
        entries = [(entry, False) for entry in document.get("assets") or []]
        entries += [(entry, True) for entry in document.get("ours") or []]
        if not entries:
            problems.append(Problem(rel, "has no non-empty `assets` or `ours` list, so it "
                                         "records nothing"))
            continue

        recorded: dict[str, dict] = {}
        for position, (entry, ours) in enumerate(entries):
            name = entry.get("file") if isinstance(entry, dict) else None
            if not isinstance(name, str) or not name.strip():
                problems.append(Problem(rel, f"entry {position} names no `file`"))
                continue
            if name in recorded:
                problems.append(Problem(rel, f"{name} is recorded twice, and the two entries "
                                             f"cannot both describe it"))
                continue
            recorded[name] = entry

            # **`file` names a file in this record's own directory, and nothing else.** It is
            # joined to that directory and then hashed, and `directory / name` happily accepts
            # `../../etc/hostname` or an absolute path, while `is_file()` and `digest()` follow
            # symlinks — so a record could clear the build by accounting for a file it does not
            # own, one outside the repository, or one git ignores. The checker would report the
            # digest of something nobody committed and call the directory accounted for.
            #
            # Checked before the fields, because every later question is about the file this names.
            here = directory / name
            escaped = None
            try:
                if Path(name).is_absolute() or ".." in Path(name).parts:
                    escaped = "names a path outside the record's own directory"
                elif here.is_symlink():
                    escaped = ("is a symlink, and what a record accounts for is the bytes it sits "
                               "beside")
                elif here.exists() and not here.resolve().is_relative_to(directory.resolve()):
                    escaped = "resolves outside the record's own directory"
            except OSError as refused:
                # `Path.is_symlink()` swallows `ENOENT`, `ENOTDIR`, `EBADF` and `ELOOP` and nothing
                # else, so a `file` whose last component reaches `NAME_MAX` — 255 bytes here, a
                # mount option elsewhere — came back out of `check()` as `OSError: [Errno 36]`. A
                # traceback is the one answer this module's docstring says it will not give: a
                # record a reader could have corrected instead named a line of checker source.
                #
                # Caught on the error rather than measured against a length, because the limit is
                # the filesystem's to state and the other ways of being unaskable — a path too long
                # in total, a permission the walk cannot pass — arrive here by the same door.
                #
                # **A NUL in the name arrives by none of them, and is not refused either.** Measured
                # on 3.11: `is_symlink`, `exists` and `is_file` all swallow the `ValueError` and
                # answer `False`, so nothing raises and nothing is caught; `legible("a\0b.jpg")` is
                # `True`, so the prose rule passes it; and `resolve()`, the one call that does
                # raise, is unreachable behind a `False` from `exists()`. Such a record ends up at
                # "names a file that is not here", which is a true sentence reached for none of the
                # reasons above it.
                #
                # This is the third version of this comment. The first claimed the NUL arrived here;
                # the second claimed `pathlib` raised and the prose rule refused it — a correction
                # that replaced one unmeasured claim with two more. The rule that follows from that
                # is the whole of why this paragraph is long: a sentence about what some *other*
                # code does is worth measuring before it is written down, and worth deleting rather
                # than rewriting when it is wrong twice.
                escaped = f"cannot be asked about: {refused.strerror}"
            if escaped is not None:
                problems.append(Problem(f"{rel} [{name}]", f"`file` {escaped}"))
                continue

            required = REQUIRED_OURS if ours else REQUIRED
            for field in required:
                wrong = unusable(field, entry.get(field))
                if wrong is not None:
                    problems.append(Problem(f"{rel} [{name}]", f"`{field}` {wrong}"))

            # And every other key the entry actually holds. The required list says which questions
            # must be answered; it did not say that an answer volunteered to a question nobody asked
            # has to be an answer, so the whole optional half of the schema was exempt from the rule
            # the required half exists to enforce. `produced_by` is the one that bites: blank, it is
            # carried, never run, and the record reads as though its command still reproduced the
            # bytes.
            for field in entry:
                if field in required:
                    continue
                wrong = unusable(field, entry[field])
                if wrong is not None:
                    problems.append(Problem(f"{rel} [{name}]", f"`{field}` {wrong}"))

            # A licence that defers has to have something to defer to. Per entry rather than once,
            # so the refusal names the record a reader would go and correct.
            #
            # `indexed_files` and not `listed`, which is the walk's own `tracked_files`. The two
            # differ by `--others`: a LICENSE that exists only in the working tree is something
            # this repository *can* commit and not something a reader's checkout has, and this
            # question is about the reader's checkout. Asking the wider one answered the deferral
            # from an untracked file — and the whole test suite's fixture was such a file, so every
            # green deferral test was green for that reason.
            #
            # `is_file()` before `stat()`, because the index answers about itself and not the disk:
            # a LICENSE that is tracked and then deleted — or a tracked dangling symlink — made this
            # raise `FileNotFoundError` out of the checker, which is the traceback-instead-of-a-
            # sentence outcome this file's own docstring exists to prevent.
            # **Our own work says its licence in one of two ways, and both are checkable.** The
            # exact deferral, which must resolve below — or a name with a `licence_url` beside it,
            # which is the bar every `assets` entry already meets, `licence_url` being in `REQUIRED`.
            #
            # A closed rule where the four rounds before it wrote open ones. Each of those widened
            # the *recognition* of a deferral and the next round found a spelling it missed; this
            # one stops trying to recognise a deferral loosely and asks instead what a record has
            # behind it. Prose that points at this repository is refused whether or not
            # `NEARLY_DEFERS` can guess what it meant.
            #
            # `ours` only, and not for tidiness: an `assets` entry is somebody else's licence and
            # already carries a URL, so a rule about licences with nothing behind them cannot fire
            # on one. The deferral is about *our* work, which is the half that had no second field.
            licence = entry.get("licence")
            if (ours and isinstance(licence, str) and not defers_to_this_repository(licence)
                    and unusable("licence_url", entry.get("licence_url")) is not None):
                intent = (f" — this reads as the deferral, which is spelled exactly "
                          f"{DEFERS_TO_THIS_REPOSITORY!r}"
                          if nearly_defers_to_this_repository(licence) else "")
                problems.append(Problem(
                    f"{rel} [{name}]",
                    f"`licence` is {licence!r}, and our own work either says exactly "
                    f"{DEFERS_TO_THIS_REPOSITORY!r} or names a licence with a `licence_url` "
                    f"beside it{intent}"))
            if defers_to_this_repository(entry.get("licence")) and trouble is not None:
                problems.append(Problem(
                    f"{rel} [{name}]",
                    f"`licence` is {DEFERS_TO_THIS_REPOSITORY!r} and {trouble}"))

            projection = entry.get("projection")
            if projection is not None and projection not in PROJECTIONS:
                problems.append(Problem(f"{rel} [{name}]",
                                        f"`projection` is {projection!r}, and the projections this "
                                        f"repository knows are {', '.join(PROJECTIONS)}"))

            if Path(name).suffix.lower() in SHAPED:
                for field in ("width", "height"):
                    if field not in entry:
                        problems.append(Problem(f"{rel} [{name}]",
                                                f"`{field}` is missing, and a raster has one"))

            # Guarded, like every other `is_file()` in this file. It was deleted one round ago as
            # unreachable — the `file` check above wraps `is_symlink()` and `exists()` in a `try`
            # and `continue`s on an `OSError`, so by here the path is one this filesystem answered
            # about, and `is_file()` is the same syscall. Every clause of that is true and the
            # conclusion does not follow: the two calls ask about the disk, and several fields are
            # checked between them. The syscall cannot newly refuse; the *file* can.
            #
            # Measured this time, which the deletion was not. The removal race really is not it —
            # `stat` swallows `ENOENT`, `ENOTDIR`, `EBADF` and `ELOOP` — but a regular file replaced
            # by a symlink to a 300-byte name raises `ENAMETOOLONG`, which is in no ignore set, and
            # at the `file` rule a moment earlier it was an ordinary file that nothing refused. A
            # concurrent build step or a parallel checkout is all it takes.
            #
            # The rule that an unreachable guard goes still stands. What went wrong is the step
            # before it: "I could not construct an input" was read as "there is none", by a
            # reviewer and then by me, about a window neither of us had tried to drive.
            path = directory / name
            try:
                present = path.is_file()
            except OSError as refused:
                problems.append(Problem(f"{rel} [{name}]",
                                        f"cannot be asked about: {refused.strerror}"))
                continue
            if not present:
                problems.append(Problem(f"{rel} [{name}]", "names a file that is not here"))
                continue
            # `digest`, not a third spelling of it: this used to inline `hashlib` here while the
            # test suite called the helper, so the two paths computed the same thing two ways. And
            # the size comes from `stat`, where it has always been: reading the file a second time
            # to call `len` on it was the whole of what `content` was for.
            # **Everything that opens the file, under one guard.** The `file` guard above catches a
            # name that cannot be asked about; this catches a file that can be named and not read —
            # a mode-000 asset, a permission the walk cannot pass, a file deleted between `is_file`
            # and here. Each of those came out of `check()` as a `PermissionError` or
            # `FileNotFoundError` traceback: the one answer this module's docstring says it will not
            # give, about a record whose problem a reader could have fixed.
            #
            # Round 14 wrapped the *first* of these calls and left the three after it bare, which is
            # the one-of-a-pair shape this branch has now produced six times.
            try:
                held = digest(path)
                size = path.stat().st_size
                # Only where a record carries one: this walks every committed asset, and hashing
                # each of them a second time for a field most entries do not have is a cost with no
                # reader. Inside the guard rather than after it because it opens the same file.
                recorded_blob = entry.get("source_blob")
                wants_blob = isinstance(recorded_blob, str) and bool(recorded_blob.strip())
                actual_blob = git_blob(path) if wants_blob else ""
            except OSError as refused:
                problems.append(Problem(f"{rel} [{name}]",
                                        f"cannot be read: {refused.strerror}"))
                continue
            recorded_digest = entry.get("sha256")
            if isinstance(recorded_digest, str) and recorded_digest.casefold() == held:
                # A digest spelled in upper case is the same digest. Answered separately because
                # the other message says *the bytes have moved on*, and a reader acts on that by
                # re-hashing a file that was never wrong — the misdirection `why_asset` carries its
                # own reason to avoid. One spelling, so the recorded value can be compared to
                # `digest()` and to `git hash-object` without a rule about case at each site.
                if recorded_digest != held:
                    problems.append(Problem(f"{rel} [{name}]",
                                            "records the right digest in the wrong case; a sha256 "
                                            "is written in lower case here"))
            elif recorded_digest != held:
                problems.append(Problem(f"{rel} [{name}]",
                                        f"has moved on from its recorded sha256; the bytes now "
                                        f"hash to {held}"))
            if entry.get("bytes") != size:
                problems.append(Problem(f"{rel} [{name}]",
                                        f"records {entry.get('bytes')} bytes and holds {size}"))

            # The upstream git object name, where a record carries one. Optional, and checked when
            # present for the same reason `sha256` is: a fact nobody derives is a fact that goes
            # quietly stale, and this one goes stale on exactly the change ADR 0059 forbids.
            #
            # **Last, and it says what to do rather than what it saw.** All three of these fire for
            # one cause, and this one used to print first, handing the reader a forty-hex value
            # whose only effect when pasted in is to turn an upstream fact into a local one that can
            # never be checked against anything again. `sha256` and `bytes` invite exactly that
            # paste and are right to; this field never does, because the local object name equals
            # the recorded one precisely when the check did not fire.
            if wants_blob:
                if actual_blob != recorded_blob:
                    problems.append(Problem(
                        f"{rel} [{name}]",
                        f"`source_blob` names object {recorded_blob} in the source repository and "
                        f"these are not those bytes — if they were re-encoded, ADR 0059 forbids "
                        f"that; if the file was legitimately replaced, re-record `source_blob` "
                        f"from upstream rather than from here"))

        for name in listed:
            if owner_of(name, prefixes) != prefix:
                continue
            tail = name[len(prefix):]
            if tail in recorded:
                continue
            path = root / name
            # Guarded like the entry walk above and for the same reason: `is_file()` raises on a
            # path this filesystem cannot answer about — an unreadable parent, a name past
            # `NAME_MAX` — and git will happily list one. A traceback here says the checker is
            # broken rather than naming the file, which is what this module promises not to do.
            try:
                why = why_asset(path) if path.is_file() else None
            except OSError as refused:
                problems.append(Problem(rel, f"{tail} cannot be read: {refused.strerror}"))
                continue
            if why is not None:
                # The reason, here as well as in the loop below. `why_asset` carries it precisely so
                # a refusal can name it — and this call site threw it away, so a Latin-1 `.md` inside
                # a recorded directory was told it "is recorded nowhere" and sent its author looking
                # for a licence, which is the misdirection that docstring exists to prevent.
                problems.append(Problem(rel, f"{tail} is here and is recorded nowhere: {why}"))

    for name in listed:
        if owner_of(name, prefixes) is not None:
            continue
        path = root / name
        try:
            if not path.is_file():
                continue
        except OSError as refused:
            problems.append(Problem(name, f"cannot be read: {refused.strerror}"))
            continue
        why = why_asset(path)
        if why is None:
            continue
        problems.append(Problem(name, f"{why}, and it is in no directory with a {RECORD} that "
                                      f"names it"))

    return problems


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
        problems = check(root)
    except RuntimeError as refused:
        print(f"this check could not run: {refused}", file=sys.stderr)
        return 1
    if not problems:
        return 0
    print(f"{len(problems)} asset(s) cannot say where they came from:\n", file=sys.stderr)
    for problem in problems:
        print(f"  {problem}", file=sys.stderr)
    print("\nRecord it in the directory's sources.json, or take the file out.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
