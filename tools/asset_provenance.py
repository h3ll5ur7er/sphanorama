#!/usr/bin/env python3
"""Fail the build if a file in the repository cannot say where it came from.

Source is self-describing and binaries are not. A `.jpg` that appears in a commit carries no author,
no licence and no origin, and six months later nobody can answer any of the three — at which point
the only safe move is to delete it and whatever was measured against it. This repository commits a
panorama because Phase 2's accuracy number has to be measured in a photographed world rather than a
checkerboard, so the answer is a record beside the bytes and a check that keeps the two together.

Two rules, and the second is the one that makes the first mean anything:

**A directory holding `sources.json` is an asset directory, and every file in it must have an
entry.** Something fetched from elsewhere goes under `assets` and names the work, its author, its
licence and where the bytes came from — digest included, so a file swapped later cannot inherit the
clearance of the one it replaced. Something this repository produced goes under `generated` and
names the command that produces it; no digest, because the bytes of a generated file are pinned by
the test that regenerates them and a second copy here would only be a second place to update.

**A tracked file that is not text has to be in one of those directories.** Without this the check is
opted into by the person it exists to catch: a `.jpg` dropped anywhere else would be invisible while
the build step above it claims every committed asset says where it came from. Text is exempt because
source explains itself and a repository is mostly source; what cannot be read is what nobody can
account for later.

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

# A file this repository produced needs the command and nothing else: its licence is the
# repository's, and its bytes are pinned by whatever test regenerates it.
REQUIRED_GENERATED = ("file", "produced_by")

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


def is_text(path: Path) -> bool:
    """Whether the bytes read as UTF-8, which is this checker's whole definition of source."""
    try:
        path.read_bytes().decode()
    except (UnicodeDecodeError, OSError):
        return False
    return True


def check(root: Path) -> list[Problem]:
    root = Path(root)
    listed = tracked_files(root)
    problems: list[Problem] = []
    accounted: set[str] = set()

    for record in records(root):
        directory = record.parent
        rel = record.relative_to(root).as_posix()
        try:
            document = json.loads(record.read_text())
        except (OSError, json.JSONDecodeError) as failure:
            problems.append(Problem(rel, f"could not be read as JSON: {failure}"))
            continue

        entries = [(entry, False) for entry in document.get("assets") or []]
        entries += [(entry, True) for entry in document.get("generated") or []]
        if not entries:
            problems.append(Problem(rel, "has no non-empty `assets` or `generated` list, so it "
                                         "records nothing"))
            continue

        recorded: dict[str, dict] = {}
        for position, (entry, generated) in enumerate(entries):
            name = entry.get("file") if isinstance(entry, dict) else None
            if not isinstance(name, str) or not name.strip():
                problems.append(Problem(rel, f"entry {position} names no `file`"))
                continue
            if name in recorded:
                problems.append(Problem(rel, f"{name} is recorded twice, and the two entries "
                                             f"cannot both describe it"))
                continue
            recorded[name] = entry

            for field in (REQUIRED_GENERATED if generated else REQUIRED):
                value = entry.get(field)
                if value is None or (isinstance(value, str) and not value.strip()):
                    problems.append(Problem(f"{rel} [{name}]", f"`{field}` is missing or blank"))

            path = directory / name
            if not path.is_file():
                problems.append(Problem(f"{rel} [{name}]", "names a file that is not here"))
                continue
            if generated:
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

        prefix = directory.relative_to(root).as_posix()
        prefix = f"{prefix}/" if prefix != "." else ""
        # Every file the record is responsible for, which is everything beneath its directory and
        # not only its immediate children: a folder is not a way to slip a file past this.
        for name in listed:
            if not name.startswith(prefix):
                continue
            tail = name[len(prefix):]
            accounted.add(name)
            if tail != RECORD and tail not in recorded:
                problems.append(Problem(rel, f"{tail} is here and is recorded nowhere"))

    for name in listed:
        if name in accounted:
            continue
        path = root / name
        if not path.is_file() or is_text(path):
            continue
        problems.append(Problem(name, "is not text and is in no directory with a "
                                      f"{RECORD}, so nothing here can say where it came from"))

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
