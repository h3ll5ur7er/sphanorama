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

**A field a test branches on is a token, not a sentence.** `projection` is the only one so far: an
entry recording it must use a value from `PROJECTIONS`, because `tools/test_synth_dataset.py` asserts
the 2:1 rule on an entry claiming to be equirectangular — and while one record spelled that field as
a description, the assertion keyed on it was dead for every record in the tree with nothing able to
notice. Everything else a record holds is prose and is judged only for being an answer at all.

Usage:  uv run tools/asset_provenance.py [repo_root]
"""
from __future__ import annotations

import codecs
import hashlib
import json
import sys
from dataclasses import dataclass
from pathlib import Path

from tracked import tracked_files

RECORD = "sources.json"

# One block of a file at a time, for the digest and for the decode. Big enough that the
# syscall count does not matter and small enough that the peak does not depend on what
# somebody left in the working tree.
BLOCK = 1024 * 1024

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
MEDIA = (".jpg", ".jpeg", ".png", ".gif", ".webp", ".avif", ".bmp", ".ico", ".tiff", ".svg",
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
SHAPED = (".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp", ".tiff", ".ppm", ".pgm", ".pnm")

# `projection` is read by a test rather than by a person: `tools/test_synth_dataset.py` asserts the
# 2:1 rule on an entry that claims to be equirectangular. So it is a token from a closed set, and an
# unrecognised one is refused here — while this field said "equirectangular, 360 by 180 degrees" the
# assertion keyed on it was dead for every record in the tree, and nothing could see that. A
# description of the projection belongs in `notes`, which nothing branches on.
PROJECTIONS = ("equirectangular",)



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
    with path.open("rb") as handle:
        while chunk := handle.read(BLOCK):
            running.update(chunk)
    return running.hexdigest()


def records(root: Path) -> list[Path]:
    """Every asset directory's record, as absolute paths."""
    return [root / name for name in tracked_files(root) if Path(name).name == RECORD]


# Fields whose answer is a number rather than a sentence. Everything else a record holds is prose —
# spelled as one string or as a list of lines, which is how the long answers (`licence_evidence`,
# `notes`) are written, so a rule that refused lists would refuse the tree this ships with — with the
# one exception twenty lines above: `projection` is a token from `PROJECTIONS`, because a test
# branches on it rather than a person reading it.
COUNTS = ("bytes", "width", "height")


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
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            what = ("a size is a whole number of bytes" if field == "bytes"
                    else f"{field} is a whole number of pixels")
            return f"is {value!r}, and {what}"
        return None
    if isinstance(value, list):
        # A blank line inside the list is a paragraph break — that is how the long answers in this
        # tree are written — so the rule is about the list as a whole rather than each line: every
        # element is prose, and at least one of them says something.
        for line in value:
            if not isinstance(line, str):
                return f"holds {line!r}, and this answers a question a reader asks in words"
        if not any(line.strip() for line in value):
            return "is a list of nothing, which answers nothing"
        return None
    if not isinstance(value, str):
        return f"is {value!r}, and this answers a question a reader asks in words"
    if not value.strip():
        return "is blank"
    return None


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
        with path.open("rb") as handle:
            while chunk := handle.read(BLOCK):
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
    problems: list[Problem] = []

    found = records(root)
    prefixes = []
    for record in found:
        relative = record.parent.relative_to(root).as_posix()
        prefixes.append("" if relative == "." else f"{relative}/")

    for record, prefix in zip(found, prefixes):
        directory = record.parent
        rel = record.relative_to(root).as_posix()
        try:
            document = json.loads(record.read_text())
        # Every way a record can fail to be a record, not only the one that has a named exception.
        # `read_text` decodes before `json.loads` sees anything, so bytes that are not UTF-8 raise
        # `UnicodeDecodeError`; and valid JSON that is a list or a string has no `.get`, so the
        # shape is checked rather than assumed. Each of those used to come out of this checker as a
        # traceback, which is a build failure that says the checker is broken rather than the record.
        except (OSError, ValueError) as failure:
            problems.append(Problem(rel, f"could not be read as JSON: {failure}"))
            continue
        if not isinstance(document, dict):
            problems.append(Problem(rel, f"could not be read as JSON: it is a "
                                         f"{type(document).__name__} and a record is an object"))
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

            path = directory / name
            if not path.is_file():
                problems.append(Problem(f"{rel} [{name}]", "names a file that is not here"))
                continue
            # `digest`, not a third spelling of it: this used to inline `hashlib` here while the
            # test suite called the helper, so the two paths computed the same thing two ways. And
            # the size comes from `stat`, where it has always been: reading the file a second time
            # to call `len` on it was the whole of what `content` was for.
            held = digest(path)
            if entry.get("sha256") != held:
                problems.append(Problem(f"{rel} [{name}]",
                                        f"has moved on from its recorded sha256; the bytes now "
                                        f"hash to {held}"))
            size = path.stat().st_size
            if entry.get("bytes") != size:
                problems.append(Problem(f"{rel} [{name}]",
                                        f"records {entry.get('bytes')} bytes and holds {size}"))

        for name in listed:
            if owner_of(name, prefixes) != prefix:
                continue
            tail = name[len(prefix):]
            if tail in recorded:
                continue
            path = root / name
            why = why_asset(path) if path.is_file() else None
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
        if not path.is_file():
            continue
        why = why_asset(path)
        if why is None:
            continue
        problems.append(Problem(name, f"{why}, and it is in no directory with a {RECORD} that "
                                      f"names it"))

    return problems


def main(argv: list[str]) -> int:
    root = Path(argv[1]) if len(argv) > 1 else Path(__file__).resolve().parent.parent
    problems = check(root)
    if not problems:
        return 0
    print(f"{len(problems)} asset(s) cannot say where they came from:\n", file=sys.stderr)
    for problem in problems:
        print(f"  {problem}", file=sys.stderr)
    print("\nRecord it in the directory's sources.json, or take the file out.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
