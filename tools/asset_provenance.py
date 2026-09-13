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

What this does *not* check is `width` and `height`, which need a decoder and would put an image
library in front of every build. `tools/test_synth_dataset.py` checks them where Pillow is already
present.

Usage:  uv run tools/asset_provenance.py [repo_root]
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

RECORD = "sources.json"

# Each answers a question that cannot be recovered from the bytes. `bytes` and `sha256` can be, and
# are here so that the record is checkable against the file rather than merely present.
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

# The same question the conflict-marker check asks, for the same reason: tracked files plus
# untracked ones git is not ignoring is exactly the set that can become a commit. It also keeps the
# scan out of node_modules and build/, where a vendored `sources.json` would otherwise be read as
# ours.
LS_FILES = ("git", "ls-files", "-z", "--cached", "--others", "--exclude-standard")


@dataclass(frozen=True)
class Problem:
    where: str
    text: str

    def __str__(self) -> str:
        return f"{self.where}: {self.text}"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tracked_files(root: Path) -> list[str]:
    result = subprocess.run(LS_FILES, cwd=root, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"git could not list this tree: {result.stderr.strip()}")
    return sorted({name for name in result.stdout.split("\0") if name})


def records(root: Path) -> list[Path]:
    """Every asset directory's record, as absolute paths."""
    return [root / name for name in tracked_files(root) if Path(name).name == RECORD]


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
        path.read_bytes().decode()
    except UnicodeDecodeError:
        return "its bytes are not valid UTF-8, so it is not source in this repository"
    except OSError:
        return None
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
        except (OSError, json.JSONDecodeError) as failure:
            problems.append(Problem(rel, f"could not be read as JSON: {failure}"))
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

            for field in (REQUIRED_OURS if ours else REQUIRED):
                value = entry.get(field)
                if value is None or (isinstance(value, str) and not value.strip()):
                    problems.append(Problem(f"{rel} [{name}]", f"`{field}` is missing or blank"))

            path = directory / name
            if not path.is_file():
                problems.append(Problem(f"{rel} [{name}]", "names a file that is not here"))
                continue
            content = path.read_bytes()
            if entry.get("sha256") != hashlib.sha256(content).hexdigest():
                problems.append(Problem(f"{rel} [{name}]",
                                        f"has moved on from its recorded sha256; the bytes now "
                                        f"hash to {hashlib.sha256(content).hexdigest()}"))
            if entry.get("bytes") != len(content):
                problems.append(Problem(f"{rel} [{name}]",
                                        f"records {entry.get('bytes')} bytes and holds "
                                        f"{len(content)}"))

        for name in listed:
            if owner_of(name, prefixes) != prefix:
                continue
            tail = name[len(prefix):]
            if tail == RECORD or tail in recorded:
                continue
            path = root / name
            if path.is_file() and why_asset(path) is not None:
                problems.append(Problem(rel, f"{tail} is here and is recorded nowhere"))

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
